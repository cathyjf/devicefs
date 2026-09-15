// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <cerrno>

export module devicefs.asr_trace.trace;
import std;
import devicefs.asr_trace.filesystem;

export namespace devicefs::asr_trace {

constexpr auto kReadbackCacheLimit = std::uint64_t{4} << 30;
constexpr auto kRollingWriteCacheLimit = std::uint64_t{4} << 30;

enum class Phase { Setup, Restore, Finalize, Detached };

struct Statistics {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t nonforward_requests = 0;
    std::uint64_t behind_completed_writes = 0;
};

struct Report {
    bool complete;
    std::array<Statistics, std::meta::enumerators_of(^^Phase).size()> phases;

    auto NonforwardRequests() const -> std::uint64_t {
        return std::get<1>(phases).nonforward_requests +
            std::get<2>(phases).nonforward_requests +
            std::get<3>(phases).nonforward_requests;
    }

    auto ReadRequestsAfterSetup() const -> std::uint64_t {
        return std::get<1>(phases).reads + std::get<2>(phases).reads +
            std::get<3>(phases).reads;
    }

    auto ReadBytesAfterSetup() const -> std::uint64_t {
        return std::get<1>(phases).read_bytes + std::get<2>(phases).read_bytes +
            std::get<3>(phases).read_bytes;
    }
};

// The same recorder receives direct synthetic I/O and calls from macFUSE.
// Its mutex orders observations; the backing I/O runs outside that mutex.
template <ImageFilesystem Filesystem>
class RecordingFilesystem final {
public:
    const std::uint64_t &Size;

    RecordingFilesystem(Filesystem &filesystem, const std::filesystem::path &trace,
        const std::uint64_t cache_limit = kReadbackCacheLimit,
        const std::uint64_t write_cache_limit = kRollingWriteCacheLimit)
        : Size{filesystem.Size}, filesystem_{filesystem},
          trace_{trace, std::ios::out | std::ios::noreplace}, cache_limit_{cache_limit},
          write_cache_limit_{write_cache_limit} {
        if (!trace_) {
            throw std::system_error{errno, std::generic_category(),
                std::format("could not create trace '{}'", trace.string())};
        }
        trace_.exceptions(std::ios::badbit | std::ios::failbit);
        std::println(trace_, "# asr-image-trace 1; length={}", Size);
        std::println(trace_,
            "event_id\ttime_ns\toperation_id\tphase\toperation\tevent\toffset\t"
            "requested\ttransferred\terror\tactive_writes\tcompleted_write_end");
        trace_.flush();
    }

    auto Read(const std::uint64_t offset, const std::span<char> buffer) -> IoResult {
        return Transfer(Kind::Read, offset, buffer.size(),
            [&filesystem = filesystem_, offset, buffer] {
                return filesystem.Read(offset, buffer);
            }, buffer);
    }

    auto Write(const std::uint64_t offset, const std::size_t length,
        const std::span<const char> data = {}) -> IoResult
        pre(data.empty() || (data.size() == length)) {
        return Transfer(Kind::Write, offset, length,
            [&filesystem = filesystem_, offset, length] {
                return filesystem.Write(offset, length);
            }, {}, data);
    }

    auto Synchronize() -> int {
        return Transfer(Kind::Synchronize, 0, 0,
            [&filesystem = filesystem_] {
                return IoResult{.error = filesystem.Synchronize()};
            }).error;
    }

    // Truncation invalidates cached bytes; the initial image keeps its fixed size.
    auto Truncate(const std::uint64_t size) -> void {
        const auto lock = std::lock_guard{mutex_};
        const auto cached_before = cache_bytes_ + write_cache_bytes_;
        if (size < Size) {
            for (auto iterator = read_cache_.begin(); iterator != read_cache_.end();) {
                const auto length = std::min<std::uint64_t>(iterator->second.size(),
                    std::saturating_sub(size, iterator->first.first));
                if (length == iterator->second.size()) {
                    ++iterator;
                    continue;
                }
                auto entry = read_cache_.extract(iterator++);
                cache_bytes_ -= entry.mapped().size();
                entry.key().second = length;
                entry.mapped().resize(length);
                entry.mapped().shrink_to_fit();
                if ((length != 0) && read_cache_.insert(std::move(entry)).inserted) {
                    cache_bytes_ += length;
                }
            }
            for (auto &[offset, bytes] : write_cache_) {
                const auto length = std::min<std::uint64_t>(bytes.size(),
                    std::saturating_sub(size, offset));
                write_cache_bytes_ -= bytes.size() - length;
                bytes.resize(length);
                bytes.shrink_to_fit();
            }
            std::erase_if(write_cache_, [](const auto &entry) {
                return entry.second.empty();
            });
        }
        std::println(std::cerr,
            "Truncate request to {} bytes: invalidated {} cached bytes; "
            "image size remains {} bytes.",
            size, cached_before - cache_bytes_ - write_cache_bytes_, Size);
        std::println(trace_, "# truncate requested_size={} invalidated_cache_bytes={}",
            size, cached_before - cache_bytes_ - write_cache_bytes_);
    }

    auto SetPhase(const Phase phase) -> void {
        const auto lock = std::lock_guard{mutex_};
        contract_assert(std::to_underlying(phase) == std::to_underlying(phase_) + 1);
        phase_ = phase;
        Record({.id = 0, .phase = phase_, .kind = Kind::Phase, .offset = 0, .requested = 0},
            "marker", {});
        trace_.flush();
    }

    auto Fail() noexcept -> void { failed_ = true; }

    auto Finish(const bool producer_succeeded) -> Report {
        const auto lock = std::lock_guard{mutex_};
        std::println("READBACK CACHE PEAK: {} bytes (limit {} bytes).",
            cache_peak_, cache_limit_);
        std::println(trace_, "# readback_cache_peak_bytes={} limit_bytes={}",
            cache_peak_, cache_limit_);
        std::println("ROLLING WRITE CACHE PEAK: {} bytes (limit {} bytes).",
            write_cache_peak_, write_cache_limit_);
        std::println(trace_, "# rolling_write_cache_peak_bytes={} limit_bytes={}",
            write_cache_peak_, write_cache_limit_);
        const auto has_errors = std::ranges::any_of(statistics_,
            [](const auto &statistics) { return statistics.errors != 0; });
        const auto report = Report{
            .complete = producer_succeeded && !failed_ && !has_errors &&
                (active_operations_ == 0) && (phase_ == Phase::Detached) &&
                (restore_progress_.completed_end != 0),
            .phases = statistics_,
        };
        for (const auto phase :
            {Phase::Setup, Phase::Restore, Phase::Finalize, Phase::Detached}) {
            const auto &statistics = statistics_.at(std::to_underlying(phase));
            std::println(trace_, "# summary phase={} reads={} writes={} read_bytes={} "
                "write_bytes={} errors={} nonforward_requests={} behind_completed_writes={}",
                kPhases.at(std::to_underlying(phase)), statistics.reads, statistics.writes,
                statistics.read_bytes, statistics.write_bytes, statistics.errors,
                statistics.nonforward_requests, statistics.behind_completed_writes);
        }
        std::println(trace_, "# end complete={} active_operations={}",
            report.complete, active_operations_);
        trace_.flush();
        return report;
    }

private:
    enum class Kind { Read, Write, Synchronize, Phase };

    struct Operation {
        std::uint64_t id;
        Phase phase;
        Kind kind;
        std::uint64_t offset;
        std::size_t requested;
    };

    struct WriteProgress {
        std::uint64_t requested_end = 0;
        std::uint64_t completed_end = 0;
    };

    static constexpr auto kPhases = std::array{
        "setup", "restore", "finalize", "detached",
    };
    static constexpr auto kKinds = std::array{"read", "write", "fsync", "phase"};

    auto ProgressFor(const Phase phase) -> WriteProgress & {
        // Late writes must still be compared with restore writes. Setup uses
        // separate history so formatting does not contaminate that comparison.
        return phase == Phase::Setup ? setup_progress_ : restore_progress_;
    }

    auto Begin(const Kind kind, const std::uint64_t offset,
        const std::size_t requested) -> Operation {
        const auto lock = std::lock_guard{mutex_};
        const auto operation = Operation{.id = ++operation_id_, .phase = phase_,
            .kind = kind, .offset = offset, .requested = requested};
        ++active_operations_;
        auto &statistics = statistics_.at(std::to_underlying(phase_));
        if (kind == Kind::Write) {
            ++active_writes_;
            ++statistics.writes;
            auto &progress = ProgressFor(phase_);
            if (requested != 0) {
                if (offset < progress.requested_end) {
                    ++statistics.nonforward_requests;
                }
                if (offset < progress.completed_end) {
                    ++statistics.behind_completed_writes;
                }
                progress.requested_end = std::max(progress.requested_end,
                    std::saturating_add(offset, std::uint64_t{requested}));
            }
        } else if (kind == Kind::Read) {
            ++statistics.reads;
        }
        Record(operation, "begin", {});
        return operation;
    }

    auto End(const Operation &operation, const IoResult result,
        std::span<char> read, std::span<const char> write) -> IoResult
        pre(result.transferred <= operation.requested) {
        const auto lock = std::lock_guard{mutex_};
        read = read.first(std::min(read.size(), result.transferred));
        write = write.first(std::min(write.size(), result.transferred));
        if ((operation.kind == Kind::Write) &&
            ((operation.offset > Size) ||
                (operation.requested > std::saturating_sub(Size, operation.offset)))) {
            std::println(std::cerr,
                "Acknowledging write at offset {} with length {} beyond the {}-byte image.",
                operation.offset, operation.requested, Size);
        }
        const auto apply_cached = [offset = operation.offset,
            length = std::max(read.size(), write.size()), read, write]
            (const std::uint64_t cached_offset, const std::span<char> bytes) {
            const auto start = std::max(offset, cached_offset);
            const auto count = std::min(
                std::saturating_sub(std::uint64_t{length}, start - offset),
                std::saturating_sub(std::uint64_t{bytes.size()}, start - cached_offset));
            if (count != 0) {
                const auto cached = bytes.subspan(start - cached_offset, count);
                if (!read.empty()) {
                    std::ranges::copy(cached, read.subspan(start - offset).begin());
                } else if (!write.empty()) {
                    std::ranges::copy(write.subspan(start - offset, cached.size()),
                        cached.begin());
                }
            }
        };
        for (auto &[range, bytes] : read_cache_) {
            apply_cached(range.first, bytes);
        }
        if (!read.empty()) {
            // Later queued writes overwrite earlier ones before the read is
            // retained permanently, so readback survives rolling eviction.
            for (auto &[offset, bytes] : write_cache_) {
                apply_cached(offset, bytes);
            }
        } else if (!write.empty()) {
            const auto recent = write.last(
                std::min<std::uint64_t>(write.size(), write_cache_limit_));
            while (write_cache_bytes_ > write_cache_limit_ - recent.size()) {
                write_cache_bytes_ -= write_cache_.front().second.size();
                write_cache_.pop_front();
            }
            if (!recent.empty()) {
                write_cache_.emplace_back(std::saturating_add(operation.offset,
                    std::uint64_t{write.size() - recent.size()}),
                    std::vector<char>{recent.begin(), recent.end()});
                write_cache_bytes_ += recent.size();
                write_cache_peak_ = std::max(write_cache_peak_, write_cache_bytes_);
            }
        }
        const auto key = std::pair{operation.offset, read.size()};
        if (!read.empty() && !read_cache_.contains(key)) {
            if (read.size() > cache_limit_ - cache_bytes_) {
                throw std::runtime_error{std::format(
                    "Readback cache limit exceeded at image offset {}: read length {} bytes, "
                    "{} bytes already cached, {} additional bytes required, limit {} bytes.",
                    operation.offset, operation.requested, cache_bytes_, read.size(),
                    cache_limit_)};
            }
            read_cache_.try_emplace(key, read.begin(), read.end());
            cache_bytes_ += read.size();
            cache_peak_ = std::max(cache_peak_, cache_bytes_);
        }
        contract_assert(active_operations_ != 0);
        --active_operations_;
        auto &statistics = statistics_.at(std::to_underlying(operation.phase));
        if (operation.kind == Kind::Write) {
            contract_assert(active_writes_ != 0);
            --active_writes_;
            statistics.write_bytes += result.transferred;
            if (result.transferred != 0) {
                auto &progress = ProgressFor(operation.phase);
                progress.completed_end = std::max(progress.completed_end,
                    std::saturating_add(operation.offset,
                        std::uint64_t{result.transferred}));
            }
        } else if (operation.kind == Kind::Read) {
            statistics.read_bytes += result.transferred;
        }
        statistics.errors += result.error != 0;
        Record(operation, "end", result);
        return result;
    }

    template <typename TransferOperation>
    auto Transfer(const Kind kind, const std::uint64_t offset,
        const std::size_t length, TransferOperation transfer,
        const std::span<char> read = {}, const std::span<const char> write = {}) -> IoResult {
        try {
            const auto operation = Begin(kind, offset, length);
            return End(operation, transfer(), read, write);
        } catch (...) {
            Fail();
            throw;
        }
    }

    auto Record(const Operation &operation, const std::string_view event,
        const IoResult result) -> void {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_).count();
        std::println(trace_, "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            ++event_id_, elapsed, operation.id,
            kPhases.at(std::to_underlying(operation.phase)),
            kKinds.at(std::to_underlying(operation.kind)), event, operation.offset,
            operation.requested, result.transferred, result.error, active_writes_,
            ProgressFor(operation.phase).completed_end);
    }

    Filesystem &filesystem_;
    std::ofstream trace_;
    const std::uint64_t cache_limit_;
    std::map<std::pair<std::uint64_t, std::size_t>, std::vector<char>> read_cache_;
    std::uint64_t cache_bytes_ = 0;
    std::uint64_t cache_peak_ = 0;
    const std::uint64_t write_cache_limit_;
    std::deque<std::pair<std::uint64_t, std::vector<char>>> write_cache_;
    std::uint64_t write_cache_bytes_ = 0;
    std::uint64_t write_cache_peak_ = 0;
    std::mutex mutex_;
    std::atomic<bool> failed_ = false;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    Phase phase_ = Phase::Setup;
    std::uint64_t event_id_ = 0;
    std::uint64_t operation_id_ = 0;
    std::uint64_t active_operations_ = 0;
    std::uint64_t active_writes_ = 0;
    decltype(Report::phases) statistics_ {};
    WriteProgress setup_progress_;
    WriteProgress restore_progress_;
};

} // namespace devicefs::asr_trace
