// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "../compat/gsl_suppress.h"

#ifdef _WIN32
    #include <devicefs/strsafe_compat.h>
#else
    #include <cerrno>
    #include <sys/ioctl.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <unistd.h>
    #ifdef __APPLE__
        #include <util.h>
    #else
        #include <pty.h>
    #endif
#endif

export module devicefs.terminal.native_tests;

import std;
import devicefs.terminal;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.scope_exit;
import devicefs.terminal.test_support;

#ifdef _WIN32
    import <windows.h>;
    import <wil/resource.h>;
    import devicefs.terminal.windows;
#else
    import devicefs.terminal.unix;
#endif

using namespace std::string_view_literals;
using namespace std::chrono_literals;
using namespace devicefs::terminal;
using namespace devicefs::terminal::tests;

namespace {

// These tests control the input of an isolated native console. TestConsole
// intercepts outgoing queries and supplies chosen replies through the operating
// system, exercising the real native reader and its queue. Intercepting output
// also prevents the console host from answering queries on the tests' behalf.
#ifdef _WIN32
using NativeConsole = WindowsConsole;

class NativeInput {
public:
    [[nodiscard]] auto Modes() const {
        auto modes = std::array<DWORD, 2>{};
        if (!GetConsoleMode(input_.get(), &modes[0]) || !GetConsoleMode(output_.get(), &modes[1])) {
            throw std::system_error(std::bit_cast<int>(GetLastError()), std::system_category(),
                "could not read the test console's input and output modes");
        }
        return modes;
    }

    auto FeedRecords(const std::span<const INPUT_RECORD> records) const -> void {
        auto written = DWORD{};
        if (!WriteConsoleInputW(input_.get(), records.data(),
                FailFastCast<DWORD>(records.size()), &written)) {
            throw std::system_error(std::bit_cast<int>(GetLastError()), std::system_category(),
                "could not supply test console events");
        }
        Require(written == records.size(), "console input accepted only some events"sv);
    }

    auto Feed(const std::string_view text) const -> void {
        const auto records = std::filesystem::path{text}.wstring() |
            std::views::transform([](const wchar_t character) {
                auto record = INPUT_RECORD{.EventType = KEY_EVENT};
                record.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                    .wVirtualKeyCode = 0, .wVirtualScanCode = 0,
                    .uChar = {.UnicodeChar = character}, .dwControlKeyState = 0};
                return record;
            }) | std::ranges::to<std::vector>();
        FeedRecords(records);
    }

private:
    wil::unique_hfile input_{CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    wil::unique_hfile output_{CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
};
#else
using NativeConsole = UnixConsole;

class NativeInput {
public:
    NativeInput(const int master, const int slave) noexcept : master_{master}, slave_{slave} {}

    [[nodiscard]] auto Modes() const {
        auto modes = termios{};
        if (tcgetattr(slave_, &modes) < 0) {
            throw std::system_error(errno, std::generic_category(), "could not read test terminal modes");
        }
#ifdef __APPLE__
        // These snapshots let the tests verify that destroying `UnixConsole`
        // restores the settings saved before construction. To receive individual
        // keystrokes, the console temporarily disables `ICANON`, the mode that
        // collects input one line at a time. When destruction restores `ICANON`,
        // macOS also sets `PENDIN` to request that queued input be reprocessed
        // under the restored rules. Excluding `PENDIN` from the snapshots
        // prevents that kernel-generated request from being reported as a
        // failure to restore the caller's settings.
        // https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/tty.c#L1358-L1391
        modes.c_lflag &= ~PENDIN;
#endif
        return std::tuple{modes.c_iflag, modes.c_oflag, modes.c_cflag, modes.c_lflag,
            cfgetispeed(&modes), cfgetospeed(&modes), std::to_array(modes.c_cc)};
    }

    auto Feed(const std::string_view text) const -> void {
        const auto count = write(master_, text.data(), text.size());
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "could not supply test terminal input");
        }
        Require(count == std::ssize(text), "could not supply the complete test input"sv);
    }

private:
    int master_;
    int slave_;
};
#endif

class TestConsole : public NativeConsole {
public:
    GSL_SUPPRESS("26434",
        "BaseConsole dispatches Write through its explicit object parameter. "
        "This test adapter replaces output so native queries receive only "
        "the replies supplied by the test.")
    auto Write(const std::string_view text) -> void {
        if (on_write) {
            on_write(text);
        }
    }

    std::function<void(std::string_view)> on_write;
};

class InjectedFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] auto TestNativeInput(const NativeInput &input) -> bool {
    const auto original_modes = input.Modes();
    auto passed = Test("native fragmented cursor reply with keys before and after it"sv, [&] {
        auto console = TestConsole{};
        auto remainder = std::future<void>{};
        console.on_write = [&](const auto text) {
            Require(text == detail::ReportRequest(detail::TerminalReport::Cursor),
                "unexpected query"sv);
            input.Feed("\x0c\x1b[4;"sv);
            remainder = std::async(std::launch::async, [&] {
                std::this_thread::sleep_for(10ms);
                input.Feed("9R1"sv);
            });
        };
        Require(console.QueryCursor() == CursorPosition{4, 9}, "fragmented reply was not recognized"sv);
        remainder.get();
        Require(console.ReadMenuInput().key == MenuKey::Redraw, "the preceding key was lost"sv);
        Require(console.ReadMenuInput().key == MenuKey::Details, "the following key was lost"sv);
    });
    passed &= Test("native cancellation interrupting a cursor query and preserving other keys"sv, [&] {
        auto console = TestConsole{};
        console.on_write = [&](const auto) { input.Feed("\x0c\x03"sv); };
        const auto start = std::chrono::steady_clock::now();
        try {
            std::ignore = console.QueryCursor();
            Require(false, "Ctrl+C did not cancel the query"sv);
        } catch (const InputCancelled &) {
            Require(std::chrono::steady_clock::now() - start < 2s,
                "cancellation waited for the query timeout"sv);
        }
        Require(console.ReadMenuInput().key == MenuKey::Redraw, "cancellation consumed another key"sv);
        console.on_write = [&](const auto) { input.Feed("\x1b[7;8R"sv); };
        Require(console.QueryCursor() == CursorPosition{7, 8}, "a later query inherited cancellation"sv);
    });
    passed &= Test("native menu cancellation during size and cursor queries"sv, [&] {
        for (const auto cancel_size : {true, false}) {
            auto console = TestConsole{};
            auto output = std::string{};
            console.on_write = [&](const auto text) {
                output.append(text);
                if (text == detail::ReportRequest(detail::TerminalReport::Size)) {
                    input.Feed(cancel_size ? "\x03"sv : "\x1b[8;24;80t"sv);
                } else if (text == detail::ReportRequest(detail::TerminalReport::Cursor)) {
                    input.Feed("\x03"sv);
                }
            };
            Require(!SelectMenuItem(console, {}, std::array{"Installation"sv}),
                "cancellation returned a selected entry"sv);
            Require(!output.contains("unavailable"sv), "cancellation painted recovery instructions"sv);
        }
    });
    passed &= Test("native cancellation queued after a completed report"sv, [&] {
        auto console = TestConsole{};
        console.on_write = [&](const auto) { input.Feed("\x1b[4;9R\x03"sv); };
        Require(console.QueryCursor() == CursorPosition{4, 9}, "the first report was lost"sv);
        console.on_write = {};
        const auto start = std::chrono::steady_clock::now();
        try {
            std::ignore = console.QueryCursor();
            Require(false, "a queued Ctrl+C did not cancel the next query"sv);
        } catch (const InputCancelled &) {
            Require(std::chrono::steady_clock::now() - start < 2s,
                "queued cancellation waited for the query timeout"sv);
        }
    });
    passed &= Test("native terminal modes restored after cancellation and an output exception"sv, [&] {
        Require(input.Modes() == original_modes, "normal exit or cancellation changed terminal modes"sv);
        try {
            auto console = TestConsole{};
            console.on_write = [](const auto) { throw InjectedFailure{"injected output failure"}; };
            std::ignore = SelectMenuItem(console, {}, std::array{"Installation"sv});
            Require(false, "the output exception did not propagate"sv);
        } catch (const InjectedFailure &) {
        }
        Require(input.Modes() == original_modes, "exception unwinding changed terminal modes"sv);
    });
    passed &= Test("native query timeout, a late reply, and subsequent successful query"sv, [&] {
        auto console = TestConsole{};
        const auto start = std::chrono::steady_clock::now();
        Require(!console.QueryCursor(), "a missing report produced a cursor position"sv);
        Require(std::chrono::steady_clock::now() - start < 8s, "the query exceeded its deadline"sv);
        input.Feed("\x1b[1;1R\x0c"sv);
        Require(console.ReadMenuInput().key == MenuKey::Redraw,
            "a late report was interpreted as a menu shortcut"sv);
        console.on_write = [&](const auto) { input.Feed("\x1b[2;3R"sv); };
        Require(console.QueryCursor() == CursorPosition{2, 3}, "a query could not recover after timeout"sv);
    });
#ifdef _WIN32
    passed &= Test("Windows console host emitting character-only cursor-reply events"sv, [] {
        auto console = NativeConsole{};
        console.Write(detail::ReportRequest(detail::TerminalReport::Cursor));
        auto reader = detail::ReportReader<std::size_t>{detail::TerminalReport::Cursor};
        for (auto length = 0uz; length < detail::kMaximumReportLength;) {
            const auto record = console.ReadInput();
            if ((record.EventType != KEY_EVENT) || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            Require(record.Event.KeyEvent.wVirtualKeyCode == 0,
                "the console host gave a report character a physical-key code"sv);
            if (reader.Push(record.Event.KeyEvent.uChar.UnicodeChar, length++)) {
                return;
            }
        }
        Require(false, "the console host did not supply a complete cursor report"sv);
    });
    passed &= Test("Windows report removal preserving repeat counts, Unicode, and resize events"sv, [&] {
        auto console = TestConsole{};
        auto key = INPUT_RECORD{.EventType = KEY_EVENT};
        key.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 3,
            .wVirtualKeyCode = VK_DOWN, .wVirtualScanCode = 80,
            .uChar = {.UnicodeChar = L'界'}, .dwControlKeyState = SHIFT_PRESSED};
        auto resize = INPUT_RECORD{.EventType = WINDOW_BUFFER_SIZE_EVENT};
        resize.Event.WindowBufferSizeEvent.dwSize = {90, 30};
        console.on_write = [&](const auto) {
            input.Feed("\x1b[4;"sv);
            input.FeedRecords(std::array{key, resize});
            input.Feed("9R"sv);
        };
        Require(console.QueryCursor() == CursorPosition{4, 9}, "interspersed events disrupted the report"sv);
        const auto retained = console.ReadInput().Event.KeyEvent;
        Require((retained.bKeyDown == key.Event.KeyEvent.bKeyDown) &&
            (retained.wRepeatCount == 3) && (retained.wVirtualKeyCode == VK_DOWN) &&
            (retained.wVirtualScanCode == 80) && (retained.uChar.UnicodeChar == L'界') &&
            (retained.dwControlKeyState == SHIFT_PRESSED), "the saved key event changed"sv);
        Require(console.ReadMenuInput().key == MenuKey::Resize, "the resize event was lost"sv);
        input.Feed("\x1b[1;"sv);
        input.FeedRecords(std::array{key});
        input.Feed("1R\x0c"sv);
        const auto navigation = console.ReadMenuInput();
        Require((navigation.key == MenuKey::Down) && (navigation.repeat == 3),
            "an unfinished late reply consumed physical navigation"sv);
        Require(console.ReadMenuInput().key == MenuKey::Redraw,
            "the remainder of a late reply became a shortcut after navigation"sv);
    });
#else
    passed &= Test("Unix fragmented navigation and a lone Escape"sv, [&] {
        auto console = TestConsole{};
        input.Feed("\x1b["sv);
        auto remainder = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(10ms);
            input.Feed("B"sv);
        });
        Require(console.ReadMenuInput().key == MenuKey::Down, "a fragmented arrow became Escape"sv);
        remainder.get();
        input.Feed("\x1b"sv);
        Require(console.ReadMenuInput().key == MenuKey::Back, "a lone Escape did not produce Back"sv);
    });
#endif
    return passed;
}

}

export auto RunNativeTests() -> int {
#ifdef _WIN32
    // The CTest launcher gives this process a hidden, private Windows console.
    return TestNativeInput(NativeInput{}) ? 0 : 1;
#else
    // A child session owns this pseudoterminal as /dev/tty. Standard output and
    // error remain connected to CTest, so test results contain no drawing codes.
    // The parent retains the terminal until the child has restored its modes.
    auto descriptors = std::array<int, 2>{};
    auto size = winsize{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    if (openpty(&descriptors[0], &descriptors[1], nullptr, nullptr, &size) < 0) {
        throw std::system_error(errno, std::generic_category(), "could not create the test pseudoterminal");
    }
    const auto connection = ScopeExit{[descriptors] {
        for (const auto descriptor : descriptors) {
            std::ignore = close(descriptor);
        }
    }};
    const auto child = fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "could not create the native test process");
    }
    if (child == 0) {
        const auto passed = Test("isolated Unix native input tests"sv, [&] {
            if (setsid() < 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not create the test terminal session");
            }
            if (ioctl(descriptors[1], TIOCSCTTY, 0) < 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not attach the test controlling terminal");
            }
            // These tests intercept ordinary output with `TestConsole::Write`,
            // but screen-restoration guards write directly to the pseudoterminal.
            // Nothing reads those restoration sequences. macOS waits for the
            // session leader's terminal output to drain during process exit,
            // so the child would wait forever while the parent waits in `waitpid`.
            // This guard discards the unused output after all test consoles have
            // been destroyed, including when a test throws.
            // https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_exit.c#L2286-L2332
            const auto discard_output = ScopeExit{[slave = descriptors[1]] {
                std::ignore = tcflush(slave, TCOFLUSH);
            }};
            Require(TestNativeInput(NativeInput{descriptors[0], descriptors[1]}),
                "one or more native input tests failed"sv);
        });
        return passed ? 0 : 1;
    }
    auto status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                "could not collect the native test process");
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
