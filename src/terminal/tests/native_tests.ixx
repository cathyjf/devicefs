// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "../compat/gsl_suppress.h"

#ifdef _WIN32
    #include <devicefs/strsafe_compat.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <pthread.h>
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
import devicefs.terminal.transcoding;
import devicefs.terminal;
import devicefs.terminal.menu;
import devicefs.terminal.text_input;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.scope_exit;
import devicefs.terminal.test_support;
import devicefs.terminal.menu_measurements;
import devicefs.terminal.vt;

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
    auto Disconnect() -> void {
        if (!FreeConsole()) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not detach the test process from its console");
        }
    }

    [[nodiscard]] auto Modes() const {
        auto modes = std::array<DWORD, 2>{};
        if (!GetConsoleMode(input_.get(), &modes[0]) || !GetConsoleMode(output_.get(), &modes[1])) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not read the test console's input and output modes");
        }
        return modes;
    }

    auto FeedRecords(const std::span<const INPUT_RECORD> records) const -> void {
        auto written = DWORD{};
        if (!WriteConsoleInputW(input_.get(), records.data(),
                FailFastCast<DWORD>(records.size()), &written)) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not supply test console events");
        }
        Require(written == records.size(), "console input accepted only some events"sv);
    }

    auto Feed(const std::string_view text) const -> void {
        const auto wide = Transcode<char16_t>(text);
        const auto records = std::u16string_view{wide} |
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
    NativeInput() = default;
    NativeInput(const NativeInput &) = delete;
    auto operator=(const NativeInput &) -> NativeInput & = delete;

    ~NativeInput() {
        // These tests intercept ordinary output with `TestConsole::Write`,
        // but screen-restoration guards write directly to the pseudoterminal.
        // Nothing reads those restoration sequences. macOS waits for the
        // session leader's terminal output to drain during process exit,
        // so the child would wait forever while the parent waits in `waitpid`.
        // Discard the unused output after all test consoles have been destroyed.
        // https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_exit.c#L2286-L2332
        std::ignore = tcflush(descriptors_[1], TCOFLUSH);
        for (const auto descriptor : descriptors_) {
            if (descriptor >= 0) {
                std::ignore = close(descriptor);
            }
        }
    }

    auto Attach() const -> void {
        if (setsid() < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not create the test terminal session");
        }
        if (ioctl(descriptors_[1], TIOCSCTTY, 0) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not attach the test controlling terminal");
        }
    }

    auto Disconnect() -> void {
        if (close(std::exchange(descriptors_[0], -1)) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not close the test pseudoterminal's master descriptor");
        }
    }

    [[nodiscard]] auto Modes() const {
        auto modes = termios{};
        if (tcgetattr(descriptors_[1], &modes) < 0) {
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
        const auto count = write(descriptors_[0], text.data(), text.size());
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "could not supply test terminal input");
        }
        Require(count == std::ssize(text), "could not supply the complete test input"sv);
    }

    // Read the expected output from the pseudoterminal's master, including
    // restoration commands that bypass `TestConsole::Write`. A short read keeps
    // collecting bytes until the complete expected output can be compared.
    [[nodiscard]] auto ReadOutput(const std::size_t length) const -> std::string {
        auto output = std::string(length, '\0');
        auto remaining = std::span{output};
        while (!remaining.empty()) {
            const auto count = read(descriptors_[0], remaining.data(), remaining.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                    "could not read test terminal output");
            }
            Require(count != 0, "the test terminal closed before completing its output"sv);
            remaining = remaining.subspan(FailFastCast<std::size_t>(count));
        }
        return output;
    }

private:
    std::array<int, 2> descriptors_ = [] {
        auto descriptors = std::array<int, 2>{};
        auto size = winsize{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
        if (openpty(&descriptors[0], &descriptors[1], nullptr, nullptr, &size) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not create the test pseudoterminal");
        }
        return descriptors;
    }();
};

[[nodiscard]] auto SetSignalHandlerScoped(const int signal, void (*handler)(int)) {
    auto action = (struct sigaction){};
    action.sa_handler = handler;
    std::ignore = sigemptyset(&action.sa_mask);
    auto previous = (struct sigaction){};
    if (sigaction(signal, &action, &previous) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not install the native test's signal handler");
    }
    return ScopeExit{[signal, previous] {
        std::ignore = sigaction(signal, &previous, nullptr);
    }};
}
volatile std::sig_atomic_t received_signal = 0;
#endif

class TestConsole : public NativeConsole {
public:
    GSL_SUPPRESS("26434",
        "BaseConsole dispatches Write through its explicit object parameter. "
        "This test adapter replaces output so native queries receive only "
        "the replies supplied by the test.")
    auto Write(const std::string_view text) -> void {
#ifdef _WIN32
        if (text == vt::kRequestKeyboardSupport) {
            if (on_keyboard_query) {
                on_keyboard_query();
            } else {
                // Tests concerned with other reports leave keyboard detection
                // pending while their own input and rendering proceed.
                NativeConsole::Write(text);
            }
            return;
        }
#endif
        if (on_write) {
            on_write(text);
        }
    }

    std::function<void(std::string_view)> on_write;
#ifdef _WIN32
    std::function<void()> on_keyboard_query;
#endif
};

class InjectedFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] auto TestNativeInput(NativeInput &input) -> bool {
    const auto original_modes = input.Modes();
    auto passed = true;
#ifdef _WIN32
    // These replies advertise disabled extensions, demonstrating that support
    // does not depend on an extension already being enabled by the shell.
    constexpr auto kKittyAvailable = "\x1b[?0u"sv;
    constexpr auto kXtermAvailable = "\x1b[>4;0m"sv;
    // Older Console Host versions answer primary DA locally. The remote
    // keyboard reply can arrive later, so DA must not end detection.
    constexpr auto kDeviceAttributes = "\x1b[?61;6;7;21;22;23;24;28;32;42c"sv;
    struct KeyboardCase {
        std::string_view name;
        std::string replies;
        std::string_view shift_enter;
    };
    for (const auto &test : std::array{
            KeyboardCase{"Windows detects Kitty after an early Console Host reply"sv,
                std::string{kKittyAvailable}, "\x1b[13;2u"sv},
            KeyboardCase{"Windows detects xterm after an early Console Host reply"sv,
                std::string{kXtermAvailable}, "\x1b[27;2;13~"sv},
            KeyboardCase{"Windows accepts successive keyboard-extension replies"sv,
                std::format("{}{}", kKittyAvailable, kXtermAvailable), "\x1b[13;2u"sv},
        }) {
        passed &= Test(test.name, [&] {
            auto console = TestConsole{};
            console.on_keyboard_query = [&input, kDeviceAttributes] { input.Feed(kDeviceAttributes); };
            // No keyboard reply is supplied until after screen entry and an
            // ordinary keystroke. Both operations must finish during detection.
            const auto screen = console.EnterScreen();
            input.Feed("a"sv);
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).character == U'a',
                "keyboard detection blocked ordinary text input"sv);
            Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
                "Console Host's reply prematurely disabled VT input"sv);
            input.Feed(test.replies.substr(0, 3));
            auto remainder = std::async(std::launch::async, [&input, &test] {
                std::this_thread::sleep_for(10ms);
                input.Feed(std::format("{}日{}z", test.replies.substr(3), test.shift_enter));
            });
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).character == U'日',
                "keyboard detection consumed ordinary text"sv);
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).key == MenuKey::Newline,
                "Shift+Enter was lost after detecting the keyboard extension"sv);
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).character == U'z',
                "keyboard detection reordered ordinary text"sv);
            Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
                "a supported keyboard extension lost VT input"sv);
        });
    }
    passed &= Test("Windows keyboard detection does not delay cancellation"sv, [&] {
        {
            auto console = TestConsole{};
            console.on_keyboard_query = [] {};
            const auto screen = console.EnterScreen();
            input.Feed("\x03"sv);
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).key == MenuKey::Cancel,
                "pending keyboard detection blocked Ctrl+C"sv);
        }
        Require(input.Modes() == original_modes,
            "cancelling keyboard detection left the console modes changed"sv);
    });
    passed &= Test("Windows falls back to native input after the keyboard-reply deadline"sv, [&] {
        auto console = TestConsole{};
        console.on_keyboard_query = [] {};
        const auto screen = console.EnterScreen();
        Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
            "VT input was disabled before the keyboard-reply deadline"sv);
        Require(console.ReadTextInput(std::chrono::steady_clock::now() + 6s).key == MenuKey::Timeout,
            "expiring keyboard detection produced a keystroke"sv);
        Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) == 0,
            "a terminal without replies retained VT input after the deadline"sv);
        for (const auto test : std::array{
                std::pair{0, MenuKey::Accept},
                std::pair{SHIFT_PRESSED, MenuKey::Newline},
                std::pair{LEFT_ALT_PRESSED, MenuKey::Newline},
                std::pair{RIGHT_ALT_PRESSED, MenuKey::Newline}}) {
            auto record = INPUT_RECORD{.EventType = KEY_EVENT};
            record.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                .wVirtualKeyCode = VK_RETURN, .wVirtualScanCode = 0,
                .uChar = {.UnicodeChar = L'\r'}, .dwControlKeyState = FailFastCast<DWORD>(test.first)};
            input.FeedRecords(std::span{&record, 1});
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).key == test.second,
                "the native Return modifier selected the wrong action"sv);
        }
    });
#endif
    passed &= Test("native input deadlines and Tab focus changes"sv, [&] {
        auto console = TestConsole{};
        Require(console.ReadMenuInput(std::chrono::steady_clock::now() + 5ms).key == MenuKey::Timeout,
            "idle input did not reach its deadline"sv);
        input.Feed("\t"sv);
        Require(console.ReadMenuInput().key == MenuKey::SwitchArea, "Tab did not change area"sv);
        input.Feed("\x03"sv);
        Require(console.ReadMenuInput(std::chrono::steady_clock::now() + 100ms).key == MenuKey::Cancel,
            "a bounded input wait lost cancellation"sv);
    });
    passed &= Test("native fragmented keys surviving an earlier refresh deadline"sv, [&] {
        auto console = TestConsole{};
        // CSI B is Down, CSI Z is Shift+Tab, and kitty's CSI 27 u is Escape.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
        input.Feed("\x1b["sv);
        Require(console.ReadMenuInput(std::chrono::steady_clock::now() + 1ms).key == MenuKey::Timeout,
            "a refresh deadline turned a partial arrow into Escape"sv);
        input.Feed("B\x1b[Z\x1b[27u"sv);
        Require(console.ReadMenuInput().key == MenuKey::Down, "a refresh deadline lost the arrow"sv);
        Require(console.ReadMenuInput().key == MenuKey::SwitchArea, "Shift+Tab did not change area"sv);
        Require(console.ReadMenuInput().key == MenuKey::Back, "kitty Escape was not decoded"sv);
        input.Feed("\x1b"sv);
        Require(console.ReadMenuInput(std::chrono::steady_clock::now() + 1ms).key == MenuKey::Timeout,
            "a refresh deadline prematurely consumed a lone Escape"sv);
        Require(console.ReadMenuInput().key == MenuKey::Back, "a lone Escape was lost between waits"sv);
    });
    passed &= Test("native incomplete sequences expire without consuming following text"sv, [&] {
        for (const auto prefix : std::array{"\x1b"sv, "\x1b["sv, "\x1bO"sv}) {
            auto console = TestConsole{};
            input.Feed(prefix);
            Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).key == MenuKey::Back,
                std::format("an incomplete sequence {:?} did not resolve to Escape", prefix));
            input.Feed("日"sv);
            for (const auto character : prefix.substr(1)) {
                Require(std::cmp_equal(+console.ReadTextInput().character, +character),
                    "Escape consumed a character following its prefix"sv);
            }
            Require(console.ReadTextInput().character == U'日',
                "an expired sequence damaged subsequent Unicode text"sv);
        }
    });
    passed &= Test("native Escape followed immediately by ordinary text"sv, [&] {
        auto console = TestConsole{};
        input.Feed("\x1b日"sv);
        Require(console.ReadTextInput().key == MenuKey::Back,
            "Unicode text following Escape hid the Escape key"sv);
        Require(console.ReadTextInput().character == U'日',
            "Escape consumed the following Unicode character"sv);
    });
#ifndef _WIN32
    passed &= Test("modified-key reporting enabled and restored on the Unix alternate screen"sv, [&] {
        constexpr auto expected = vt::Concatenate<
            vt::kEnterAlternateScreen, vt::kEnableModifiedKeys, vt::kPushDisambiguatedKeys,
            vt::kSaveCursorVisibility, vt::kHideCursor,
            vt::kPopKeyboardMode, vt::kResetModifiedKeys, vt::kResetAttributes, vt::kShowCursor,
            vt::kRestoreCursorVisibility, vt::kLeaveAlternateScreen>();
        for (const auto unwind : std::array{false, true}) {
            auto console = NativeConsole{};
            try {
                const auto screen = console.EnterScreen();
                if (unwind) {
                    throw InjectedFailure{"leave the screen through exception unwinding"};
                }
            } catch (const InjectedFailure &) {
            }
            const auto output = input.ReadOutput(expected.size());
            Require(output == expected, std::format(
                "screen entry and restoration: expected {:?}, received {:?}", expected, output));
        }
    });
#endif
    passed &= Test("native update measurements counting begin, presentation, and guard output"sv, [&] {
        // BSU (`CSI ? 2026 h`) begins a synchronized update. ESU (`CSI ? 2026 l`)
        // ends it, once for presentation and once more when the guard is destroyed.
        // https://github.com/contour-terminal/vt-extensions/blob/master/synchronized-output.md
        constexpr auto update_sequences = vt::Concatenate<vt::kBeginSynchronizedUpdate,
            vt::kEndSynchronizedUpdate, vt::kEndSynchronizedUpdate>();
        auto console = MeasuringConsole<NativeConsole>{};
        {
            const auto update = console.BeginUpdate();
            console.PresentFrame();
        }
        input.Feed("\x0c"sv);
        Require(console.ReadMenuInput().key == MenuKey::Redraw,
            "recording an update changed the supplied input"sv);
        const auto &measurement = console.measurements.at(0);
        Require(measurement.writes == 3, "an update control write was not counted"sv);
        Require(measurement.bytes == update_sequences.size(),
            "update measurements omitted control-sequence bytes"sv);
    });
    passed &= Test("native fragmented cursor reply with keys before and after it"sv, [&] {
        auto console = TestConsole{};
        auto remainder = std::future<void>{};
        console.on_write = [&](const auto text) {
            Require(text == detail::ReportRequest(detail::TerminalReport::Cursor),
                "unexpected query"sv);
            // CPR (`CSI row;column R`) reports (4, 9), split after the semicolon.
            // Ctrl+L precedes the report and the `1` shortcut follows it.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
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
        // CPR reports row 7, column 8 after the cancelled query.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
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
                    // XTWINOPS replies `CSI 8;rows;columns t`: 24 rows, 80 columns.
                    // The other branch supplies Ctrl+C to cancel the size query.
                    // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
                    input.Feed(cancel_size ? "\x03"sv : "\x1b[8;24;80t"sv);
                } else if (text == detail::ReportRequest(detail::TerminalReport::Cursor)) {
                    input.Feed("\x03"sv);
                }
            };
            const auto screen = console.EnterScreen();
            Require(!SelectTestMenuItem(console, {}, std::array{"Installé"sv}),
                "cancellation returned a selected entry"sv);
            Require(!output.contains("unavailable"sv), "cancellation painted recovery instructions"sv);
        }
    });
    passed &= Test("native cancellation queued after a completed report"sv, [&] {
        auto console = TestConsole{};
        // CPR (`CSI 4;9 R`) reports the cursor, followed by Ctrl+C (ETX, 0x03).
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
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
            const auto screen = console.EnterScreen();
            console.on_write = [](const auto) { throw InjectedFailure{"injected output failure"}; };
            std::ignore = SelectTestMenuItem(console, {}, std::array{"Installation"sv});
            Require(false, "the output exception did not propagate"sv);
        } catch (const InjectedFailure &) {
        }
        Require(input.Modes() == original_modes, "exception unwinding changed terminal modes"sv);
    });
    passed &= Test("native text input decodes Unicode and modified Enter without consuming following text"sv, [&] {
        constexpr auto kKittyAltReturn = "\x1b[13;3u"sv;
        constexpr auto kKittyMetaReturn = "\x1b[13;33u"sv;
        constexpr auto kXtermAltReturn = "\x1b[27;3;13~"sv;
        constexpr auto kXtermMetaReturn = "\x1b[27;9;13~"sv;
        for (const auto sequence : std::array{
                "\x1b[13;2u"sv, "\x1b[13;66u"sv, "\x1b[57414;2u"sv,
                "\x1b[13;2:1u"sv, "\x1b[13;2:2u"sv,
                "\x1b[27;2;13~"sv, "\n"sv, vt::kMetaReturn,
                kKittyAltReturn, kKittyMetaReturn, kXtermAltReturn, kXtermMetaReturn}) {
            auto console = TestConsole{};
            input.Feed(std::format("{}日😀", sequence));
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            Require(console.ReadTextInput(deadline).key == MenuKey::Newline,
                std::format("modified Enter {:?} did not insert a newline", sequence));
            const auto japanese = console.ReadTextInput(deadline);
            const auto emoji = console.ReadTextInput(deadline);
            Require((japanese.key == MenuKey::Text) && (japanese.character == U'日') &&
                (emoji.key == MenuKey::Text) && (emoji.character == U'😀'),
                "decoding modified Enter damaged the following Unicode text"sv);
        }
    });
    passed &= Test("keyboard reports cannot insert invalid Unicode or unhandled function keys"sv, [&] {
        auto console = TestConsole{};
        input.Feed("\x1b[55296u\x1b[1114112u\x1b[57376uZ"sv);
        Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1s).character == U'Z',
            "a non-text key report reached the editor as text"sv);
    });
    passed &= Test("encoded Ctrl+C cancels a query while preserving neighboring text"sv, [&] {
        for (const auto sequence : std::array{"\x1b[99;5u"sv, "\x1b[27;5;99~"sv}) {
            auto console = TestConsole{};
            console.on_write = [&](const auto) { input.Feed(std::format("a{}z", sequence)); };
            try {
                std::ignore = console.QueryCursor();
                Require(false, "encoded Ctrl+C did not cancel the cursor query"sv);
            } catch (const InputCancelled &) {
            }
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            Require((console.ReadTextInput(deadline).character == U'a') &&
                (console.ReadTextInput(deadline).character == U'z'),
                "query cancellation consumed neighboring text"sv);
        }
    });
    passed &= Test("fragmented Shift+Enter and Meta Return survive an input deadline"sv, [&] {
        constexpr auto kShiftEnter = "\x1b[13;2u"sv;
        constexpr auto kShiftEnterRelease = "\x1b[13;2:3u"sv;
        for (const auto sequence : {kShiftEnter, vt::kMetaReturn}) {
            for (const auto split : std::views::iota(1uz, sequence.size())) {
                auto console = TestConsole{};
                input.Feed(sequence.substr(0, split));
                Require(console.ReadTextInput(std::chrono::steady_clock::now() + 1ms).key == MenuKey::Timeout,
                    "an incomplete modified Enter was treated as a key"sv);
                input.Feed(std::format("{}{}Z", sequence.substr(split), kShiftEnterRelease));
                const auto deadline = std::chrono::steady_clock::now() + 1s;
                Require(console.ReadTextInput(deadline).key == MenuKey::Newline,
                    "fragmenting modified Enter changed its action"sv);
                Require(console.ReadTextInput(deadline).character == U'Z',
                    "a key release inserted text or consumed the following key"sv);
            }
        }
    });
#ifdef _WIN32
    passed &= Test("query cancellation preserves a resize inside an encoded Ctrl+C"sv, [&] {
        auto console = TestConsole{};
        console.on_write = [&](const auto) {
            auto resize = INPUT_RECORD{.EventType = WINDOW_BUFFER_SIZE_EVENT};
            resize.Event.WindowBufferSizeEvent.dwSize = {90, 30};
            input.Feed("\x1b[99;"sv);
            input.FeedRecords(std::span{&resize, 1});
            input.Feed("5uz"sv);
        };
        try {
            std::ignore = console.QueryCursor();
            Require(false, "encoded Ctrl+C did not cancel the query"sv);
        } catch (const InputCancelled &) {
        }
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        Require((console.ReadTextInput(deadline).key == MenuKey::Resize) &&
            (console.ReadTextInput(deadline).character == U'z'),
            "removing a cancellation report also removed unrelated input"sv);
    });
    passed &= Test("Windows pasted emoji survive Alt key-up records and intervening modifiers"sv, [&] {
        auto console = TestConsole{};
        const auto screen = console.EnterScreen();
        constexpr auto text = "❤️❤️test❤️❤️😀👩‍💻"sv;
        // Console Host's `SynthesizeNumpadEvents` delivers each UTF-16 code unit
        // on an Alt key-up, preceded by characterless modifier and digit records.
        // The supplementary emoji exercise a surrogate pair with those records
        // between its two halves; the hearts also exercise variation selectors.
        // https://github.com/microsoft/terminal/blob/main/src/interactivity/base/EventSynthesis.cpp
        const auto wide = Transcode<wchar_t>(text);
        for (const auto character : std::wstring_view{wide}) {
            auto alt = INPUT_RECORD{.EventType = KEY_EVENT};
            alt.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                .wVirtualKeyCode = VK_MENU, .wVirtualScanCode = 0,
                .uChar = {.UnicodeChar = L'\0'}, .dwControlKeyState = LEFT_ALT_PRESSED};
            auto digit = alt;
            digit.Event.KeyEvent.wVirtualKeyCode = VK_NUMPAD1;
            auto result = alt;
            result.Event.KeyEvent.bKeyDown = FALSE;
            result.Event.KeyEvent.uChar.UnicodeChar = character;
            result.Event.KeyEvent.dwControlKeyState = 0;
            input.FeedRecords(std::array{alt, digit, result});
        }
        auto release = INPUT_RECORD{.EventType = KEY_EVENT};
        release.Event.KeyEvent = {.bKeyDown = FALSE, .wRepeatCount = 1,
            .wVirtualKeyCode = 'X', .wVirtualScanCode = 0,
            .uChar = {.UnicodeChar = L'X'}, .dwControlKeyState = 0};
        input.FeedRecords(std::span{&release, 1});
        input.Feed("Z"sv);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        const auto expected = Transcode<char32_t>(text);
        for (const auto character : std::u32string_view{expected}) {
            const auto decoded = console.ReadTextInput(deadline);
            Require((decoded.key == MenuKey::Text) && (decoded.character == character) &&
                (decoded.repeat == 1), "pasted Unicode was dropped or changed"sv);
        }
        Require(console.ReadTextInput(deadline).character == U'Z',
            "an ordinary key release inserted duplicate text"sv);
    });
#endif
    passed &= Test("native query timeout, a late reply, and subsequent successful query"sv, [&] {
        auto console = TestConsole{};
        const auto start = std::chrono::steady_clock::now();
        Require(!console.QueryCursor(), "a missing report produced a cursor position"sv);
        Require(std::chrono::steady_clock::now() - start < 8s, "the query exceeded its deadline"sv);
        // A late CPR reports row 1, column 1; Ctrl+L follows as ordinary input.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        input.Feed("\x1b[1;1R\x0c"sv);
        Require(console.ReadMenuInput().key == MenuKey::Redraw,
            "a late report was interpreted as a menu shortcut"sv);
        // CPR reports row 2, column 3 in response to the next query.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        console.on_write = [&](const auto) { input.Feed("\x1b[2;3R"sv); };
        Require(console.QueryCursor() == CursorPosition{2, 3}, "a query could not recover after timeout"sv);
    });
#ifdef _WIN32
    passed &= Test("successive menus and scrolling through native Console Host"sv, [&input] {
        auto console = MeasuringConsole<NativeConsole>{};
        const auto records = std::array{VK_DOWN, VK_END, VK_HOME, VK_RETURN} |
            std::views::transform([](const int key) {
                auto record = INPUT_RECORD{.EventType = KEY_EVENT};
                record.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                    .wVirtualKeyCode = FailFastCast<WORD>(key), .wVirtualScanCode = 0,
                    .uChar = {.UnicodeChar = L'\0'}, .dwControlKeyState = 0};
                return record;
            }) | std::ranges::to<std::vector>();
        input.FeedRecords(records);
        const auto entries = [] {
            auto result = std::vector<std::string>{};
            for (auto index = 1; index <= 100; ++index) {
                result.push_back(std::format("Backup {:03}", index));
            }
            result.front() = "Installation — café — é";
            result.back() = "日本語 — 👩‍💻 — ©️";
            return result;
        }();
        const auto labels = entries | std::views::transform([](const auto &entry) {
            return std::string_view{entry};
        }) | std::ranges::to<std::vector>();
        {
            const auto screen = console.EnterScreen();
            constexpr auto header = std::array{"Native menu measurements"sv};
            Require(SelectTestMenuItem(console, header, labels) == 0,
                "the native menu could not complete the scripted selection"sv);
            input.FeedRecords(std::span{records}.last(1));
            Require(SelectTestMenuItem(console, header,
                    std::array{"Backup 001"sv, "Backup 002"sv}) == 0,
                "the second native menu could not select an entry on the same screen"sv);
        }
        PrintMenuMeasurements(console.measurements);
        // Console Host can deliver a size event after entering the alternate
        // screen. Separate those updates from the navigation supplied here so
        // the assertions identify the intended menu transitions.
        const auto navigation = console.measurements | std::views::filter([](const auto &update) {
            return update.input != MenuKey::Resize;
        }) | std::ranges::to<std::vector>();
        Require(navigation.size() == (records.size() + 1),
            "native menu updates were not all measured"sv);
        Require((navigation.at(0).cursor_queries > 0) &&
            (navigation.at(2).cursor_queries > 0),
            "the native menu did not measure unfamiliar text"sv);
        Require((navigation.at(1).cursor_queries == 0) &&
            (navigation.at(3).cursor_queries == 0) &&
            (navigation.at(4).cursor_queries == 0),
            "the native menus repeated measurements for cached text"sv);
    });
    passed &= Test("arriving Unicode output and a command selection through native Console Host"sv, [&] {
        auto console = NativeConsole{};
        const auto screen = console.EnterScreen();
        auto view = OutputMenu{};
        view.SetCommands(std::array{OutputCommand{7, "Open folder"sv}});
        auto updates = 0;
        const auto selected = view.Select(console, [](auto &frame) {
            frame.Write("Output menu native test\n"sv);
        }, [&](auto &output) {
            ++updates;
            if (updates == 1) {
                output.AppendLine("日本語 — café — é — 👩‍💻");
            } else if (updates == 2) {
                output.AppendLine("Mount ready.");
            } else if (updates == 3) {
                auto enter = INPUT_RECORD{.EventType = KEY_EVENT};
                enter.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                    .wVirtualKeyCode = VK_RETURN, .wVirtualScanCode = 0,
                    .uChar = {.UnicodeChar = L'\r'}, .dwControlKeyState = 0};
                input.FeedRecords(std::span{&enter, 1});
            }
        });
        Require(selected == 7, "the output view could not select its native command"sv);
    });
    passed &= Test("multiline text editing through native Console Host"sv, [&] {
        auto console = NativeConsole{};
        const auto screen = console.EnterScreen();
        input.Feed("é日👩‍💻\x1b[D\x1b[3~\x1b[13;2uZ\r"sv);
        const auto result = EditText(console, [](auto &frame) {
            frame.Write("Enter accepts; Shift+Enter inserts a newline.\n\n");
        });
        Require(result == "é日\nZ",
            "the native editor did not preserve Unicode around deletion and a newline"sv);
    });
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
        const auto screen = console.EnterScreen();
        // This case supplies native key records, so detection must finish and
        // disable VT input before Console Host receives those records.
        Require(console.ReadTextInput(std::chrono::steady_clock::now() + 6s).key == MenuKey::Timeout,
            "native report-removal setup received unexpected input"sv);
        auto key = INPUT_RECORD{.EventType = KEY_EVENT};
        key.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 3,
            .wVirtualKeyCode = VK_DOWN, .wVirtualScanCode = 80,
            .uChar = {.UnicodeChar = L'界'}, .dwControlKeyState = SHIFT_PRESSED};
        auto resize = INPUT_RECORD{.EventType = WINDOW_BUFFER_SIZE_EVENT};
        resize.Event.WindowBufferSizeEvent.dwSize = {90, 30};
        console.on_write = [&](const auto) {
            // CPR (`CSI 4;9 R`) is split around unrelated native input records.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
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
        // CPR (`CSI 1;1 R`) is split around a key event and followed by Ctrl+L.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        input.Feed("\x1b[1;"sv);
        input.FeedRecords(std::array{key});
        input.Feed("1R\x0c"sv);
        const auto navigation = console.ReadMenuInput();
        Require((navigation.key == MenuKey::Down) && (navigation.repeat == 3),
            "an unfinished late reply consumed physical navigation"sv);
        Require(console.ReadMenuInput().key == MenuKey::Redraw,
            "the remainder of a late reply became a shortcut after navigation"sv);
    });
#endif
    passed &= Test("native cancellation inside complete and fragmented escape sequences"sv, [&] {
        for (const auto fragmented : {false, true}) {
            auto console = TestConsole{};
            auto remainder = [&input, fragmented] {
                if (!fragmented) {
                    // CPR (`CSI 12;34 R`) contains an inserted Ctrl+C, then
                    // Ctrl+L follows the report as the next command to the menu.
                    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
                    input.Feed("\x1b[12;\x03" "34R\x0c"sv);
                    return std::future<void>{};
                }
                // The same CPR (`CSI 12;34 R`) arrives in separate fragments,
                // with Ctrl+C between the row and column and Ctrl+L afterward.
                // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
                input.Feed("\x1b[12;"sv);
                return std::async(std::launch::async, [&input] {
                    std::this_thread::sleep_for(10ms);
                    input.Feed("\x03"sv);
                    std::this_thread::sleep_for(10ms);
                    input.Feed("34R\x0c"sv);
                });
            }();
            Require(console.ReadMenuInput().key == MenuKey::Cancel,
                "a terminal reply swallowed Ctrl+C"sv);
            if (remainder.valid()) {
                remainder.get();
            }
            Require(console.ReadMenuInput().key == MenuKey::Redraw,
                "cancellation lost a later key or exposed part of the reply as input"sv);
        }
    });
#ifndef _WIN32
    passed &= Test("Unix signals interrupting a query without extending its deadline"sv, [] {
        received_signal = 0;
        const auto handler = SetSignalHandlerScoped(SIGUSR1, [](int) { received_signal = 1; });
        auto console = TestConsole{};
        const auto start = std::chrono::steady_clock::now();
        // Repeated signals exercise interrupted waits throughout the query.
        // Stop the sender before restoring the handler or leaving this thread.
        auto signals = std::jthread{[thread = pthread_self()](const std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(20ms);
                std::ignore = pthread_kill(thread, SIGUSR1);
            }
        }};
        Require(!console.QueryCursor(), "an interrupted missing reply produced a cursor position"sv);
        Require(received_signal != 0, "the query received none of the test signals"sv);
        Require(std::chrono::steady_clock::now() - start < 8s,
            "signal interruptions extended the query beyond its completion limit"sv);
    });
#endif
    passed &= Test("kitty keyboard protocol Escape, shortcuts, modifiers, and keypad navigation"sv, [&] {
        // Kitty's CSI u codes identify keys, with an optional modifier value
        // equal to one plus the modifier bits. CSI letter/tilde navigation
        // remains available alongside encoded keys and ordinary Enter/text.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#quickstart
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
        GSL_SUPPRESS("26445",
            "This structured binding copies the pair, including its string view. "
            "C26445 incorrectly diagnoses a reference to the view even though "
            "the declaration uses `const auto`, without `&`.")
        for (const auto [sequence, expected] : std::array{
                std::pair{"\x1b[27u"sv, MenuKey::Back},
                std::pair{"\x1b[27;1u"sv, MenuKey::Back},
                std::pair{"\x1b[27;u"sv, MenuKey::Back},
                std::pair{"\x1b[99;5u"sv, MenuKey::Cancel},
                std::pair{"\x1b[99;6u"sv, MenuKey::Cancel},
                std::pair{"\x1b[99;197u"sv, MenuKey::Cancel},
                std::pair{"\x1b[108;5u"sv, MenuKey::Redraw},
                std::pair{"\x1b[108;69u"sv, MenuKey::Redraw},
                std::pair{"\x1b[13u"sv, MenuKey::Accept},
                std::pair{"\x1b[109;5u"sv, MenuKey::Accept},
                std::pair{"\x1b[57414u"sv, MenuKey::Accept},
                std::pair{"\x1b[57419u"sv, MenuKey::Up},
                std::pair{"\x1b[57420u"sv, MenuKey::Down},
                std::pair{"\x1b[57421u"sv, MenuKey::PageUp},
                std::pair{"\x1b[57422u"sv, MenuKey::PageDown},
                std::pair{"\x1b[57423u"sv, MenuKey::Home},
                std::pair{"\x1b[57424u"sv, MenuKey::End},
                std::pair{"\x1b[1;129A"sv, MenuKey::Up},
                std::pair{"\x1b[5;129~"sv, MenuKey::PageUp},
                std::pair{"\x1b[6;129~"sv, MenuKey::PageDown},
                std::pair{"\r"sv, MenuKey::Accept},
                std::pair{"1"sv, MenuKey::Details}}) {
            auto console = TestConsole{};
            input.Feed(std::format("{}1", sequence));
            const auto start = std::chrono::steady_clock::now();
            Require(console.ReadMenuInput().key == expected,
                std::format("incorrect action for encoded key {:?}", sequence));
            if (sequence == "\x1b[27u"sv) {
                std::println("Encoded Escape: {:.3f} ms", std::chrono::duration<double, std::milli>{
                    std::chrono::steady_clock::now() - start}.count());
            }
            Require(console.ReadMenuInput().key == MenuKey::Details,
                "decoding a key consumed the following shortcut"sv);
        }
    });
    passed &= Test("unrelated and malformed kitty keyboard reports leaving the next command intact"sv, [&] {
        // These CSI u sequences include unrelated modifiers, unknown keys,
        // invalid numeric fields, and optional enhancements we did not enable.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#an-overview
        for (const auto sequence : std::array{
                "\x1b[99u"sv, "\x1b[99;3u"sv, "\x1b[99;7u"sv,
                "\x1b[999999999999999999999u"sv, "\x1b[27;0u"sv,
                "\x1b[27;999999999999999999999u"sv, "\x1b[;5u"sv,
                "\x1b[99;5:3u"sv, "\x1b[?1u"sv}) {
            auto console = TestConsole{};
            input.Feed(std::format("{}1", sequence));
            Require(console.ReadMenuInput().key == MenuKey::Details,
                std::format("unrelated encoded input {:?} invoked a menu action", sequence));
        }
    });
    passed &= Test("kitty keyboard protocol Escape surviving every input split"sv, [&] {
        // Kitty reports Escape as CSI 27 u, with a final byte distinguishing it
        // from both a lone legacy Escape and an incomplete sequence.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
        constexpr auto escape = "\x1b[27u"sv;
        for (const auto split : std::views::iota(1uz, escape.size())) {
            auto console = TestConsole{};
            input.Feed(escape.substr(0, split));
            auto remainder = std::async(std::launch::async, [&input, split, escape] {
                std::this_thread::sleep_for(5ms);
                input.Feed(std::format("{}1", escape.substr(split)));
            });
            Require(console.ReadMenuInput().key == MenuKey::Back,
                "a fragmented encoded Escape was not recognized"sv);
            remainder.get();
            Require(console.ReadMenuInput().key == MenuKey::Details,
                "a fragmented Escape consumed subsequent input"sv);
        }
    });
    passed &= Test("fragmented kitty keyboard protocol Ctrl+C interrupting cursor and size queries"sv, [&] {
        // CSI 99;5 u is Ctrl+C; CSI 108;5 u is Ctrl+L. Leave Ctrl+L before
        // cancellation and the `1` shortcut after it to verify queue retention.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        constexpr auto cancel = "\x1b[99;5u"sv;
        for (const auto size_query : std::array{false, true}) {
            for (const auto split : std::views::iota(1uz, cancel.size())) {
                auto console = TestConsole{};
                auto remainder = std::future<void>{};
                console.on_write = [&](const auto) {
                    input.Feed(std::format("\x1b[108;5u{}", cancel.substr(0, split)));
                    remainder = std::async(std::launch::async, [&input, split, cancel] {
                        std::this_thread::sleep_for(5ms);
                        input.Feed(std::format("{}1", cancel.substr(split)));
                    });
                };
                try {
                    if (size_query) {
                        std::ignore = console.QuerySize();
                    } else {
                        std::ignore = console.QueryCursor();
                    }
                    Require(false, "encoded Ctrl+C did not cancel the query"sv);
                } catch (const InputCancelled &) {
                }
                remainder.get();
                Require(console.ReadMenuInput().key == MenuKey::Redraw,
                    "cancellation lost the preceding Ctrl+L"sv);
                Require(console.ReadMenuInput().key == MenuKey::Details,
                    "cancellation lost the following shortcut"sv);
            }
        }
    });
    passed &= Test("native query replies retaining their order relative to kitty keyboard protocol Ctrl+C"sv, [&] {
        // A CPR reports (4, 9); CSI 99;5 u cancels. Test both arrival orders.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        for (const auto reply_first : std::array{false, true}) {
            auto console = TestConsole{};
            console.on_write = [&](const auto) {
                input.Feed(reply_first ? "\x1b[4;9R\x1b[99;5u1"sv :
                    "\x1b[99;5u\x1b[4;9R1"sv);
            };
            if (reply_first) {
                Require(console.QueryCursor() == CursorPosition{4, 9},
                    "a later Ctrl+C displaced the preceding reply"sv);
                console.on_write = {};
            }
            try {
                std::ignore = console.QueryCursor();
                Require(false, "encoded Ctrl+C was lost while reading the reply"sv);
            } catch (const InputCancelled &) {
            }
            Require(console.ReadMenuInput().key == MenuKey::Details,
                "query cancellation damaged the queued shortcut"sv);
        }
    });
    passed &= Test("native fragmented navigation and a lone Escape"sv, [&] {
        auto console = TestConsole{};
        // The Down key is `CSI B` in normal cursor-key mode, split after `ESC [`.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
        input.Feed("\x1b["sv);
        auto remainder = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(10ms);
            input.Feed("B"sv);
        });
        Require(console.ReadMenuInput().key == MenuKey::Down, "a fragmented arrow became Escape"sv);
        remainder.get();
        // A standalone ESC (0x1B) represents the Escape key rather than a sequence.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        input.Feed("\x1b"sv);
        const auto escape_started = std::chrono::steady_clock::now();
        Require(console.ReadMenuInput().key == MenuKey::Back, "a lone Escape did not produce Back"sv);
        std::println("Lone Escape: {:.3f} ms", std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - escape_started}.count());
        input.Feed("\r"sv);
        const auto enter_started = std::chrono::steady_clock::now();
        Require(console.ReadMenuInput().key == MenuKey::Accept, "Enter did not produce Accept"sv);
        std::println("Enter: {:.3f} ms", std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - enter_started}.count());
    });
    passed &= Test("native terminal disconnection reporting I/O errors and preserving the primary failure"sv, [&] {
#ifndef _WIN32
        const auto hangup = SetSignalHandlerScoped(SIGHUP, SIG_IGN);
#endif
        const auto start = std::chrono::steady_clock::now();
        try {
            auto console = TestConsole{};
            const auto screen = console.EnterScreen();
            input.Disconnect();
            const auto output_failed = [&console] {
                try {
                    console.NativeConsole::Write("test output"sv);
                } catch (const std::system_error &) {
                    return true;
                }
                return false;
            }();
            Require(output_failed, "disconnected terminal output succeeded"sv);
#ifdef _WIN32
            std::ignore = console.ReadInput();
#else
            std::ignore = console.QueryCursor();
#endif
        } catch (const std::runtime_error &error) {
            const auto message = std::string_view{error.what()};
            Require(message.contains("terminal"sv) && message.contains("input"sv),
                "screen or mode restoration replaced the terminal input failure"sv);
            Require(std::chrono::steady_clock::now() - start < 2s,
                "terminal disconnection did not finish promptly"sv);
            return;
        }
        Require(false, "disconnected terminal input returned normally"sv);
    });
    return passed;
}

}

export auto RunNativeTests() -> int {
#ifdef _WIN32
    // The CTest launcher gives this process a hidden, private Windows console.
    auto input = NativeInput{};
    return TestNativeInput(input) ? 0 : 1;
#else
    // A child session owns this pseudoterminal as /dev/tty. Standard output and
    // error remain connected to CTest, so test results contain no drawing codes.
    auto input = NativeInput{};
    const auto child = fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "could not create the native test process");
    }
    if (child == 0) {
        const auto passed = Test("isolated Unix native input tests"sv, [&] {
            input.Attach();
            Require(TestNativeInput(input),
                "one or more native input tests failed"sv);
        });
        return passed ? 0 : 1;
    }
    // Only the child may retain a master descriptor: the disconnection test
    // closes that last reference to simulate the terminal application exiting.
    input.Disconnect();
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
