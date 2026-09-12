// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../compat/forceinline_compat.h"
#include "../../compat/gsl_suppress.h"

import std;
import devicefs.terminal.transcoding_benchmark;
using namespace devicefs::terminal;
using namespace std::string_view_literals;
auto Observe(const void *, std::size_t) noexcept -> void;

namespace {

struct Options {
    std::uint32_t seed = 20260912;
    std::string_view processor = "unavailable"sv;
    bool retry_only = false;
    bool verify_only = false;
};

enum class Work { Control, Count, AllocateExact, AllocateWorst, ConvertOnly,
    Exact, Worst, Hybrid, StringExact, StringWorst, RetryExact, RetryWorst };
constexpr auto names = std::array{"control"sv, "count"sv, "alloc_exact"sv, "alloc_worst"sv,
    "convert_only"sv, "exact"sv, "worst"sv, "hybrid"sv, "string_exact"sv, "string_worst"sv,
    "retry_exact"sv, "retry_worst"sv};

template <Work Operation, typename Output, typename Input>
auto Exercise(const std::basic_string_view<Input> input, const std::size_t exact,
    const std::size_t bound, const std::span<Output> buffer)
    noexcept(Operation == Work::Control || Operation == Work::Count) -> void {
    // The timed operations call conversion helpers directly. Applying the same
    // inlining hint as `Transcode` makes their call overhead representative of
    // production conversions when comparing the sizing policies.
    ATTRIBUTE_MSVC_FLATTEN
    if constexpr (Operation == Work::Control) {
        Observe(input.data(), input.size());
    } else if constexpr (Operation == Work::Count) {
        Observe(input.data(), Count<Output>(input));
    } else if constexpr (Operation == Work::AllocateExact || Operation == Work::AllocateWorst) {
        const auto allocated = std::make_unique_for_overwrite<Output[]>(
            (Operation == Work::AllocateExact ? exact : bound) + 1);
        Observe(allocated.get(), (Operation == Work::AllocateExact ? exact : bound) + 1);
    } else if constexpr (Operation == Work::ConvertOnly) {
        const auto count = input.empty() ? 0 : Convert(input, buffer);
        buffer[count] = Output{};
        Observe(buffer.data(), count);
    } else if constexpr (Operation == Work::StringExact || Operation == Work::StringWorst) {
        const auto result = StringResult<Operation == Work::StringExact ? Sizing::Exact : Sizing::Worst, Output>(input);
        Observe(result.data(), result.size());
    } else {
        constexpr auto policy = Operation == Work::RetryExact ? Sizing::RetryExact :
            Operation == Work::RetryWorst ? Sizing::RetryWorst :
            Operation == Work::Exact ? Sizing::Exact :
            Operation == Work::Worst ? Sizing::Worst : Sizing::Hybrid;
        const auto result = TranscodedText<Output, policy>{input};
        Observe(result.data(), result.size());
    }
}

template <typename Output, typename Input>
auto RunCase(const std::string_view direction, const std::string_view pattern,
    const std::basic_string_view<Input> input, std::mt19937 &random,
    const Options options) -> void {
    const auto exact = Count<Output>(input);
    const auto bound = WorstCaseCapacity<Output>(input);
    auto buffer = std::vector<Output>(bound + 1);
    const auto reference = TranscodedText<Output>{input};
    const auto worst = TranscodedText<Output, Sizing::Worst>{input};
    const auto hybrid = TranscodedText<Output, Sizing::Hybrid>{input};
    const auto same = [&](const auto &value) {
        return std::basic_string_view<Output>{value} == std::basic_string_view<Output>{reference};
    };
    if (reference.size() != exact || !same(worst) || !same(hybrid) ||
        !same(StringResult<Sizing::Exact, Output>(input)) ||
        !same(StringResult<Sizing::Worst, Output>(input))) {
        throw std::runtime_error("sizing policies produced different output");
    }
    if constexpr (sizeof(Input) == 2 && sizeof(Output) == 1) {
        if (!same(TranscodedText<Output, Sizing::RetryExact>{input}) ||
            !same(TranscodedText<Output, Sizing::RetryWorst>{input})) {
            throw std::runtime_error("retry policies produced different output");
        }
    }
    if (options.verify_only) {
        return;
    }
    const auto functions = [] {
        const auto make_operations = [](const auto... retry_operations) {
            return std::to_array<decltype(&Exercise<Work::Exact, Output, Input>)>({
                &Exercise<Work::Control, Output, Input>, &Exercise<Work::Count, Output, Input>,
                &Exercise<Work::AllocateExact, Output, Input>, &Exercise<Work::AllocateWorst, Output, Input>,
                &Exercise<Work::ConvertOnly, Output, Input>, &Exercise<Work::Exact, Output, Input>,
                &Exercise<Work::Worst, Output, Input>, &Exercise<Work::Hybrid, Output, Input>,
                &Exercise<Work::StringExact, Output, Input>, &Exercise<Work::StringWorst, Output, Input>,
                retry_operations...});
        };
        if constexpr (sizeof(Input) == 2 && sizeof(Output) == 1) {
            return make_operations(&Exercise<Work::RetryExact, Output, Input>,
                &Exercise<Work::RetryWorst, Output, Input>);
        } else {
            return make_operations();
        }
    }();
    const auto operation_count = functions.size();
    auto iterations = std::array<std::size_t, names.size()>{};
    auto samples = std::array<std::array<double, 7>, names.size()>{};
    // Each iteration reloads the input address from a volatile object. Counting
    // therefore cannot be hoisted out of the loop as a repeated pure expression.
    const Input *volatile input_address = input.data();
    const auto time = [&](const std::size_t operation, const std::size_t count) {
        const auto start = std::chrono::steady_clock::now();
        for (auto iteration = 0uz; iteration < count; ++iteration) {
            functions.at(operation)({input_address, input.size()}, exact, bound, buffer);
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    };
    for (auto operation = 0uz; operation < operation_count; ++operation) {
        auto count = 16uz;
        auto elapsed = time(operation, count);
        while (elapsed < 100000.0) {
            count *= 2;
            elapsed = time(operation, count);
        }
        iterations.at(operation) = std::max(1uz, static_cast<std::size_t>(count * 750000.0 / elapsed));
    }
    auto order = std::vector<std::size_t>(operation_count);
    std::iota(order.begin(), order.end(), 0uz);
    for (auto round = 0uz; round < 7; ++round) {
        std::shuffle(order.begin(), order.end(), random);
        for (const auto operation : order) {
            const auto count = iterations.at(operation);
            samples.at(operation).at(round) = time(operation, count) / count;
        }
    }
    for (auto operation = 0uz; operation < operation_count; ++operation) {
        auto sorted = samples.at(operation);
        std::ranges::sort(sorted);
        // Quoting the processor field keeps commas and quotes in a supplied
        // display name from changing the CSV's columns.
        const auto processor = [&] {
            auto escaped = std::string{};
            for (const auto character : options.processor) {
                escaped += character;
                if (character == '"') { escaped += '"'; }
            }
            return escaped;
        }();
        std::println("{},{},{},{},\"{}\",{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f}",
            BENCHMARK_COMPILER, BENCHMARK_ARCHITECTURE, BENCHMARK_CONFIGURATION,
            BenchmarkImplementation(), processor, options.seed, direction, pattern,
            input.size(), exact, bound, names.at(operation), iterations.at(operation), sorted[3], sorted[1], sorted[5]);
    }
}

auto RunBenchmark(const Options options) -> void {
    auto random = std::mt19937{options.seed};
    const auto patterns = std::array{"ascii"sv, "mostly_ascii"sv, "latin"sv, "cjk"sv, "emoji"sv, "mixed"sv};
    const auto lengths = std::array{0uz, 1uz, 5uz, 16uz, 31uz, 63uz, 64uz, 65uz, 84uz,
        85uz, 86uz, 100uz, 127uz, 128uz, 129uz, 170uz, 254uz, 255uz, 256uz, 257uz,
        512uz, 1436uz, 4096uz, 65536uz, 1048576uz};
    auto cases = std::vector<std::pair<std::string_view, std::size_t>>{};
    for (const auto pattern : patterns) {
        for (const auto length : lengths) { cases.emplace_back(pattern, length); }
    }
    std::shuffle(cases.begin(), cases.end(), random);
    if (!options.verify_only) {
        std::println("compiler,architecture,configuration,simdutf,processor,seed,direction,pattern,"
            "input_units,output_units,worst_units,operation,iterations,median_ns,p14_ns,p86_ns");
    }
    GSL_SUPPRESS("26445",
        "This structured binding copies the pair, including its string view. "
        "C26445 incorrectly diagnoses a reference to the view even though "
        "the declaration uses `const auto`, without `&`.")
    for (const auto [pattern, length] : cases) {
        auto points = std::basic_string<BenchmarkUtf32>{};
        points.reserve(length);
        for (auto index = 0uz; index < length; ++index) {
            const auto point = [&]() -> char32_t {
                if (pattern == "ascii") { return U"abcdefghijklmnopqrstuvwxyz"sv[index % 26]; }
                if (pattern == "mostly_ascii") { return index % 32 == 31 ? U'é' : U"abcdefghijklmnopqrstuvwxyz"sv[index % 26]; }
                if (pattern == "latin") { return U'é'; }
                if (pattern == "cjk") { return U'界'; }
                if (pattern == "emoji") { return U'\U0001f600'; }
                constexpr auto mixed = U"path/café/日本語/é/👩‍💻/2026-09-12/"sv;
                return mixed[index % mixed.size()];
            }();
            points.push_back(point);
        }
        const auto utf32 = std::basic_string_view{points};
        const auto converted = Transcode<BenchmarkUtf16>(utf32);
        const auto utf16 = std::basic_string_view<BenchmarkUtf16>{converted};
        RunCase<char>("16to8"sv, pattern, utf16, random, options);
        if (options.retry_only) {
            continue;
        }
        RunCase<char>("32to8"sv, pattern, utf32, random, options);
        RunCase<BenchmarkUtf16>("32to16"sv, pattern, utf32, random, options);
    }
}

}

auto main(const int argc, char **argv) -> int {
    try {
        auto options = Options{};
        const auto arguments = std::span(argv, argc).subspan(1);
        for (auto index = 0uz; index < arguments.size(); ++index) {
            const auto argument = std::string_view{arguments[index]};
            if (argument == "--help"sv) {
                std::println("Usage: devicefs-transcoding-benchmark [--seed N] [--processor NAME] [--retry-only] [--verify-only]\n"
                    "Compare transcoding capacity policies; write measurements as CSV.\n"
                    "  --seed N       Set the case/operation shuffle seed (default 20260912).\n"
                    "  --processor NAME  Record the processor name supplied by Run.cmake.\n"
                    "  --retry-only   Limit the comparison to UTF-16 to UTF-8.\n"
                    "  --verify-only  Check equal output across policies without timing them.");
                return 0;
            }
            if (argument == "--retry-only"sv) {
                options.retry_only = true;
            } else if (argument == "--verify-only"sv) {
                options.verify_only = true;
            } else if ((argument == "--processor"sv) && (++index < arguments.size())) {
                options.processor = arguments[index];
            } else if ((argument == "--seed"sv) && (++index < arguments.size())) {
                const auto value = std::string_view{arguments[index]};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.seed);
                if ((parsed.ec != std::errc{}) || (parsed.ptr != value.data() + value.size())) {
                    throw std::invalid_argument("--seed requires an unsigned 32-bit integer");
                }
            } else {
                throw std::invalid_argument(std::format("unrecognized or incomplete option: {}", argument));
            }
        }
        RunBenchmark(options);
        if (options.verify_only) {
            std::println("All transcoding capacity policies produced equal output.");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << std::format("Transcoding benchmark failed: {}\n", error.what());
        return 1;
    }
}
