// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../compat/forceinline_compat.h"
#include "../../compat/gsl_suppress.h"

#ifdef __GNUC__
    #define ATTRIBUTE_FLATTEN_FUNCTION __attribute__((flatten))
#else
    #define ATTRIBUTE_FLATTEN_FUNCTION
#endif

// These annotations let the compiler optimize accesses to newly allocated
// storage independently of accesses to existing objects, as it can with the
// ordinary allocator. `allocation_pointers` retains the backing addresses only
// for batch cleanup to pass to `free` after the caller finishes using them.
// Nothing reads or writes the allocated contents through those saved pointers,
// so retaining them does not introduce a competing access to the caller's data.
//
// Microsoft's `__declspec(restrict)` documentation illustrates this distinction
// with an allocator that retains a global pointer to its backing memory pool
// while returning separately usable regions of that pool:
// https://learn.microsoft.com/en-us/cpp/cpp/restrict?view=msvc-170#example
//
// Clang translates `[[gnu::malloc]]` into a `noalias` return attribute. LLVM
// uses the resulting alias information to skip stores that cannot modify the
// memory being read; the relevant independence concerns the memory accesses,
// rather than the mere existence of another copy of the address:
// https://github.com/llvm/llvm-project/blob/483af6c1be7ca13eda7cc5314bb93185d21b272a/clang/lib/CodeGen/CGCall.cpp#L2844-L2846
// https://github.com/llvm/llvm-project/blob/483af6c1be7ca13eda7cc5314bb93185d21b272a/llvm/lib/Analysis/MemoryDependenceAnalysis.cpp#L581-L598
//
// GCC likewise represents a malloc-like result as a distinct heap object in
// its points-to analysis. That analysis treats `free` separately from reading
// or writing the allocation's contents, matching our use of the saved pointers
// solely for deferred cleanup:
// https://github.com/gcc-mirror/gcc/blob/1c1cf87e07567288d09c5ec07e32d05a37d5faa3/gcc/gimple-ssa-pta-constraints.cc#L1591-L1616
// https://github.com/gcc-mirror/gcc/blob/1c1cf87e07567288d09c5ec07e32d05a37d5faa3/gcc/gimple-ssa-pta-constraints.cc#L2670-L2674
#ifdef _MSC_VER
    #define ALLOCATION_ANNOTATIONS \
        _Ret_notnull_ _Post_writable_byte_size_(bytes) \
        __declspec(restrict) __declspec(allocator)
#else
    #define ALLOCATION_ANNOTATIONS [[gnu::malloc]]
#endif

import std;
#ifdef _MSC_VER
    import <sal.h>;
#endif
import devicefs.terminal.transcoding_benchmark;
import devicefs.terminal.scope_exit;
using namespace devicefs::terminal;
using namespace std::string_view_literals;
auto Observe(const void *, std::size_t) noexcept -> void;

namespace {

#ifdef BENCHMARK_DEFER_DEALLOCATION
constexpr auto kDeallocationPolicy = "deferred"sv;
// Each allocation occupies one slot until batch cleanup. Earlier allocations,
// including the input fixtures, remain recorded while later batches run.
void *allocation_pointers[1'048'576]{};
#else
constexpr auto kDeallocationPolicy = "immediate"sv;
#endif
auto next_allocation_index = 0uz;

// A batch frees only allocations recorded since its starting index, leaving
// its input fixtures available for subsequent measurements.
// Cleanup is skipped when an exception escapes because the exception may own
// an allocation that the outer handler still needs to read.
[[nodiscard]] auto FreeNewAllocationsOnExit() noexcept {
#ifdef BENCHMARK_DEFER_DEALLOCATION
    return ScopeExit{[first = next_allocation_index,
                         exceptions = std::uncaught_exceptions()] {
        if (std::uncaught_exceptions() != exceptions) {
            return;
        }
        for (void *const pointer : std::span{allocation_pointers}.subspan(
                first, next_allocation_index - first)) {
            GSL_SUPPRESS("26408",
                "The recorded blocks came directly from `malloc`; `free` releases them "
                "without invoking the empty replacement `operator delete`.")
            std::free(pointer);
        }
        next_allocation_index = first;
    }};
#else
    return std::monostate{};
#endif
}

}

#ifdef BENCHMARK_DEFER_DEALLOCATION
#ifdef _MSC_VER
    // `std` declares these overloads without `__declspec(restrict)`, so MSVC
    // reports C4559 when our definitions add it. Our replacements return fresh
    // storage and retain addresses only for cleanup, as justified above;
    // strengthening the allocation promise is intentional.
    #pragma warning(push)
    #pragma warning(disable : 4559)
#endif
#if defined(__GNUC__) && !defined(__clang__)
    // GCC warns when `always_inline` is used without an `inline` declaration.
    // Replacement allocation functions must not be declared `inline`, so these
    // definitions use the optimization hint and suppress that warning locally.
    // https://gcc.gnu.org/pipermail/gcc-cvs/2024-January/397876.html
    // https://eel.is/c++draft/dcl.fct.def.replace
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wattributes"
#endif
#if defined(__APPLE__) && defined(__clang__)
    // String allocations inside shared libc++ must also use this replacement.
    // However, dyld's search for overrides of library weak symbols skips
    // binaries without the `MH_WEAK_DEFINES` flag, which this executable lacked.
    // An exported weak definition anywhere in the executable sets that flag;
    // it need not be `operator new`. An unrelated weak definition also fixed the
    // reproducer while leaving `operator new` strong. Applying the attribute here
    // supplies the executable-wide flag without adding a dummy definition, not
    // because `operator new` needs weaker precedence. See
    // `handleStrongWeakDefOverrides` and `resolveSymbol`:
    // https://github.com/apple-oss-distributions/dyld/blob/main/dyld/JustInTimeLoader.cpp
    // https://github.com/apple-oss-distributions/dyld/blob/main/dyld/Loader.cpp
    // libc++ uses the same workaround via `TEST_WORKAROUND_BUG_109234844_WEAK`,
    // which expands to `__attribute__((weak))` on Apple platforms:
    // https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/libcxx/test/std/language.support/support.dynamic/new.delete/new.delete.single/new.size_nothrow.replace.indirect.pass.cpp#L31
    // https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/libcxx/test/support/test_macros.h#L511
    [[gnu::weak]]
#endif
GSL_SUPPRESS("26408",
    "The replacement `operator new` obtains its backing storage from `malloc`; "
    "calling `new` would recurse.")
ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
ALLOCATION_ANNOTATIONS
auto operator new(const std::size_t bytes, const std::align_val_t alignment) -> void * {
    const auto boundary = std::to_underlying(alignment);
    // Extended alignment needs room to advance the address returned by `malloc`.
    // The list retains the original address so batch cleanup can release it
    // with `free`. Ordinary allocations request exactly `bytes` from `malloc`.
    const auto padding = boundary > __STDCPP_DEFAULT_NEW_ALIGNMENT__ ? boundary - 1 : 0;
    if ((bytes == 0) ||
        (next_allocation_index == std::size(allocation_pointers)) ||
        (bytes > (std::numeric_limits<std::size_t>::max() - padding))) [[unlikely]] {
        throw std::bad_alloc{};
    }
    auto space = bytes + padding;
    ATTRIBUTE_MSVC_FLATTEN
    if (auto pointer = std::malloc(space)) [[likely]] {
        allocation_pointers[next_allocation_index++] = pointer;
        return (padding == 0) ? pointer : std::align(boundary, bytes, pointer, space);
    }
    throw std::bad_alloc{};
}

ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
ALLOCATION_ANNOTATIONS
auto operator new(const std::size_t bytes) -> void * {
    ATTRIBUTE_MSVC_FLATTEN
    return ::operator new(bytes, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__});
}
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

// The standard library forwards array and `nothrow` forms to these scalar
// replacements. Sized deletion also forwards, but defining both sizes follows
// the standard's recommendation and avoids GCC's `-Wsized-deallocation` warning.
// https://eel.is/c++draft/new.delete.single
// https://eel.is/c++draft/new.delete.array
ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
auto operator delete(void *) noexcept -> void {}
ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
auto operator delete(void *, std::align_val_t) noexcept -> void {}
ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
auto operator delete(void *, std::size_t) noexcept -> void {}
ATTRIBUTE_FORCEINLINE ATTRIBUTE_FLATTEN_FUNCTION
auto operator delete(void *, std::size_t, std::align_val_t) noexcept -> void {}
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
#endif
#endif

namespace {

// Explicit calls exercise the library's forwarding overloads without allowing
// the compiler to omit allocations as it may with a `new` expression.
GSL_SUPPRESS("26401", "This test owns each block and calls its matching deletion overload.")
GSL_SUPPRESS("26407", "Heap allocation is the behavior being tested.")
GSL_SUPPRESS("26409",
    "Explicit calls select each overload to verify forwarding; "
    "a smart pointer would select the overload itself.")
auto VerifyAllocationForms(const auto... alignment) -> std::size_t {
    constexpr auto bytes = 513uz;
    const auto pointers = std::array{
        ::operator new(bytes, alignment...),
        ::operator new[](bytes, alignment...),
        ::operator new(bytes, alignment..., std::nothrow),
        ::operator new[](bytes, alignment..., std::nothrow),
    };
    const auto boundary = std::max({std::size_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
        std::to_underlying(alignment)...});
    for (void *const pointer : pointers) {
        auto aligned = pointer;
        auto space = bytes;
        if ((pointer == nullptr) || (std::align(boundary, bytes, aligned, space) != pointer)) {
            throw std::runtime_error("benchmark allocation returned incorrectly aligned storage");
        }
        std::ranges::fill(std::span{static_cast<std::byte *>(pointer), bytes}, std::byte{0x5a});
        Observe(pointer, bytes);
    }
    ::operator delete(pointers[0], alignment...);
    ::operator delete[](pointers[1], alignment...);
    ::operator delete(pointers[2], alignment..., std::nothrow);
    ::operator delete[](pointers[3], alignment..., std::nothrow);
#ifdef __cpp_sized_deallocation
    ::operator delete(::operator new(bytes, alignment...), bytes, alignment...);
    ::operator delete[](::operator new[](bytes, alignment...), bytes, alignment...);
    return pointers.size() + 2;
#else
    return pointers.size();
#endif
}

// Array and string allocations also exercise calls made from library code.
// Two batches verify that cleanup restores the starting index.
auto VerifyAllocationPolicy() -> void {
    for (auto batch = 0; batch < 2; ++batch) {
        const auto first = next_allocation_index;
        {
            [[maybe_unused]] const auto allocations = FreeNewAllocationsOnExit();
            const auto expected = 16 + VerifyAllocationForms() +
                VerifyAllocationForms(std::align_val_t{64}) +
                VerifyAllocationForms(std::align_val_t{4096});
            for (auto iteration = 0; iteration < 8; ++iteration) {
                const auto array = std::make_unique_for_overwrite<char[]>(512);
                const auto text = std::string(512, 'x');
                Observe(array.get(), 512);
                Observe(text.data(), text.size());
            }
            if (next_allocation_index - first !=
                (kDeallocationPolicy == "deferred"sv ? expected : 0uz)) {
                throw std::runtime_error("benchmark allocation policy did not record the expected allocations");
            }
        }
        if (next_allocation_index != first) {
            throw std::runtime_error("benchmark allocation cleanup did not restore the starting index");
        }
    }
    // An inner guard must leave an escaping exception's storage alive until
    // its handler finishes; the outer guard then releases the retained storage.
    [[maybe_unused]] const auto allocations = FreeNewAllocationsOnExit();
    const auto first = next_allocation_index;
    try {
        [[maybe_unused]] const auto batch = FreeNewAllocationsOnExit();
        throw std::runtime_error(std::string(512, 'x'));
    } catch (const std::runtime_error &error) {
        if ((kDeallocationPolicy == "deferred"sv) && (next_allocation_index == first)) {
            throw std::runtime_error("benchmark allocation cleanup ran during exception unwinding");
        }
        if (std::string_view{error.what()} != std::string(512, 'x')) {
            throw std::runtime_error("benchmark allocation cleanup invalidated the exception message");
        }
    }
}

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
#if defined(__GNUC__) && !defined(__clang__)
        // The `Observe` function does not read the bytes pointed to by its
        // pointer argument.
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
        Observe(allocated.get(), (Operation == Work::AllocateExact ? exact : bound) + 1);
#if defined(__GNUC__) && !defined(__clang__)
        #pragma GCC diagnostic pop
#endif
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
        [[maybe_unused]] const auto allocations = FreeNewAllocationsOnExit();
        const auto start = std::chrono::steady_clock::now();
        for (auto iteration = 0uz; iteration < count; ++iteration) {
            functions.at(operation)({input_address, input.size()}, exact, bound, buffer);
        }
        const auto elapsed = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count();
        return elapsed;
    };
    // The full profile measures 4,800 operations seven times each. A 1.5 ms
    // sample budget totals about 50 seconds, leaving room for calibration and
    // fixture preparation within an approximately one-minute run. Calibration
    // lasts at least 0.5 ms to estimate iterations from a longer observation.
    for (auto operation = 0uz; operation < operation_count; ++operation) {
        auto count = 16uz;
        auto elapsed = time(operation, count);
        while (elapsed < 500'000.0) {
            count *= 2;
            elapsed = time(operation, count);
        }
        iterations.at(operation) = std::max(1uz, static_cast<std::size_t>(count * 1'500'000.0 / elapsed));
    }
    // Allocating operations perform the same number of conversions before
    // cleanup, so a faster policy does not accumulate more live allocations.
    // The smallest calibrated count sets that batch size. Faster operations
    // repeat whole batches to retain their measurement duration. Operations
    // that do not allocate can use their entire sample as one batch.
    const auto batch_sizes = [&] {
        auto sizes = iterations;
        const auto shared = std::ranges::min(std::span{iterations}.first(operation_count));
        for (auto operation = static_cast<std::size_t>(Work::AllocateExact);
            operation < operation_count; ++operation) {
            if (operation != static_cast<std::size_t>(Work::ConvertOnly)) {
                sizes.at(operation) = shared;
            }
        }
        return sizes;
    }();
    for (auto operation = 0uz; operation < operation_count; ++operation) {
        const auto batch_size = batch_sizes.at(operation);
        iterations.at(operation) =
            ((iterations.at(operation) + batch_size - 1) / batch_size) * batch_size;
    }
    auto order = std::vector<std::size_t>(operation_count);
    std::iota(order.begin(), order.end(), 0uz);
    for (auto round = 0uz; round < 7; ++round) {
        std::shuffle(order.begin(), order.end(), random);
        for (const auto operation : order) {
            const auto count = iterations.at(operation);
            const auto batch_size = batch_sizes.at(operation);
            auto elapsed = 0.0;
            for (auto completed = 0uz; completed < count; completed += batch_size) {
                elapsed += time(operation, batch_size);
            }
            samples.at(operation).at(round) = elapsed / count;
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
        std::println("{},{},{},{},\"{}\",{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{},{}",
            BENCHMARK_COMPILER, BENCHMARK_ARCHITECTURE, BENCHMARK_CONFIGURATION,
            BenchmarkImplementation(), processor, options.seed, direction, pattern,
            input.size(), exact, bound, names.at(operation), iterations.at(operation),
            sorted[3], sorted[1], sorted[5],
            kDeallocationPolicy, batch_sizes.at(operation));
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
            "input_units,output_units,worst_units,operation,iterations,median_ns,p14_ns,p86_ns,"
            "allocation_policy,batch_iterations");
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
        VerifyAllocationPolicy();
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
