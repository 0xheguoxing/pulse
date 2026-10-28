#pragma once
#include <windows.h>
#include "index_diagnostics.h"
#include <psapi.h>
#include <array>
#include <cstdint>
#include <string>

namespace pulse::index {
enum class IndexMemoryPoint : size_t {
    ChangeFlushBefore, ChangeFlushSnapshot, ChangeFlushAfter, MergeBefore, MergeFlattened, TimingFlush,
    // WriteIndexFile stages: the full-merge peak lies after MergeFlattened.
    WritePrefix1, WritePrefix2, WriteSorted, Count
};
// Worker-owned, allocation-free samples at a few maintenance boundaries, not an
// allocator hook or a per-event poll. Process-wide values include other threads.
class IndexMemoryProbe {
public:
    struct Retained {
        uint64_t containers_tick = 0, feed_records = 0, feed_page_capacity_bytes = 0;
        uint64_t history_records = 0, history_vector_capacity_bytes = 0, history_tick = 0;
        uint64_t flush_snapshot_records = 0;
        uint64_t aggregate_mapped_file_bytes = 0, shard_mapped_file_bytes = 0;
        uint64_t overlay_nodes = 0, overlay_patches = 0;
        uint64_t usn_streams = 0;
        uint64_t usn_queue_packets = 0;
        uint64_t usn_queue_payload_bytes = 0;
        uint64_t usn_queue_capacity_bytes = 0;
        uint64_t usn_queue_charged_bytes = 0;
        uint64_t usn_sum_stream_peak_capacity_bytes = 0;
        uint64_t usn_sum_stream_peak_charged_bytes = 0;
        uint64_t usn_queue_overflows = 0;
    } retained;
    struct Sample {
        uint64_t calls = 0, failures = 0, tick = 0;
        uint64_t working_set_bytes = 0, private_commit_bytes = 0;
        uint64_t max_working_set_bytes = 0, max_private_commit_bytes = 0;
        // Process lifetime peak commit (PeakPagefileUsage) when sampled; catches
        // short peaks between sample points.
        uint64_t peak_private_commit_bytes = 0;
    };
    void Capture(IndexMemoryPoint point) noexcept {
        if (!IndexDiagnosticsEnabled()) return;
        auto& sample = samples_[static_cast<size_t>(point)];
        ++sample.calls;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            ++sample.failures; return;
        }
        sample.tick = GetTickCount64();
        sample.working_set_bytes = counters.WorkingSetSize;
        sample.private_commit_bytes = counters.PrivateUsage;
        sample.peak_private_commit_bytes = counters.PeakPagefileUsage;
        if (sample.working_set_bytes > sample.max_working_set_bytes) sample.max_working_set_bytes = sample.working_set_bytes;
        if (sample.private_commit_bytes > sample.max_private_commit_bytes) sample.max_private_commit_bytes = sample.private_commit_bytes;
    }
    const Sample& At(IndexMemoryPoint point) const { return samples_[static_cast<size_t>(point)]; }
    std::string Json() const {
        const auto& r = retained;
        std::string out = "{\"retained\":{\"containers_tick\":" + std::to_string(r.containers_tick) +
            ",\"feed_records\":" + std::to_string(r.feed_records) +
            ",\"feed_page_capacity_bytes\":" + std::to_string(r.feed_page_capacity_bytes) +
            ",\"history_records\":" + std::to_string(r.history_records) +
            ",\"history_vector_capacity_bytes\":" + std::to_string(r.history_vector_capacity_bytes) +
            ",\"history_tick\":" + std::to_string(r.history_tick) +
            ",\"flush_snapshot_records\":" + std::to_string(r.flush_snapshot_records) +
            ",\"aggregate_mapped_file_bytes\":" + std::to_string(r.aggregate_mapped_file_bytes) +
            ",\"shard_mapped_file_bytes\":" + std::to_string(r.shard_mapped_file_bytes) +
            ",\"overlay_nodes\":" + std::to_string(r.overlay_nodes) +
            ",\"overlay_patches\":" + std::to_string(r.overlay_patches) +
            ",\"usn_streams\":" + std::to_string(r.usn_streams) +
            ",\"usn_queue_packets\":" + std::to_string(r.usn_queue_packets) +
            ",\"usn_queue_payload_bytes\":" + std::to_string(r.usn_queue_payload_bytes) +
            ",\"usn_queue_capacity_bytes\":" + std::to_string(r.usn_queue_capacity_bytes) +
            ",\"usn_queue_charged_bytes\":" + std::to_string(r.usn_queue_charged_bytes) +
            ",\"usn_sum_stream_peak_capacity_bytes\":" + std::to_string(r.usn_sum_stream_peak_capacity_bytes) +
            ",\"usn_sum_stream_peak_charged_bytes\":" + std::to_string(r.usn_sum_stream_peak_charged_bytes) +
            ",\"usn_queue_overflows\":" + std::to_string(r.usn_queue_overflows) + "},\"points\":{";
        constexpr const char* names[]{"change_flush_before", "change_flush_snapshot", "change_flush_after",
            "merge_before", "merge_flattened", "timing_flush", "write_prefix1", "write_prefix2",
            "write_sorted"};
        static_assert(std::size(names) == static_cast<size_t>(IndexMemoryPoint::Count));
        for (size_t i = 0; i < samples_.size(); ++i) {
            const auto& s = samples_[i];
            if (i) out += ',';
            out += "\"" + std::string(names[i]) + "\":{\"calls\":" + std::to_string(s.calls) +
                ",\"failures\":" + std::to_string(s.failures) + ",\"tick\":" + std::to_string(s.tick) +
                ",\"working_set_bytes\":" + std::to_string(s.working_set_bytes) +
                ",\"private_commit_bytes\":" + std::to_string(s.private_commit_bytes) +
                ",\"max_working_set_bytes\":" + std::to_string(s.max_working_set_bytes) +
                ",\"max_private_commit_bytes\":" + std::to_string(s.max_private_commit_bytes) +
                ",\"peak_private_commit_bytes\":" + std::to_string(s.peak_private_commit_bytes) + "}";
        }
        return out + "}}";
    }
private:
    std::array<Sample, static_cast<size_t>(IndexMemoryPoint::Count)> samples_{};
};
}
