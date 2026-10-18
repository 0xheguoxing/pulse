#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace pulse::ops {

class TransferRateEstimator {
public:
    // Byte transfers only trust rates above 1 B/s (below that a countdown is noise);
    // item counts run far slower, so they drop the floor to keep their ETA alive.
    void SetEtaFloor(double floor) { eta_floor_ = floor; }

    void Reset(ULONGLONG tick, uint64_t bytes) {
        samples_.clear();
        samples_.push_back({ tick, bytes });
        smoothed_speed_ = 0.0;
        eta_seconds_ = 0;
        last_rate_tick_ = tick;
        last_eta_tick_ = tick;
        first_eta_tick_ = 0;
    }

    void Observe(ULONGLONG tick, uint64_t bytes, uint64_t total_bytes) {
        if (samples_.empty() || bytes < samples_.back().bytes) {
            Reset(tick, bytes);
            return;
        }

        constexpr ULONGLONG kMinimumSampleMs = 100;
        constexpr ULONGLONG kWindowMs = 4000;
        constexpr ULONGLONG kWarmupMs = 750;
        if (tick - samples_.back().tick < kMinimumSampleMs && bytes < total_bytes)
            return;

        if (tick == samples_.back().tick) {
            samples_.back().bytes = bytes;
        } else {
            samples_.push_back({ tick, bytes });
        }
        while (samples_.size() > 2 && samples_[1].tick + kWindowMs <= tick)
            samples_.pop_front();

        const ULONGLONG span = tick - samples_.front().tick;
        const uint64_t byte_delta = bytes - samples_.front().bytes;
        if (span < kWarmupMs || byte_delta == 0) return;

        const double window_speed = static_cast<double>(byte_delta) * 1000.0
            / static_cast<double>(span);
        if (smoothed_speed_ <= 0.0) {
            smoothed_speed_ = window_speed;
        } else {
            const double seconds = static_cast<double>(tick - last_rate_tick_) / 1000.0;
            const double alpha = 1.0 - std::exp(-seconds);
            smoothed_speed_ += alpha * (window_speed - smoothed_speed_);
        }
        last_rate_tick_ = tick;

        if (bytes >= total_bytes || smoothed_speed_ <= eta_floor_) {
            eta_seconds_ = 0;
            return;
        }
        if (eta_seconds_ != 0 && tick - last_eta_tick_ < 1000) return;

        // The first seconds start cold (file cache, a slow first file): the
        // 4 s window and the smoothing both remember that, and the estimate
        // read several times too long. While settling, estimate from the
        // last second alone and follow it; after that, smooth as before.
        const bool settling = first_eta_tick_ == 0 || tick - first_eta_tick_ < kSettleMs;
        double eta_speed = smoothed_speed_;
        if (settling) {
            size_t from = samples_.size() - 1;
            while (from > 0 && samples_[from].tick + kRecentMs > tick) --from;
            const ULONGLONG recent_span = tick - samples_[from].tick;
            if (recent_span > 0 && bytes > samples_[from].bytes)
                eta_speed = static_cast<double>(bytes - samples_[from].bytes) * 1000.0
                    / static_cast<double>(recent_span);
        }
        if (eta_speed <= eta_floor_) eta_speed = smoothed_speed_;
        const double raw_eta = static_cast<double>(total_bytes - bytes) / eta_speed;
        if (eta_seconds_ == 0) {
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::ceil(raw_eta)));
            if (first_eta_tick_ == 0) first_eta_tick_ = tick;
        } else {
            // React faster to a slowdown than to a transient speed-up.
            const double alpha = settling ? 1.0
                : raw_eta > static_cast<double>(eta_seconds_) ? 0.45 : 0.20;
            const double blended = static_cast<double>(eta_seconds_)
                + alpha * (raw_eta - static_cast<double>(eta_seconds_));
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::llround(blended)));
        }
        last_eta_tick_ = tick;
    }

    double speed() const { return smoothed_speed_; }
    uint64_t eta_seconds() const { return eta_seconds_; }
    static constexpr ULONGLONG kSettleMs = 10000;
    static constexpr ULONGLONG kRecentMs = 1000;

private:
    struct Sample {
        ULONGLONG tick = 0;
        uint64_t bytes = 0;
    };
    std::deque<Sample> samples_;
    double eta_floor_ = 1.0;
    double smoothed_speed_ = 0.0;
    uint64_t eta_seconds_ = 0;
    ULONGLONG last_rate_tick_ = 0;
    ULONGLONG last_eta_tick_ = 0;
    ULONGLONG first_eta_tick_ = 0;
};

} // namespace pulse::ops
