// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

module;

#include <windows.h>

#include <wil/stl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

export module devicefs.filesystem_measurement;

export class FreeClusterMeasurement {
    static constexpr auto kClustersPerClaimWord =
        sizeof(std::uint64_t) * CHAR_BIT;

    [[nodiscard]] static auto CountFreeClusters(
        const std::span<const unsigned char> bitmap,
        const std::uint64_t cluster_count) noexcept {
        auto allocated_clusters = std::uint64_t{};
        const auto full_bytes = cluster_count / CHAR_BIT;
        for (auto i = 0uz; i < full_bytes; ++i) {
            allocated_clusters += std::popcount(bitmap[i]);
        }
        const auto remaining_bits = cluster_count % CHAR_BIT;
        if (remaining_bits != 0) {
            const auto mask = (1u << remaining_bits) - 1;
            allocated_clusters += std::popcount(bitmap[full_bytes] & mask);
        }
        return cluster_count - allocated_clusters;
    }

    [[nodiscard]] auto IsAllocated(const std::uint64_t cluster) const noexcept {
        if (cluster >= cluster_count_) {
            return true;
        }
        return (bitmap_[cluster / CHAR_BIT] &
            (1u << (cluster % CHAR_BIT))) != 0;
    }

    [[nodiscard]] auto Claim(const std::uint64_t cluster) noexcept {
        const auto word_index = cluster / kClustersPerClaimWord;
        const auto mask = 1ULL << (cluster % kClustersPerClaimWord);
        return (claims_[word_index].fetch_or(
            mask, std::memory_order_relaxed) & mask) == 0;
    }

public:
    FreeClusterMeasurement(
        const std::span<const unsigned char> bitmap,
        const std::uint32_t cluster_size,
        const std::uint64_t cluster_count)
        : bitmap_(bitmap),
          cluster_size_(cluster_size),
          cluster_count_(cluster_count),
          claims_(std::make_unique<std::atomic<std::uint64_t>[]>(
              cluster_count / kClustersPerClaimWord +
              ((cluster_count % kClustersPerClaimWord) != 0))),
          total_free_clusters_(CountFreeClusters(bitmap, cluster_count)) {
    }

    auto ObserveRead(
        const std::span<const unsigned char> data,
        const std::uint64_t offset) noexcept {
        if (data.empty()) {
            return;
        }

        const auto end = offset + data.size();
        const auto first_cluster = offset / cluster_size_;
        const auto last_cluster = (end - 1) / cluster_size_;
        auto fully_examined_clusters = std::uint64_t{};
        auto nonzero_clusters = std::uint64_t{};
        auto nonzero_bytes = std::uint64_t{};

        // Count a cluster only when this read returned all of its bytes.
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            const auto cluster_begin = cluster * cluster_size_;
            const auto cluster_end = cluster_begin + cluster_size_;
            if (IsAllocated(cluster) || (cluster_begin < offset) ||
                (cluster_end > end) || !Claim(cluster)) {
                continue;
            }

            ++fully_examined_clusters;
            auto cluster_nonzero_bytes = std::uint64_t{};
            const auto cluster_data = data.subspan(
                cluster_begin - offset, cluster_size_);
            for (const auto value : cluster_data) {
                cluster_nonzero_bytes += value != 0;
            }
            nonzero_bytes += cluster_nonzero_bytes;
            nonzero_clusters += cluster_nonzero_bytes != 0;
        }
        if (fully_examined_clusters == 0) {
            return;
        }
        fully_examined_clusters_.fetch_add(
            fully_examined_clusters, std::memory_order_relaxed);
        nonzero_clusters_.fetch_add(
            nonzero_clusters, std::memory_order_relaxed);
        nonzero_bytes_.fetch_add(nonzero_bytes, std::memory_order_relaxed);
    }

    auto Report(const wil::zwstring_view name) const noexcept {
        const auto fully_examined_clusters =
            fully_examined_clusters_.load(std::memory_order_relaxed);
        const auto nonzero_clusters =
            nonzero_clusters_.load(std::memory_order_relaxed);
        const auto nonzero_bytes =
            nonzero_bytes_.load(std::memory_order_relaxed);
        std::fwprintf(stderr,
            L"devicefs: free-cluster measurement for '%ls': "
            L"%llu of %llu bitmap-free clusters fully examined in one read; "
            L"%llu of those clusters contained nonzero data (%llu bytes total)\n",
            name.c_str(),
            fully_examined_clusters,
            total_free_clusters_,
            nonzero_clusters,
            nonzero_bytes);
    }

private:
    const std::span<const unsigned char> bitmap_;
    const std::uint32_t cluster_size_;
    const std::uint64_t cluster_count_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> claims_;
    const std::uint64_t total_free_clusters_;
    std::atomic<std::uint64_t> fully_examined_clusters_ = 0;
    std::atomic<std::uint64_t> nonzero_clusters_ = 0;
    std::atomic<std::uint64_t> nonzero_bytes_ = 0;
};

#if DEVICEFS_MEASURE_READ_PATH

// These constants are defined in wdm.h, which is part of the Windows Driver Kit,
// and is not a dependency of this project.
constexpr auto FILE_SEQUENTIAL_ONLY = std::uint32_t{0x00000004};
constexpr auto FILE_RANDOM_ACCESS = std::uint32_t{0x00000800};
constexpr auto FILE_NO_INTERMEDIATE_BUFFERING = std::uint32_t{0x00000008};

export class ReadPathMeasurement {
    using Clock = std::chrono::steady_clock;

    enum class ReadPath {
        unfinished,
        no_bounce,
        bounce,
        synthetic,
    };

    struct Timing {
        std::int64_t samples = 0;
        Clock::duration callback{};
        Clock::duration source{};
    };

    static constexpr auto kRequestLengthBounds = std::array<std::uint32_t, 11>{
        4 * 1024,
        8 * 1024,
        16 * 1024,
        32 * 1024,
        64 * 1024,
        128 * 1024,
        256 * 1024,
        512 * 1024,
        1024 * 1024,
        2 * 1024 * 1024,
        4 * 1024 * 1024,
    };
    static constexpr auto kTimingSampleInterval = std::uint64_t{256};

    struct ThreadStatistics {
        std::uint64_t sequence = 0;
        std::uint64_t open_count = 0;
        std::uint64_t sequential_open_count = 0;
        std::uint64_t random_open_count = 0;
        std::uint64_t unbuffered_open_count = 0;
        std::uint64_t read_count = 0;
        std::uint64_t requested_bytes = 0;
        std::uint64_t wanted_bytes = 0;
        std::uint64_t maximum_request = 0;
        std::array<std::uint64_t, kRequestLengthBounds.size() + 1>
            request_lengths{};
        std::uint64_t no_bounce_read_count = 0;
        std::uint64_t no_bounce_bytes = 0;
        std::uint64_t bounce_read_count = 0;
        std::uint64_t bounce_wanted_bytes = 0;
        std::uint64_t bounce_source_bytes = 0;
        std::uint64_t synthetic_read_count = 0;
        std::uint64_t synthetic_bytes = 0;
        std::uint64_t unfinished_read_count = 0;
        std::uint64_t source_read_count = 0;
        std::uint64_t completed_source_read_count = 0;
        std::uint64_t source_bytes = 0;
        std::uint64_t immediate_source_read_count = 0;
        std::uint64_t pending_source_read_count = 0;
        std::uint64_t maximum_active_reads = 0;
        Timing no_bounce_timing;
        Timing bounce_timing;
        Timing synthetic_timing;
    };

public:
    ReadPathMeasurement() noexcept = default;
    ~ReadPathMeasurement() = default;
    ReadPathMeasurement(const ReadPathMeasurement &) = delete;
    auto operator=(const ReadPathMeasurement &)
        -> ReadPathMeasurement & = delete;
    ReadPathMeasurement(ReadPathMeasurement &&) = delete;
    auto operator=(ReadPathMeasurement &&)
        -> ReadPathMeasurement & = delete;

    class ReadObservation {
    public:
        ReadObservation(const ReadObservation &) = delete;
        auto operator=(const ReadObservation &) -> ReadObservation & = delete;
        ReadObservation(ReadObservation &&) = delete;
        auto operator=(ReadObservation &&) -> ReadObservation & = delete;

        ~ReadObservation() noexcept {
            const auto callback_finished =
                sampled_ ? Clock::now() : Clock::time_point{};
            owner_->active_reads_.fetch_sub(1, std::memory_order_relaxed);
            if (statistics_ == nullptr) {
                return;
            }

            switch (path_) {
            case ReadPath::no_bounce:
                ++statistics_->no_bounce_read_count;
                statistics_->no_bounce_bytes += wanted_bytes_;
                FinishTiming(statistics_->no_bounce_timing, callback_finished);
                break;
            case ReadPath::bounce:
                ++statistics_->bounce_read_count;
                statistics_->bounce_wanted_bytes += wanted_bytes_;
                statistics_->bounce_source_bytes += source_bytes_;
                FinishTiming(statistics_->bounce_timing, callback_finished);
                break;
            case ReadPath::synthetic:
                ++statistics_->synthetic_read_count;
                statistics_->synthetic_bytes += wanted_bytes_;
                FinishTiming(statistics_->synthetic_timing, callback_finished);
                break;
            case ReadPath::unfinished:
                ++statistics_->unfinished_read_count;
                return;
            }
        }

        auto RecordSynthetic() noexcept -> void {
            path_ = ReadPath::synthetic;
        }

        auto BeginSourceRead() noexcept -> void {
            if (statistics_ == nullptr) {
                return;
            }
            ++statistics_->source_read_count;
            if (sampled_) {
                source_started_ = Clock::now();
            }
        }

        auto RecordSourcePending() noexcept -> void {
            source_pending_ = true;
        }

        auto FinishSourceRead(const std::uint32_t bytes) noexcept -> void {
            if (statistics_ == nullptr) {
                return;
            }
            const auto source_finished =
                sampled_ ? Clock::now() : Clock::time_point{};
            ++statistics_->completed_source_read_count;
            statistics_->source_bytes += bytes;
            if (source_pending_) {
                ++statistics_->pending_source_read_count;
            } else {
                ++statistics_->immediate_source_read_count;
            }
            if (sampled_) {
                source_elapsed_ = source_finished - source_started_;
            }
            source_bytes_ = bytes;
            path_ = ReadPath::no_bounce;
        }

        auto RecordBounce() noexcept -> void {
            path_ = ReadPath::bounce;
        }

    private:
        friend class ReadPathMeasurement;

        auto FinishTiming(
            Timing &timing,
            const Clock::time_point callback_finished) const noexcept -> void {
            if (!sampled_) {
                return;
            }
            ++timing.samples;
            timing.callback += callback_finished - callback_started_;
            timing.source += source_elapsed_;
        }

        ReadObservation(
            ReadPathMeasurement &owner,
            ThreadStatistics *const statistics,
            const std::uint32_t wanted_bytes,
            const bool sampled) noexcept
            : owner_(&owner),
              statistics_(statistics),
              wanted_bytes_(wanted_bytes),
              sampled_(sampled),
              callback_started_(sampled ? Clock::now() : Clock::time_point{}) {
        }

        ReadPathMeasurement *const owner_;
        ThreadStatistics *const statistics_;
        const std::uint32_t wanted_bytes_;
        const bool sampled_;
        bool source_pending_ = false;
        std::uint32_t source_bytes_ = 0;
        ReadPath path_ = ReadPath::unfinished;
        Clock::time_point callback_started_{};
        Clock::time_point source_started_{};
        Clock::duration source_elapsed_{};
    };

    auto RecordOpen(const std::uint32_t create_options) noexcept -> void {
        auto *const statistics = CurrentThread();
        if (statistics == nullptr) {
            return;
        }
        ++statistics->open_count;
        statistics->sequential_open_count +=
            (create_options & FILE_SEQUENTIAL_ONLY) != 0;
        statistics->random_open_count +=
            (create_options & FILE_RANDOM_ACCESS) != 0;
        statistics->unbuffered_open_count +=
            (create_options & FILE_NO_INTERMEDIATE_BUFFERING) != 0;
    }

    [[nodiscard]] auto BeginRead(
        const std::uint32_t requested_bytes,
        const std::uint32_t wanted_bytes) noexcept {
        auto *const statistics = CurrentThread();
        auto sampled = false;
        if (statistics != nullptr) {
            ++statistics->read_count;
            statistics->requested_bytes += requested_bytes;
            statistics->wanted_bytes += wanted_bytes;
            statistics->maximum_request =
                std::max(statistics->maximum_request,
                    std::uint64_t{requested_bytes});
            RecordRequestLength(*statistics, requested_bytes);
            sampled = (++statistics->sequence % kTimingSampleInterval) == 0;
        }

        const auto active =
            active_reads_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (statistics != nullptr) {
            statistics->maximum_active_reads =
                std::max(statistics->maximum_active_reads, active);
        }
        return ReadObservation{*this, statistics, wanted_bytes, sampled};
    }

    auto Report() noexcept -> void {
        auto total = ThreadStatistics{};
        auto read_threads = 0uz;
        for (const auto &thread : threads_) {
            read_threads += thread->read_count != 0;
            total.open_count += thread->open_count;
            total.sequential_open_count += thread->sequential_open_count;
            total.random_open_count += thread->random_open_count;
            total.unbuffered_open_count += thread->unbuffered_open_count;
            total.read_count += thread->read_count;
            total.requested_bytes += thread->requested_bytes;
            total.wanted_bytes += thread->wanted_bytes;
            total.maximum_request =
                std::max(total.maximum_request, thread->maximum_request);
            for (auto i = 0uz; i < total.request_lengths.size(); ++i) {
                total.request_lengths[i] += thread->request_lengths[i];
            }
            total.no_bounce_read_count += thread->no_bounce_read_count;
            total.no_bounce_bytes += thread->no_bounce_bytes;
            total.bounce_read_count += thread->bounce_read_count;
            total.bounce_wanted_bytes += thread->bounce_wanted_bytes;
            total.bounce_source_bytes += thread->bounce_source_bytes;
            total.synthetic_read_count += thread->synthetic_read_count;
            total.synthetic_bytes += thread->synthetic_bytes;
            total.unfinished_read_count += thread->unfinished_read_count;
            total.source_read_count += thread->source_read_count;
            total.completed_source_read_count +=
                thread->completed_source_read_count;
            total.source_bytes += thread->source_bytes;
            total.immediate_source_read_count +=
                thread->immediate_source_read_count;
            total.pending_source_read_count +=
                thread->pending_source_read_count;
            total.maximum_active_reads =
                std::max(total.maximum_active_reads,
                    thread->maximum_active_reads);
            AccumulateTiming(total.no_bounce_timing, thread->no_bounce_timing);
            AccumulateTiming(total.bounce_timing, thread->bounce_timing);
            AccumulateTiming(total.synthetic_timing, thread->synthetic_timing);
        }

        std::fwprintf(stderr,
            L"devicefs: WinFsp Read-callback measurement\n");
        std::fwprintf(stderr,
            L"  reads satisfied above this callback are not observed\n");
        std::fwprintf(stderr,
            L"  opens: total=%llu sequential=%llu random=%llu "
                L"no-intermediate-buffering=%llu\n",
            total.open_count,
            total.sequential_open_count,
            total.random_open_count,
            total.unbuffered_open_count);
        std::fwprintf(stderr,
            L"  measured reads: count=%llu requested-bytes=%llu wanted-bytes=%llu "
                L"maximum-request=%llu\n",
            total.read_count,
            total.requested_bytes,
            total.wanted_bytes,
            total.maximum_request);
        std::fwprintf(stderr,
            L"  request lengths: <=4K=%llu <=8K=%llu <=16K=%llu <=32K=%llu "
                L"<=64K=%llu <=128K=%llu <=256K=%llu <=512K=%llu "
                L"<=1M=%llu <=2M=%llu <=4M=%llu >4M=%llu\n",
            total.request_lengths[0],
            total.request_lengths[1],
            total.request_lengths[2],
            total.request_lengths[3],
            total.request_lengths[4],
            total.request_lengths[5],
            total.request_lengths[6],
            total.request_lengths[7],
            total.request_lengths[8],
            total.request_lengths[9],
            total.request_lengths[10],
            total.request_lengths[11]);
        std::fwprintf(stderr,
            L"  paths: no-bounce-reads=%llu no-bounce-bytes=%llu "
                L"bounce-reads=%llu bounce-wanted-bytes=%llu "
                L"bounce-backing-bytes=%llu synthetic-all-free-reads=%llu "
                L"synthetic-all-free-bytes=%llu unfinished-reads=%llu\n",
            total.no_bounce_read_count,
            total.no_bounce_bytes,
            total.bounce_read_count,
            total.bounce_wanted_bytes,
            total.bounce_source_bytes,
            total.synthetic_read_count,
            total.synthetic_bytes,
            total.unfinished_read_count);
        std::fwprintf(stderr,
            L"  backing reads: started=%llu completed=%llu bytes=%llu "
                L"immediate=%llu pending=%llu\n",
            total.source_read_count,
            total.completed_source_read_count,
            total.source_bytes,
            total.immediate_source_read_count,
            total.pending_source_read_count);
        std::fwprintf(stderr,
            L"  concurrency: read-threads=%zu peak-active=%llu active-at-stop=%llu "
                L"unrecorded-threads=%llu\n",
            read_threads,
            total.maximum_active_reads,
            active_reads_.load(std::memory_order_relaxed),
            registration_failures_.load(std::memory_order_relaxed));
        std::fwprintf(stderr,
            L"  sampled timing (1/%llu per thread, averages in ns): "
                L"no-bounce-samples=%lld no-bounce-callback=%lld "
                L"no-bounce-backing=%lld "
                L"bounce-samples=%lld bounce-callback=%lld bounce-backing=%lld "
                L"synthetic-samples=%lld synthetic-callback=%lld\n",
            kTimingSampleInterval,
            total.no_bounce_timing.samples,
            AverageCallbackNanoseconds(total.no_bounce_timing),
            AverageSourceNanoseconds(total.no_bounce_timing),
            total.bounce_timing.samples,
            AverageCallbackNanoseconds(total.bounce_timing),
            AverageSourceNanoseconds(total.bounce_timing),
            total.synthetic_timing.samples,
            AverageCallbackNanoseconds(total.synthetic_timing));
    }

private:
    [[nodiscard]] auto CurrentThread() noexcept -> ThreadStatistics * {
        // Each dispatcher thread updates one stable block; shutdown aggregates
        // those blocks only after WinFsp has stopped the dispatchers.
        struct ThreadBinding {
            const ReadPathMeasurement *owner = nullptr;
            ThreadStatistics *statistics = nullptr;
        };
        thread_local auto binding = ThreadBinding{};
        if (binding.owner == this) {
            return binding.statistics;
        }

        try {
            auto statistics = std::make_unique<ThreadStatistics>();
            auto *const result = statistics.get();
            {
                auto lock = std::lock_guard{registration_mutex_};
                threads_.push_back(std::move(statistics));
            }
            binding = {.owner = this, .statistics = result};
            return result;
        } catch (...) {
            registration_failures_.fetch_add(1, std::memory_order_relaxed);
            binding = {.owner = this, .statistics = nullptr};
            return nullptr;
        }
    }

    static auto RecordRequestLength(
        ThreadStatistics &statistics,
        const std::uint32_t requested_bytes) noexcept -> void {
        auto bucket = statistics.request_lengths.begin();
        for (const auto maximum : kRequestLengthBounds) {
            if (requested_bytes <= maximum) {
                ++*bucket;
                return;
            }
            ++bucket;
        }
        ++*bucket;
    }

    static auto AccumulateTiming(
        Timing &total, const Timing &addend) noexcept -> void {
        total.samples += addend.samples;
        total.callback += addend.callback;
        total.source += addend.source;
    }

    [[nodiscard]] static auto AverageCallbackNanoseconds(
        const Timing &timing) noexcept -> std::int64_t {
        if (timing.samples == 0) {
            return std::int64_t{};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.callback).count() / timing.samples;
    }

    [[nodiscard]] static auto AverageSourceNanoseconds(
        const Timing &timing) noexcept -> std::int64_t {
        if (timing.samples == 0) {
            return std::int64_t{};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing.source).count() / timing.samples;
    }

    std::mutex registration_mutex_;
    std::vector<std::unique_ptr<ThreadStatistics>> threads_;
    std::atomic<std::uint64_t> active_reads_ = 0;
    std::atomic<std::uint64_t> registration_failures_ = 0;
};

#endif
