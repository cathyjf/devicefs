// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <experimental/scope>

import std;
import devicefs.terminal.safecast;
import devicefs.asr_trace.filesystem;
import devicefs.asr_trace.trace;
import devicefs.asr_trace.synthetic;
import devicefs.asr_trace.experiment;

using namespace devicefs::asr_trace;

namespace {

auto Require(const bool condition, const std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <WritingPolicy Policy, ImageFilesystem Filesystem>
auto CheckPolicy(Filesystem &filesystem, const std::filesystem::path &directory,
    const std::string_view name) -> void {
    auto producer = SyntheticProducer<Policy>{};
    const auto report = RunExperiment(filesystem, producer,
        directory / std::format("{}.tsv", name));
    Require(report.complete, "synthetic capture was incomplete");
    if constexpr (std::same_as<Policy, SerialWriting>) {
        Require(report.NonforwardRequests() == 0,
            "serial writes were classified as nonforward");
    } else {
        Require(report.NonforwardRequests() != 0, "jump-around writes were not detected");
        Require(std::get<1>(report.phases).behind_completed_writes != 0,
            "jump-around writes lacked a completed-write witness");
    }
    Require(std::get<1>(report.phases).write_bytes == filesystem.Size,
        "write byte total differs from fixture size");
    std::println("PASS {}: {} nonforward write requests; payload discarded",
        name, report.NonforwardRequests());
}

template <ImageFilesystem Filesystem>
auto CheckBoundaries(Filesystem &filesystem) -> void {
    const auto offset = filesystem.Size - 2;
    const auto extended = filesystem.Write(offset, 4);
    Require((extended.error == 0) && (extended.transferred == 4),
        "write beyond the fixed image must acknowledge its full length");
    Require(filesystem.Write(offset, 2).transferred == 2,
        "final partial write");
    auto result = std::array{'x', 'x', 'x', 'x'};
    const auto read = filesystem.Read(offset, result);
    Require((read.error == 0) && (read.transferred == result.size()) &&
        (result == std::array<char, 4>{}),
        "reads crossing the fixed image size must return their full length as zeros");
    Require(filesystem.Read(filesystem.Size, result).transferred == result.size(),
        "reads starting at the fixed image size must return their full length");
    Require(filesystem.Write(filesystem.Size, 0).error == 0, "empty write at EOF");
}

auto CheckFinalization(const std::filesystem::path &directory) -> void {
    auto filesystem = SyntheticFilesystem{131072};
    auto recording = RecordingFilesystem{filesystem, directory / "finalization.tsv"};
    Require(recording.Write(120000, 4).error == 0, "setup write");
    recording.SetPhase(Phase::Restore);
    Require(recording.Write(0, 4).error == 0, "restore first write");
    Require(recording.Write(65536, 4).error == 0, "restore later write");
    recording.SetPhase(Phase::Finalize);
    Require(recording.Write(0, 4).error == 0, "finalization rewrite");
    recording.SetPhase(Phase::Detached);
    const auto report = recording.Finish(true);
    Require(report.complete && (std::get<1>(report.phases).nonforward_requests == 0) &&
        (std::get<2>(report.phases).behind_completed_writes == 1),
        "phase accounting lost a late rewrite");
    std::println("PASS finalization: setup isolated and late rewrite detected");
}

auto CheckDiscardedWrites(const std::filesystem::path &directory) -> void {
    constexpr auto size = std::uint64_t{1} << 40;
    auto large = SyntheticFilesystem{size};
    Require(large.Write(0, size).transferred == size,
        "a metadata-only terabyte write must require no payload storage");
    CheckBoundaries(large);

    const auto seed_path = directory / "initial-seed.raw";
    const auto cleanup = std::experimental::scope_exit{[&seed_path] {
        auto error = std::error_code{};
        std::filesystem::remove(seed_path, error);
        if (error) {
            std::println(std::cerr, "could not remove fixture '{}' (error {})",
                seed_path.c_str(), error.value());
        }
    }};
    constexpr auto initial = std::string_view{"initial empty image metadata"};
    {
        auto output = std::ofstream{seed_path, std::ios::binary | std::ios::noreplace};
        output.exceptions(std::ios::badbit | std::ios::failbit);
        std::print(output, "{}", initial);
    }
    auto seed = SeedFilesystem{seed_path};
    Require(seed.Write(0, initial.size()).transferred == initial.size(),
        "seed write acknowledgement");
    auto bytes = std::array<char, initial.size()>{};
    const auto read = seed.Read(0, bytes);
    Require((read.error == 0) && (read.transferred == initial.size()) &&
        (std::string_view{bytes.data(), read.transferred} == initial),
        "destination writes must leave initial seed bytes unchanged");
    std::println("PASS discard: terabyte request uses metadata only; seed remains unchanged");

    auto sample = std::array<char, 4>{};
    for (const auto offset : {0uz, initial.size() / 2, initial.size() - sample.size()}) {
        const auto result = seed.Read(offset, sample);
        Require((result.error == 0) && (result.transferred == sample.size()) &&
            (std::string_view{sample.data(), sample.size()} ==
                initial.substr(offset, sample.size())), "initial-image positional read");
    }
    auto recording = RecordingFilesystem{
        seed, directory / "initial-image-cache.tsv", 10, 8};
    auto short_read = std::array<char, 2>{};
    auto crossing = std::array<char, 6>{};
    constexpr auto payload = std::string_view{"ABCDEFGH"};
    recording.SetPhase(Phase::Restore);
    recording.Read(4, sample);
    recording.Read(4, short_read);
    recording.Write(2, payload.size(), std::span{payload});
    const auto distant = recording.Write(std::numeric_limits<std::uint64_t>::max(), 4);
    recording.Read(4, sample);
    Require((distant.error == 0) && (distant.transferred == 4) &&
        (sample == std::array{'C', 'D', 'E', 'F'}),
        "an extreme write offset must be acknowledged without altering cached bytes");
    recording.Truncate(6);
    recording.Read(2, crossing);
    Require(crossing == std::array{'A', 'B', 'C', 'D', 'l', ' '},
        "truncate must preserve cached prefixes and restore the initial-image suffix");
    recording.Write(16, payload.size(), std::span{payload});
    recording.Truncate(recording.Size);
    recording.Truncate(recording.Size + 100);
    recording.Read(16, short_read);
    Require(short_read == std::array{'A', 'B'},
        "same-size and expansion requests must retain cached writes");
    recording.Truncate(16);
    recording.Read(16, short_read);
    Require(short_read == std::array{'a', 'g'},
        "truncate must discard both caches at the exact cutoff and release read capacity");
    recording.Truncate(0);
    recording.Write(0, payload.size(), std::span{payload});
    recording.Read(0, crossing);
    Require(crossing == std::array{'A', 'B', 'C', 'D', 'E', 'F'},
        "cache refill after truncation to zero");
    recording.Truncate(0);
    recording.Read(0, crossing);
    Require(std::string_view{crossing.data(), crossing.size()} ==
        initial.substr(0, crossing.size()),
        "truncation to zero must invalidate all cached writes");
    sample.fill('?');
    const auto end_read = recording.Read(initial.size() - 2, sample);
    Require((end_read.error == 0) && (end_read.transferred == sample.size()) &&
        (sample == std::array{'t', 'a', '\0', '\0'}),
        "reads crossing the initial-image end must zero the suffix");
    const auto beyond = seed.Read(seed.Size + 4, sample);
    Require((beyond.error == 0) && (beyond.transferred == sample.size()) &&
        (sample == std::array<char, 4>{}),
        "reads beyond the initial image must return their full length as zeros");
    const auto unchanged = seed.Read(0, bytes);
    Require((recording.Size == initial.size()) && (seed.Size == initial.size()) &&
        (std::filesystem::file_size(seed_path) == initial.size()) &&
        (std::string_view{bytes.data(), unchanged.transferred} == initial),
        "cached writes and truncation must leave the initial file and size unchanged");
    recording.SetPhase(Phase::Finalize);
    recording.SetPhase(Phase::Detached);
    Require(recording.Finish(true).complete, "initial-image cache capture was incomplete");
    std::println("PASS initial image: positional reads, EOF and cache-only truncation");
}

auto CheckReads(const std::filesystem::path &directory) -> void {
    auto filesystem = SyntheticFilesystem{64};
    auto recording = RecordingFilesystem{filesystem, directory / "reads.tsv"};
    auto buffer = std::array<char, 4>{};
    Require(recording.Read(0, buffer).transferred == buffer.size(), "setup read");
    recording.SetPhase(Phase::Restore);
    Require(recording.Write(0, 4).error == 0, "restore write before read");
    Require(recording.Read(32, buffer).transferred == buffer.size(), "restore read");
    recording.SetPhase(Phase::Finalize);
    Require(recording.Read(0, buffer).transferred == buffer.size(), "finalization read");
    recording.SetPhase(Phase::Detached);
    const auto report = recording.Finish(true);
    Require(report.complete && (std::get<0>(report.phases).reads == 1) &&
        (report.ReadRequestsAfterSetup() == 2) && (report.ReadBytesAfterSetup() == 8),
        "reads after setup must remain visible independently of initial attachment reads");
    std::println("PASS reads: restore and finalization reads reported separately from setup");
}

auto CheckIncompleteCapture(const std::filesystem::path &directory) -> void {
    auto filesystem = SyntheticFilesystem{64};
    auto recording = RecordingFilesystem{filesystem, directory / "explicit-failure.tsv"};
    recording.SetPhase(Phase::Restore);
    Require(recording.Write(0, 4).transferred == 4, "valid write before explicit failure");
    recording.Fail();
    recording.SetPhase(Phase::Finalize);
    recording.SetPhase(Phase::Detached);
    const auto report = recording.Finish(true);
    Require(!report.complete,
        "an explicit failure must prevent a complete capture when the producer reports success");
    std::println("PASS failure: an explicit failure prevents a complete capture");
}

auto CheckReadbackCache(const std::filesystem::path &directory) -> void {
    auto filesystem = SyntheticFilesystem{32};
    auto recording = RecordingFilesystem{filesystem, directory / "readback-cache.tsv", 8, 0};
    auto buffer = std::array<char, 4>{};
    recording.SetPhase(Phase::Restore);
    recording.Read(4, buffer);
    constexpr auto payload = std::string_view{"abcdefghijklmnop"};
    recording.Write(0, payload.size(), std::span{payload});
    recording.Read(2, buffer);
    Require(buffer == std::array{'\0', '\0', 'e', 'f'},
        "only previously read bytes may retain writes");
    constexpr auto replacement = std::string_view{"XYZW"};
    recording.Write(3, replacement.size(), std::span{replacement});
    recording.Read(4, buffer);
    Require(buffer == std::array{'Y', 'Z', 'W', 'h'}, "partial cached overwrite");
    recording.Read(2, buffer);
    Require(buffer == std::array{'\0', 'X', 'Y', 'Z'},
        "overlapping cached reads must agree and reuse their allocations");
    auto limit_error = std::string{};
    try {
        recording.Read(20, buffer);
    } catch (const std::runtime_error &error) {
        limit_error = error.what();
    }
    Require(limit_error.contains("offset 20") &&
        limit_error.contains("8 bytes already cached") &&
        limit_error.contains("4 additional bytes required, limit 8 bytes"),
        "cache exhaustion must report the request, usage, additional allocation and limit");
    recording.SetPhase(Phase::Finalize);
    recording.SetPhase(Phase::Detached);
    Require(!recording.Finish(true).complete, "cache exhaustion must fail the capture");
    std::println("PASS readback cache: partial overlaps, discarded writes, reuse and size limit");
}

auto CheckRollingWriteCache(const std::filesystem::path &directory) -> void {
    auto filesystem = SyntheticFilesystem{64};
    auto recording = RecordingFilesystem{
        filesystem, directory / "rolling-write-cache.tsv", 64, 8};
    constexpr auto first = std::string_view{"ABCD"};
    constexpr auto newer = std::string_view{"WXYZ"};
    constexpr auto replacement = std::string_view{"mnop"};
    constexpr auto full = std::string_view{"12345678"};
    auto promoted = std::array<char, 6>{};
    recording.SetPhase(Phase::Restore);
    recording.Write(20, first.size(), std::span{first});
    recording.Write(22, newer.size(), std::span{newer});
    recording.Read(20, promoted);
    Require(promoted == std::array{'A', 'B', 'W', 'X', 'Y', 'Z'},
        "writes before the first read must be returned with the newest overlap winning");
    recording.Write(0, full.size(), std::span{full});
    recording.Read(20, promoted);
    Require(promoted == std::array{'A', 'B', 'W', 'X', 'Y', 'Z'},
        "a promoted read must survive eviction of its original writes");
    recording.Write(21, replacement.size(), std::span{replacement});
    recording.Read(20, promoted);
    Require(promoted == std::array{'A', 'm', 'n', 'o', 'p', 'Z'},
        "later writes must update promoted read buffers");
    recording.SetPhase(Phase::Finalize);
    recording.SetPhase(Phase::Detached);
    Require(recording.Finish(true).complete, "rolling write capture was incomplete");

    auto age = RecordingFilesystem{filesystem, directory / "rolling-write-age.tsv", 64, 8};
    auto buffer = std::array<char, 4>{};
    age.SetPhase(Phase::Restore);
    age.Write(40, first.size(), std::span{first});
    age.Write(0, newer.size(), std::span{newer});
    age.Write(20, replacement.size(), std::span{replacement});
    age.Read(40, buffer);
    Require(buffer == std::array<char, 4>{}, "eviction must remove the oldest write by age");
    Require(age.Write(filesystem.Size, 0).error == 0, "empty rolling write");
    age.Read(0, buffer);
    Require(buffer == std::array{'W', 'X', 'Y', 'Z'},
        "empty writes must not evict retained writes");
    const auto extended = age.Write(62, first.size(), std::span{first});
    Require((extended.error == 0) && (extended.transferred == first.size()),
        "out-of-range rolling write must acknowledge its full length");
    age.Truncate(filesystem.Size);
    age.Truncate(filesystem.Size + 1);
    const auto end_read = age.Read(62, buffer);
    Require((end_read.transferred == buffer.size()) &&
        (buffer == std::array{'A', 'B', 'C', 'D'}) && (age.Size == filesystem.Size),
        "rolling cache must retain bytes beyond the fixed image size");
    constexpr auto oversized = std::string_view{"0123456789"};
    age.Write(48, oversized.size(), std::span{oversized});
    auto tail = std::array<char, oversized.size()>{};
    age.Read(48, tail);
    Require(tail == std::array{'\0', '\0', '2', '3', '4', '5', '6', '7', '8', '9'},
        "a request larger than the rolling budget must retain only its tail");
    age.Read(62, buffer);
    Require(buffer == std::array{'A', 'B', 'C', 'D'},
        "promoted bytes beyond the image must survive rolling eviction");
    age.Write(filesystem.Size, newer.size(), std::span{newer});
    age.Read(62, buffer);
    Require((buffer == std::array{'A', 'B', 'W', 'X'}) && (age.Size == filesystem.Size),
        "writes beyond the image must update promoted bytes without changing its size");
    age.Write(std::numeric_limits<std::uint64_t>::max(), first.size(), std::span{first});
    age.Read(std::numeric_limits<std::uint64_t>::max(), buffer);
    Require(buffer == std::array{'A', 'B', 'C', 'D'},
        "cache intersection must preserve bytes when an end offset would overflow");
    age.SetPhase(Phase::Finalize);
    age.SetPhase(Phase::Detached);
    Require(age.Finish(true).complete, "an acknowledged write must not fail the capture");
    std::println("PASS rolling write cache: newest data, promotion, FIFO eviction and bounds");
}

auto Run(const std::span<const char *const> arguments) -> int {
    auto output = std::filesystem::path{"asr-image-trace-results"};
    for (auto index = 1uz; index < arguments.size(); ++index) {
        const auto argument = std::string_view{arguments[index]};
        if (argument == "--help") {
            std::println("Usage: asr-image-trace-synthetic [--output-root DIRECTORY]\n"
                "Create fixtures, test serial and jump-around policies, and retain traces.");
            return 0;
        }
        if ((argument != "--output-root") || (++index == arguments.size())) {
            throw std::invalid_argument{
                std::format("invalid or incomplete option '{}'", argument)};
        }
        output = arguments[index];
    }
    const auto directory = CreateRunDirectory(output);
    std::println("Results: {}", directory.string());
    // The final short block verifies that neither policy assumes a block-aligned image.
    constexpr auto size = 4 * 1024 * 1024 + 137;
    auto filesystem = SyntheticFilesystem{size};
    CheckPolicy<SerialWriting>(filesystem, directory, "synthetic-serial");
    CheckPolicy<JumpAroundWriting>(filesystem, directory, "synthetic-jump-around");
    CheckBoundaries(filesystem);
    CheckFinalization(directory);
    CheckDiscardedWrites(directory);
    CheckReads(directory);
    CheckIncompleteCapture(directory);
    CheckReadbackCache(directory);
    CheckRollingWriteCache(directory);
    return 0;
}

} // namespace

auto main(const int argc, char *argv[]) -> int {
    try {
        return Run({argv, devicefs::terminal::FailFastCast<std::size_t>(argc)});
    } catch (const std::invalid_argument &error) {
        std::println(std::cerr, "asr-image-trace-synthetic: {}", error.what());
        return 2;
    } catch (const std::exception &error) {
        std::println(std::cerr, "asr-image-trace-synthetic: {}", error.what());
        return 1;
    }
}
