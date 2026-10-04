// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#ifndef _WIN32
    #include <cerrno>
    #include <dlfcn.h>
    #include <sys/select.h>
#endif

export module devicefs.terminal.test_clock;

import std;
import devicefs.terminal;
import devicefs.terminal.safecast;
#ifdef _WIN32
    import <windows.h>;
#endif

using namespace std::chrono_literals;

export namespace devicefs::terminal::tests {

enum class WaitPolicy { Native, AdvanceClock, Block };

// Time advances at a controlled native wait, not after a number of `now()`
// calls. Queued keys and incidental resize events therefore leave time alone.
// Live Console Host queries instead block for their actual replies; the outer
// CTest timeout detects a broken host without a shorter scheduling assumption.
struct TestClock {
    using rep = std::chrono::steady_clock::rep;
    using period = std::chrono::steady_clock::period;
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<TestClock>;
    static constexpr auto is_steady = false;

    [[nodiscard]] static auto now() noexcept -> time_point { return current; }

    static inline thread_local auto current = time_point{};
    static inline thread_local auto wait_policy = WaitPolicy::Native;
};
static_assert(Clock<TestClock>);

#ifndef _WIN32
struct InterruptedWait {
    std::array<std::chrono::nanoseconds, 2> timeouts{};
    std::size_t calls = 0;
};

inline thread_local auto interrupted_wait = std::optional<InterruptedWait>{};
#endif

}

namespace {

// The native call still receives queued input and reports real I/O failures.
// Only an empty finite wait advances the clock instead of sleeping.
[[nodiscard]] auto SimulateWait(
    const std::optional<std::chrono::nanoseconds> remaining,
    const auto timed_out, const auto &wait) noexcept {
    using namespace devicefs::terminal::tests;
    if (TestClock::wait_policy == WaitPolicy::Native) {
        return wait(remaining);
    }
    if (!remaining || (TestClock::wait_policy == WaitPolicy::Block)) {
        return wait(std::nullopt);
    }
    const auto result = wait(0ns);
    if (result == timed_out) {
        TestClock::current +=
            std::chrono::ceil<TestClock::duration>(*remaining);
    }
    return result;
}

#ifdef _WIN32
auto WINAPI ControlledWindowsWait(const HANDLE handle, const DWORD timeout)
    noexcept -> DWORD {
    static const auto original = [] {
        const auto module = GetModuleHandleW(L"kernel32.dll");
        if (!module) {
            std::abort();
        }
        [[gsl::suppress("26490", justification:
            "`GetProcAddress` returns the address of the requested function "
            "but with an incorrect return type.")]]
        const auto function = reinterpret_cast<decltype(&WaitForSingleObject)>(
            GetProcAddress(module, "WaitForSingleObject"));
        if (!function) {
            std::abort();
        }
        return function;
    }();
    const auto remaining = (timeout == INFINITE) ? std::nullopt :
        std::optional{std::chrono::nanoseconds{
            std::chrono::milliseconds{timeout}}};
    return SimulateWait(remaining,
        devicefs::terminal::CompileTimeCast<DWORD, WAIT_TIMEOUT>(),
        [handle](const std::optional<std::chrono::nanoseconds> duration) {
            const auto milliseconds = duration ?
                devicefs::terminal::FailFastCast<DWORD>(
                    std::chrono::ceil<std::chrono::milliseconds>(
                        *duration).count()) :
                INFINITE;
            return original(handle, milliseconds);
        });
}
#else
[[nodiscard]] auto FindPselect(const char *const symbol) -> decltype(&pselect) {
    const auto address = dlsym(RTLD_NEXT, symbol);
    if (address == nullptr) {
        std::abort();
    }
    return reinterpret_cast<decltype(&pselect)>(address);
}

// A signal sender cannot guarantee that delivery occurs inside `pselect`.
// This test-executable replacement supplies `EINTR` on the first armed wait
// and records the timeout on its retry. The interruption consumes the remaining
// simulated time, so the retry must poll against the original deadline.
[[nodiscard]] auto Wait(const decltype(&pselect) original,
    const int count, fd_set *const readable,
    fd_set *const writable, fd_set *const exceptional,
    const timespec *const timeout, const sigset_t *const mask) -> int {
    using namespace devicefs::terminal::tests;
    if (interrupted_wait) {
        auto &wait = *interrupted_wait;
        if (wait.calls < wait.timeouts.size()) {
            wait.timeouts[wait.calls] = std::chrono::seconds{timeout->tv_sec} +
                std::chrono::nanoseconds{timeout->tv_nsec};
        }
        if (wait.calls++ == 0) {
            TestClock::current += std::chrono::ceil<TestClock::duration>(
                std::chrono::seconds{timeout->tv_sec} +
                std::chrono::nanoseconds{timeout->tv_nsec});
            errno = EINTR;
            return -1;
        }
    }
    const auto remaining = timeout ? std::optional{
        std::chrono::seconds{timeout->tv_sec} +
            std::chrono::nanoseconds{timeout->tv_nsec}} : std::nullopt;
    return SimulateWait(remaining, 0,
        [original, count, readable, writable, exceptional, mask](
            const std::optional<std::chrono::nanoseconds> duration) {
            const auto native_timeout = duration.transform(
                [](const auto value) {
                    const auto seconds =
                        std::chrono::duration_cast<std::chrono::seconds>(value);
                    return timespec{.tv_sec = seconds.count(),
                        .tv_nsec = (value - seconds).count()};
                });
            return original(count, readable, writable, exceptional,
                native_timeout ? &*native_timeout : nullptr, mask);
        });
}
#endif

}

#ifdef _WIN32
// MSVC's `dllimport` calls use `__imp_WaitForSingleObject` on both supported
// Windows architectures. Supplying that import pointer in the test executable
// redirects those calls without modifying the production console or the DLL.
// The replacement obtains the real function directly from `kernel32.dll`.
// https://learn.microsoft.com/cpp/build/importing-function-calls-using-declspec-dllimport
// Retain the replacement import pointer during whole-program optimization.
#pragma comment(linker, "/include:__imp_WaitForSingleObject")
extern "C" {
    decltype(&WaitForSingleObject) __imp_WaitForSingleObject =
        &ControlledWindowsWait;
}
#else
// On macOS, the build's `_DARWIN_UNLIMITED_SELECT` definition selects
// `pselect$DARWIN_EXTSN` for both the console's call and this replacement.
// On other Unix platforms, both use the ordinary `pselect` symbol instead.
extern "C" auto pselect(const int count, fd_set *const readable,
    fd_set *const writable, fd_set *const exceptional,
    const timespec *const timeout, const sigset_t *const mask) -> int {
#ifdef __APPLE__
    static const auto original = FindPselect("pselect$DARWIN_EXTSN");
#else
    static const auto original = FindPselect("pselect");
#endif
    return Wait(original, count, readable, writable, exceptional,
        timeout, mask);
}
#endif
