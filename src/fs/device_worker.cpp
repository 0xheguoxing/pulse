// device_worker.cpp - On-demand STA thread for device calls (device_worker.h).
#include "device_worker.h"

#include <windows.h>
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::fs {

namespace {

struct Task {
    uint64_t seq = 0;
    std::function<void()> run;
};

// One thread's queue. A lane is abandoned when its running task overran its
// budget; its thread then exits after that task without taking more work.
struct Lane {
    std::condition_variable cv;
    std::deque<Task> queue;
    uint64_t running = 0;  // seq of the task in progress, 0 = idle
    bool abandoned = false;
    bool exit_requested = false;
    bool exited = false;
};

} // namespace

struct DeviceWorker::Core {
    std::mutex mutex;
    std::condition_variable exited_cv;
    std::shared_ptr<Lane> live;
    uint32_t idle_exit_ms = 60000;
    uint64_t next_seq = 0;
    uint32_t started = 0;
    uint32_t abandoned = 0;
};

namespace {

void LaneLoop(std::shared_ptr<DeviceWorker::Core> core, std::shared_ptr<Lane> lane) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::unique_lock<std::mutex> lock(core->mutex);
    for (;;) {
        if (lane->abandoned || lane->exit_requested) break;
        if (lane->queue.empty()) {
            const bool woke = lane->cv.wait_for(lock, std::chrono::milliseconds(core->idle_exit_ms), [&] {
                return !lane->queue.empty() || lane->abandoned || lane->exit_requested;
            });
            if (!woke) break;  // idle: the thread goes away until the next post
            continue;
        }
        Task task = std::move(lane->queue.front());
        lane->queue.pop_front();
        lane->running = task.seq;
        lock.unlock();
        if (task.run) task.run();
        task.run = nullptr;  // captured state is released on this thread
        lock.lock();
        lane->running = 0;
    }
    lane->exited = true;
    if (core->live == lane) core->live.reset();
    core->exited_cv.notify_all();
    lock.unlock();
    if (SUCCEEDED(com)) CoUninitialize();
}

// Caller holds core->mutex.
std::shared_ptr<Lane> StartLaneLocked(const std::shared_ptr<DeviceWorker::Core>& core) {
    auto lane = std::make_shared<Lane>();
    core->live = lane;
    ++core->started;
    std::thread(LaneLoop, core, lane).detach();
    return lane;
}

struct BudgetCheck {
    std::weak_ptr<DeviceWorker::Core> core;
    std::weak_ptr<Lane> lane;
    uint64_t seq = 0;
    std::function<void()> on_stuck;
};

void CALLBACK BudgetExpired(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER timer) {
    std::unique_ptr<BudgetCheck> check(static_cast<BudgetCheck*>(context));
    CloseThreadpoolTimer(timer);  // allowed from the callback
    auto core = check->core.lock();
    auto lane = check->lane.lock();
    if (!core || !lane) return;
    bool stuck = false;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if (lane->running == check->seq && !lane->abandoned && !lane->exited) {
            stuck = true;
            lane->abandoned = true;
            ++core->abandoned;
            std::deque<Task> pending = std::move(lane->queue);
            lane->queue.clear();
            if (core->live == lane) core->live.reset();
            if (!pending.empty()) {
                auto fresh = StartLaneLocked(core);
                fresh->queue = std::move(pending);
                fresh->cv.notify_one();
            }
        }
    }
    if (stuck && check->on_stuck) check->on_stuck();
}

} // namespace

DeviceWorker::DeviceWorker(uint32_t idle_exit_ms) : core_(std::make_shared<Core>()) {
    core_->idle_exit_ms = idle_exit_ms;
}

DeviceWorker::~DeviceWorker() { Stop(0); }

void DeviceWorker::Post(std::function<void()> task, uint32_t budget_ms, std::function<void()> on_stuck) {
    std::shared_ptr<Lane> lane;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        seq = ++core_->next_seq;
        lane = core_->live && !core_->live->exited && !core_->live->exit_requested
            ? core_->live : StartLaneLocked(core_);
        lane->queue.push_back({seq, std::move(task)});
        lane->cv.notify_one();
    }
    if (budget_ms == 0) return;
    // The budget counts from the post: a task queued behind a slow one is
    // only abandoned for its own overrun (running == seq when this fires).
    auto check = std::make_unique<BudgetCheck>();
    check->core = core_;
    check->lane = lane;
    check->seq = seq;
    check->on_stuck = std::move(on_stuck);
    PTP_TIMER timer = CreateThreadpoolTimer(BudgetExpired, check.get(), nullptr);
    if (!timer) return;
    check.release();
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(budget_ms) * 10000);
    FILETIME ft{due.LowPart, due.HighPart};
    SetThreadpoolTimer(timer, &ft, 0, 0);
}

void DeviceWorker::Stop(uint32_t wait_ms) {
    std::unique_lock<std::mutex> lock(core_->mutex);
    auto lane = core_->live;
    if (!lane) return;
    lane->exit_requested = true;
    lane->queue.clear();
    lane->cv.notify_one();
    core_->live.reset();
    if (wait_ms)
        core_->exited_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return lane->exited; });
}

bool DeviceWorker::HasThread() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->live && !core_->live->exited;
}

uint32_t DeviceWorker::ThreadsStarted() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->started;
}

uint32_t DeviceWorker::ThreadsAbandoned() const {
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->abandoned;
}

} // namespace pulse::fs
