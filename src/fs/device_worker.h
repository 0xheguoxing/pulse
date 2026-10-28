// device_worker.h - The thread every DeviceSource call runs on.
//
// One STA thread, created on demand and gone after `idle_exit_ms` without
// work, so a PC without a phone never has it. A Shell / MTP call cannot be
// interrupted: when a task overruns its budget the thread is abandoned (it
// exits once that call returns, without running anything else) and the
// queued tasks move to a fresh thread, so one stuck phone call never blocks
// the next request. The UI only ever posts and gets results back by message.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>

namespace pulse::fs {

class DeviceWorker {
public:
    explicit DeviceWorker(uint32_t idle_exit_ms = 60000);
    ~DeviceWorker();
    DeviceWorker(const DeviceWorker&) = delete;
    DeviceWorker& operator=(const DeviceWorker&) = delete;

    // Runs `task` on the STA thread, in posting order. With a budget, a task
    // still running after budget_ms abandons its thread and `on_stuck` runs
    // on a pool thread (the task itself keeps running until its call returns).
    void Post(std::function<void()> task, uint32_t budget_ms = 0,
              std::function<void()> on_stuck = {});

    // Asks the live thread to finish its current task and exit; waits up to
    // wait_ms. Threads are never joined: a stuck one cannot hold up exit.
    void Stop(uint32_t wait_ms);

    // Tests / diagnostics.
    bool HasThread() const;
    uint32_t ThreadsStarted() const;
    uint32_t ThreadsAbandoned() const;

    struct Core;

private:
    std::shared_ptr<Core> core_;
};

} // namespace pulse::fs
