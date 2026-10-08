// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "../compat/gsl_suppress.h"

#ifdef _WIN32
    #include <devicefs/strsafe_compat.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstdio>
    #include <cstdlib>
    #include <string.h>
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
import devicefs.terminal.base_console;
import devicefs.terminal.menu;
import devicefs.terminal.text_input;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.scope_exit;
import devicefs.terminal.test_support;
import devicefs.terminal.menu_measurements;
import devicefs.terminal.vt;
import devicefs.terminal.test_clock;

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

namespace devicefs::terminal::tests {

template <Clock Clock = std::chrono::steady_clock>
#ifdef _WIN32
using BasicNativeConsole = BasicWindowsConsole<Clock>;
#else
using BasicNativeConsole = BasicUnixConsole<Clock>;
#endif

}

namespace {

// These tests control the input of an isolated native console. TestConsole
// intercepts outgoing queries and supplies chosen replies through the operating
// system, exercising the real native reader and its queue. Intercepting output
// also prevents the console host from answering queries on the tests' behalf.
#ifdef _WIN32
class NativeInput {
public:
    auto DiscardQueuedInput() const -> void {
        if (!FlushConsoleInputBuffer(input_.get())) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not discard input left by an earlier native test");
        }
    }

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
class NativeInput {
public:
    NativeInput() = default;
    NativeInput(const NativeInput &) = delete;
    auto operator=(const NativeInput &) -> NativeInput & = delete;

    auto DiscardQueuedInput() const -> void {
        if (tcflush(descriptors_[1], TCIFLUSH) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not discard input left by an earlier native test");
        }
    }

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

    [[nodiscard]] auto DevicePath() const -> std::span<const char> {
        // The replica pathname is needed to open test consoles. On macOS,
        // `ttyname` searches `/dev`, where other concurrent fixtures create
        // and delete PTY entries. `ptsname` queries our primary directly instead.
        // https://github.com/apple-oss-distributions/Libc/blob/71bbe350ab79eef58113991d817ccc6165061a64/gen/devname.c
        const auto path = ptsname(descriptors_[0]);
        if (path == nullptr) {
            throw std::system_error(errno, std::generic_category(),
                "could not identify the test pseudoterminal's device path");
        }
        return {path, std::char_traits<char>::length(path) + 1};
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
                "could not close the test pseudoterminal's primary descriptor");
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

    // Read the expected output from the pseudoterminal's primary, including
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
#endif

template <class TimePoint>
[[nodiscard]] auto ReadKeyboardInput(const auto &read,
    const TimePoint deadline) -> MenuInput {
    // Console Host can supply size notifications independently of the keys
    // injected by a test. Keyboard assertions ignore those notifications;
    // tests of resize delivery use the native reader directly. Keeping the
    // original deadline prevents notifications from extending the input wait.
    for (;;) {
        const auto input = read(deadline);
        if (input.key != MenuKey::Resize) {
            return input;
        }
    }
}

using NativeConsole = BasicNativeConsole<TestClock>;

#ifdef _WIN32
// Drawing uses actual Console Host replies. Only the input wait simulates idle
// time, allowing the update callback to run again without sleeping or depending
// on how long Console Host takes to answer a layout query.
class OutputConsole : public NativeConsole {
public:
    GSL_SUPPRESS("26434",
        "OutputMenu dispatches ReadMenuInput through this test adapter. "
        "Only its input wait simulates elapsed time; layout queries still "
        "wait for actual Console Host replies.")
    [[nodiscard]] auto ReadMenuInput(
        const TestClock::time_point deadline =
            TestClock::time_point::max()) -> MenuInput {
        const auto previous = std::exchange(
            TestClock::wait_policy, WaitPolicy::AdvanceClock);
        const auto restore = ScopeExit{[previous] {
            TestClock::wait_policy = previous;
        }};
        const auto input = NativeConsole::ReadMenuInput(deadline);
        if (input.key == MenuKey::Timeout) {
            ++timeouts;
        }
        return input;
    }

    std::size_t timeouts = 0;
};
#endif

template <Clock Clock = TestClock>
class TestConsole : public BasicNativeConsole<Clock> {
public:
    using BasicNativeConsole<Clock>::BasicNativeConsole;

    GSL_SUPPRESS("26434",
        "BaseConsole dispatches ReadTextInput through its explicit object "
        "parameter. This test adapter excludes unrelated resize events "
        "from keyboard assertions.")
    [[nodiscard]] auto ReadTextInput(
        const typename Clock::time_point deadline =
            Clock::time_point::max()) -> MenuInput {
        return ReadKeyboardInput([this](const auto until) {
            return BasicNativeConsole<Clock>::ReadTextInput(until);
        }, deadline);
    }

    GSL_SUPPRESS("26434",
        "BaseConsole dispatches Write through its explicit object parameter. "
        "This test adapter replaces output so native queries receive only "
        "the replies supplied by the test.")
    auto Write(const std::string_view text) -> void {
#ifdef _WIN32
        if (text == vt::kRequestKeyboardSupport) {
            if (on_keyboard_query) {
                on_keyboard_query();
            }
            // An unanswered test query leaves detection pending. Forwarding
            // it to Console Host would introduce replies outside the fixture.
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

// Feed one chosen fragment per receive operation. Unlike a sleeping writer,
// this adapter establishes the parser's read boundaries regardless of thread
// scheduling. The native cases below separately exercise OS input and queues.
class SequenceConsole : public BasicBaseConsole<TestClock> {
public:
    explicit SequenceConsole(const std::string_view prefix) : pending{prefix} {}

    [[nodiscard]] auto Decode(const TestClock::time_point
        deadline = TestClock::time_point::max()) {
        return ReadEscape(deadline);
    }

    [[nodiscard]] auto ConsumeCancellation() const noexcept -> bool {
        return false;
    }

    [[nodiscard]] auto SequenceCharacters() const noexcept -> std::string_view {
        return pending;
    }

    auto DiscardSequenceCharacters(const std::size_t count) -> void {
        pending.erase(0, count);
    }

    [[nodiscard]] auto ReceiveUntil(
        const TestClock::time_point deadline) -> bool {
        if (on_receive) {
            on_receive(deadline);
        }
        if (fragments.empty()) {
            return false;
        }
        pending.append(fragments.front());
        fragments = fragments.subspan(1);
        return true;
    }

    std::string pending;
    std::span<const std::string_view> fragments;
    std::function<void(TestClock::time_point)> on_receive;
};

[[nodiscard]] auto InputKeyName(const MenuKey key) noexcept
    -> std::string_view {
    switch (key) {
    case MenuKey::Up: return "Up"sv;
    case MenuKey::Down: return "Down"sv;
    case MenuKey::PageUp: return "PageUp"sv;
    case MenuKey::PageDown: return "PageDown"sv;
    case MenuKey::Home: return "Home"sv;
    case MenuKey::End: return "End"sv;
    case MenuKey::Accept: return "Accept"sv;
    case MenuKey::Back: return "Back"sv;
    case MenuKey::Cancel: return "Cancel"sv;
    case MenuKey::Details: return "Details"sv;
    case MenuKey::Resize: return "Resize"sv;
    case MenuKey::Redraw: return "Redraw"sv;
    case MenuKey::SwitchArea: return "SwitchArea"sv;
    case MenuKey::Timeout: return "Timeout"sv;
    case MenuKey::Left: return "Left"sv;
    case MenuKey::Right: return "Right"sv;
    case MenuKey::Backspace: return "Backspace"sv;
    case MenuKey::Delete: return "Delete"sv;
    case MenuKey::Newline: return "Newline"sv;
    case MenuKey::Text: return "Text"sv;
    default: return "Unknown"sv;
    }
}

auto RequireInput(const MenuInput received, const MenuInput expected,
    const std::string_view context) -> void {
    if ((received.key != expected.key) ||
        (received.character != expected.character) ||
        (received.repeat != expected.repeat)) {
        throw std::runtime_error(std::format(
            "{}: expected {} (MenuKey {}), character U+{:04X}, repeat {}; "
            "received {} (MenuKey {}), character U+{:04X}, repeat {}",
            context, InputKeyName(expected.key),
            std::to_underlying(expected.key), +expected.character,
            expected.repeat, InputKeyName(received.key),
            std::to_underlying(received.key),
            +received.character, received.repeat));
    }
}

[[nodiscard]] auto TestNativeInput(NativeInput &input, const auto &...console_arguments) -> bool {
    const auto make_console = [&console_arguments...]<class T = TestConsole<>> {
        return T{console_arguments...};
    };
    const auto run_test = [&input](const std::string_view name,
        const auto &operation) {
        return Test(name, [&input, &operation] {
            // A failed assertion can leave unread keys in the OS queue after
            // its console has been destroyed. Each independent case starts
            // without those keys, rather than inheriting the previous failure.
            input.DiscardQueuedInput();
            TestClock::current = {};
            TestClock::wait_policy = WaitPolicy::AdvanceClock;
            const auto restore = ScopeExit{[] {
                TestClock::wait_policy = WaitPolicy::Native;
            }};
            std::invoke(operation);
        });
    };
    const auto queue_prefix = [&input](auto &console,
        const std::string_view prefix) {
        // A successful cursor query has read the preceding prefix into the
        // native queue while removing only its own report. The next input read
        // therefore starts with a known partial sequence, without a timed writer.
        const auto start = TestClock::now();
        console.on_write = [&input, prefix](const auto) {
#ifdef _WIN32
            // Two preceding resize events exercise the clock independently of
            // the number of input-loop iterations before the partial sequence.
            auto resize = INPUT_RECORD{.EventType = WINDOW_BUFFER_SIZE_EVENT};
            resize.Event.WindowBufferSizeEvent.dwSize = {90, 30};
            input.FeedRecords(std::array{resize, resize});
#endif
            input.Feed(std::format("{}\x1b[4;9R", prefix));
        };
        Require(console.QueryCursor() == CursorPosition{4, 9},
            "the partial-input setup did not recognize its cursor reply"sv);
        Require(TestClock::now() == start,
            "queued input advanced the simulated clock"sv);
        console.on_write = {};
    };
    const auto original_modes = input.Modes();
    auto passed = true;
    passed &= run_test("keyboard assertions skipping only resize notifications"sv,
        [] {
            constexpr auto expected = std::array{
                MenuInput{.key = MenuKey::Text, .repeat = 3, .character = U'界'},
                MenuInput{.key = MenuKey::Cancel},
                MenuInput{.key = MenuKey::Back},
                MenuInput{.key = MenuKey::Timeout}};
            const auto events = std::array{
                MenuInput{.key = MenuKey::Resize}, expected[0],
                MenuInput{.key = MenuKey::Resize}, expected[1],
                expected[2], MenuInput{.key = MenuKey::Resize}, expected[3]};
            const auto deadline = TestClock::now() + 1s;
            auto position = 0uz;
            for (const auto event : expected) {
                RequireInput(ReadKeyboardInput([&](const auto until) {
                    Require(until == deadline,
                        "a resize changed the keyboard input deadline"sv);
                    return events.at(position++);
                }, deadline), event, "filtering native size notifications"sv);
            }
        });
    passed &= run_test("input mismatch diagnostics identifying the returned event"sv,
        [] {
            try {
                RequireInput({.key = MenuKey::Back},
                    {.key = MenuKey::Text, .character = U'a'},
                    "ordinary text during keyboard detection"sv);
            } catch (const std::runtime_error &error) {
                const auto message = std::string_view{error.what()};
                Require(message.contains(
                        "ordinary text during keyboard detection"sv) &&
                    message.contains("expected Text"sv) &&
                    message.contains("character U+0061"sv) &&
                    message.contains("received Back"sv) &&
                    message.contains("character U+0000"sv) &&
                    message.contains("repeat 1"sv),
                    "the input mismatch omitted expected or received values"sv);
                return;
            }
            Require(false, "an unexpected key passed the input assertion"sv);
        });
    passed &= run_test("input isolation discarding an earlier case's queued keys"sv,
        [&input, &make_console] {
            auto console = make_console();
            input.Feed("leftover"sv);
            input.DiscardQueuedInput();
            input.Feed("Z"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'Z'},
                "input after discarding the previous case's keys"sv);
        });
    passed &= run_test("a delayed wait preserving the caller's deadline and partial key"sv,
        [] {
            auto console = SequenceConsole{"\x1b["sv};
            const auto caller_deadline = TestClock::now() + 10ms;
            auto received = false;
            console.on_receive = [caller_deadline, &received](
                const auto until) {
                received = true;
                Require(until == caller_deadline,
                    "the parser waited beyond the earlier caller deadline"sv);
                // Resumption after both deadlines must not turn a refresh
                // timeout into Escape or remove part of the queued sequence.
                TestClock::current += 100ms;
            };
            Require(!console.Decode(caller_deadline),
                "resuming after both deadlines produced a key"sv);
            Require(console.pending == "\x1b["sv,
                "resuming after both deadlines consumed the partial key"sv);
            Require(received, "the delayed wait was not exercised"sv);
        });
    passed &= run_test("shared keyboard decoding preserving every input split"sv,
        [] {
            GSL_SUPPRESS("26445",
                "This structured binding copies the pair, including its string "
                "view. C26445 incorrectly diagnoses a reference to the view "
                "even though the declaration uses `const auto`, without `&`.")
            for (const auto [sequence, key] : std::array{
                    std::pair{"\x1b[B"sv, MenuKey::Down},
                    std::pair{"\x1bOA"sv, MenuKey::Up},
                    std::pair{"\x1b[Z"sv, MenuKey::SwitchArea},
                    std::pair{"\x1b[27u"sv, MenuKey::Back},
                    std::pair{"\x1b[13;2u"sv, MenuKey::Newline},
                    std::pair{vt::kMetaReturn, MenuKey::Newline},
                    std::pair{"\x1b[99;5u"sv, MenuKey::Cancel},
                    std::pair{"\x1b[27;5;99~"sv, MenuKey::Cancel}}) {
                for (const auto split :
                    std::views::iota(1uz, sequence.size())) {
                    const auto prefix = sequence.substr(0, split);
                    auto console = SequenceConsole{prefix};
                    Require(!console.Decode(),
                        "an incomplete sequence produced a key"sv);
                    Require(console.pending == prefix,
                        "an incomplete sequence was consumed"sv);
                    const auto tail = std::format(
                        "{}Z", sequence.substr(split));
                    const auto fragments = std::array{std::string_view{tail}};
                    console.fragments = fragments;
                    const auto decoded = console.Decode();
                    Require(decoded && (decoded->key == key),
                        std::format("split {} changed key {:?}",
                            split, sequence));
                    Require(console.pending == "Z"sv,
                        std::format("split {} left an undecoded suffix "
                            "or consumed the next character for {:?}",
                            split, sequence));
                }
            }
        });
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
        passed &= run_test(test.name, [&] {
            auto console = make_console();
            console.on_keyboard_query = [&input, kDeviceAttributes] { input.Feed(kDeviceAttributes); };
            // No keyboard reply is supplied until after screen entry and an
            // ordinary keystroke. Both operations must finish during detection.
            const auto screen = console.EnterScreen();
            auto resize = INPUT_RECORD{.EventType = WINDOW_BUFFER_SIZE_EVENT};
            resize.Event.WindowBufferSizeEvent.dwSize = {90, 30};
            input.FeedRecords(std::span{&resize, 1});
            input.Feed("a"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'a'},
                "ordinary text during keyboard detection"sv);
            Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
                "Console Host's reply prematurely disabled VT input"sv);
            input.Feed(std::format("{}日{}z", test.replies, test.shift_enter));
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'日'},
                "Unicode text after the keyboard reply"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Newline},
                "Shift+Enter after detecting the keyboard extension"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'z'},
                "text following Shift+Enter"sv);
            Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
                "a supported keyboard extension lost VT input"sv);
        });
    }
    passed &= run_test("Windows keyboard detection does not delay cancellation"sv, [&] {
        {
            auto console = make_console();
            console.on_keyboard_query = [] {};
            const auto screen = console.EnterScreen();
            input.Feed("\x03"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Cancel},
                "Ctrl+C during keyboard detection"sv);
        }
        Require(input.Modes() == original_modes,
            "cancelling keyboard detection left the console modes changed"sv);
    });
    passed &= run_test("Windows falls back to native input after the keyboard-reply deadline"sv, [&] {
        auto console = make_console();
        console.on_keyboard_query = [] {};
        const auto screen = console.EnterScreen();
        Require((input.Modes()[0] & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0,
            "VT input was disabled before the keyboard-reply deadline"sv);
        TestClock::current += 5s;
        input.Feed("\t"sv);
        RequireInput(console.ReadTextInput(),
            {.key = MenuKey::SwitchArea}, "input after the keyboard-detection deadline"sv);
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
            RequireInput(console.ReadTextInput(),
                {.key = test.second}, "native Return modifier"sv);
        }
    });
#endif
    passed &= run_test("native input deadlines and Tab focus changes"sv, [&] {
        auto console = make_console();
        const auto deadline = TestClock::now() + 5ms;
        Require(console.ReadMenuInput(deadline).key == MenuKey::Timeout,
            "idle input did not reach its deadline"sv);
        Require(TestClock::now() == deadline,
            "the idle wait did not advance to its requested deadline"sv);
        input.Feed("\t"sv);
        Require(console.ReadMenuInput().key == MenuKey::SwitchArea, "Tab did not change area"sv);
        Require(TestClock::now() == deadline,
            "queued Tab input advanced the simulated clock"sv);
        input.Feed("\x03"sv);
        Require(console.ReadMenuInput(TestClock::now()).key ==
                MenuKey::Timeout,
            "queued cancellation bypassed the caller's expired deadline"sv);
        Require(console.ReadMenuInput().key == MenuKey::Cancel,
            "an expired input wait lost cancellation"sv);
    });
    passed &= run_test("native fragmented keys surviving an earlier refresh deadline"sv, [&] {
        auto console = make_console();
        // CSI B is Down, CSI Z is Shift+Tab, and kitty's CSI 27 u is Escape.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
        queue_prefix(console, "\x1b["sv);
        const auto deadline = TestClock::now() + 1ms;
        Require(console.ReadMenuInput(deadline).key == MenuKey::Timeout,
            "a refresh deadline turned a partial arrow into Escape"sv);
        input.Feed("B\x1b[Z\x1b[27u"sv);
        TestClock::current += 100ms;
        Require(console.ReadMenuInput().key == MenuKey::Down, "a refresh deadline lost the arrow"sv);
        Require(console.ReadMenuInput().key == MenuKey::SwitchArea, "Shift+Tab did not change area"sv);
        Require(console.ReadMenuInput().key == MenuKey::Back, "kitty Escape was not decoded"sv);
        queue_prefix(console, "\x1b"sv);
        const auto escape_deadline = TestClock::now() + 1ms;
        Require(console.ReadMenuInput(escape_deadline).key == MenuKey::Timeout,
            "a refresh deadline prematurely consumed a lone Escape"sv);
        TestClock::current += 100ms;
        Require(console.ReadMenuInput().key == MenuKey::Back, "a lone Escape was lost between waits"sv);
    });
    passed &= run_test("native queued continuation surviving an expired Escape deadline"sv, [&] {
        auto console = make_console();
        // Reading a CPR leaves the preceding partial arrow in the native
        // reader's queue. Expiring its Escape deadline before supplying the
        // rest models a scheduling delay without relying on thread timing.
        queue_prefix(console, "\x1b["sv);
        const auto deadline = TestClock::now() + 1ms;
        Require(console.ReadTextInput(deadline).key == MenuKey::Timeout,
            "the partial arrow did not reach the caller's deadline"sv);
        TestClock::current += 100ms;
        input.Feed("B日"sv);
        Require(console.ReadTextInput(TestClock::now()).key == MenuKey::Timeout,
            "queued continuation bypassed the caller's expired deadline"sv);
        Require(console.ReadMenuInput().key == MenuKey::Down,
            "an expired Escape deadline split an already available arrow"sv);
        Require(console.ReadTextInput().character == U'日',
            "completing the arrow consumed the following Unicode character"sv);
    });
    passed &= run_test("native incomplete sequences expire without consuming following text"sv, [&] {
        for (const auto prefix : std::array{"\x1b"sv, "\x1b["sv, "\x1bO"sv}) {
            auto console = make_console();
            queue_prefix(console, prefix);
            Require(console.ReadTextInput().key == MenuKey::Back,
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
    passed &= run_test("native Escape followed immediately by ordinary text"sv, [&] {
        auto console = make_console();
        input.Feed("\x1b日"sv);
        Require(console.ReadTextInput().key == MenuKey::Back,
            "Unicode text following Escape hid the Escape key"sv);
        Require(console.ReadTextInput().character == U'日',
            "Escape consumed the following Unicode character"sv);
    });
#ifndef _WIN32
    passed &= run_test("modified-key reporting enabled and restored on the Unix alternate screen"sv, [&] {
        constexpr auto expected = vt::Concatenate<
            vt::kEnterAlternateScreen, vt::kEnableModifiedKeys, vt::kPushDisambiguatedKeys,
            vt::kSaveCursorVisibility, vt::kHideCursor,
            vt::kPopKeyboardMode, vt::kResetModifiedKeys, vt::kResetAttributes, vt::kShowCursor,
            vt::kRestoreCursorVisibility, vt::kLeaveAlternateScreen>();
        for (const auto unwind : std::array{false, true}) {
            auto console = make_console.template operator()<NativeConsole>();
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
    passed &= run_test("native update measurements counting begin, presentation, and guard output"sv, [&] {
        // BSU (`CSI ? 2026 h`) begins a synchronized update. ESU (`CSI ? 2026 l`)
        // ends it, once for presentation and once more when the guard is destroyed.
        // https://github.com/contour-terminal/vt-extensions/blob/master/synchronized-output.md
        constexpr auto update_sequences = vt::Concatenate<vt::kBeginSynchronizedUpdate,
            vt::kEndSynchronizedUpdate, vt::kEndSynchronizedUpdate>();
        auto console = make_console.template operator()<MeasuringConsole<NativeConsole>>();
        {
            const auto update = console.BeginUpdate();
            console.PresentFrame();
        }
        input.Feed("\x0c"sv);
        Require(ReadKeyboardInput([&console](const auto) {
                return console.ReadMenuInput();
            }, TestClock::time_point::max()).key == MenuKey::Redraw,
            "recording an update changed the supplied input"sv);
        const auto &measurement = console.measurements.at(0);
        Require(measurement.writes == 3, "an update control write was not counted"sv);
        Require(measurement.bytes == update_sequences.size(),
            "update measurements omitted control-sequence bytes"sv);
    });
    passed &= run_test("native cursor reply with keys before and after it"sv, [&] {
        auto console = make_console();
        console.on_write = [&input](const auto text) {
            Require(text == detail::ReportRequest(detail::TerminalReport::Cursor),
                "unexpected query"sv);
            // CPR (`CSI row;column R`) reports (4, 9).
            // Ctrl+L precedes the report and the `1` shortcut follows it.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
            input.Feed("\x0c\x1b[4;"sv);
            input.Feed("9R1"sv);
        };
        Require(console.QueryCursor() == CursorPosition{4, 9},
            "the cursor reply was not recognized"sv);
        RequireInput(console.ReadTextInput(), {.key = MenuKey::Redraw},
            "the key preceding the cursor reply"sv);
        RequireInput(console.ReadTextInput(),
            {.key = MenuKey::Text, .character = U'1'},
            "the key following the cursor reply"sv);
    });
    passed &= run_test("native cancellation interrupting a cursor query and preserving other keys"sv, [&] {
        auto console = make_console();
        console.on_write = [&](const auto) { input.Feed("\x0c\x03"sv); };
        try {
            std::ignore = console.QueryCursor();
            Require(false, "Ctrl+C did not cancel the query"sv);
        } catch (const InputCancelled &) {
        }
        Require(console.ReadMenuInput().key == MenuKey::Redraw, "cancellation consumed another key"sv);
        // CPR reports row 7, column 8 after the cancelled query.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        console.on_write = [&](const auto) { input.Feed("\x1b[7;8R"sv); };
        Require(console.QueryCursor() == CursorPosition{7, 8}, "a later query inherited cancellation"sv);
    });
    passed &= run_test("native menu cancellation during size and cursor queries"sv, [&] {
        for (const auto cancel_size : {true, false}) {
            auto console = make_console();
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
    passed &= run_test("native cancellation queued after a completed report"sv, [&] {
        auto console = make_console();
        // CPR (`CSI 4;9 R`) reports the cursor, followed by Ctrl+C (ETX, 0x03).
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        console.on_write = [&](const auto) { input.Feed("\x1b[4;9R\x03"sv); };
        Require(console.QueryCursor() == CursorPosition{4, 9}, "the first report was lost"sv);
        console.on_write = {};
        try {
            std::ignore = console.QueryCursor();
            Require(false, "a queued Ctrl+C did not cancel the next query"sv);
        } catch (const InputCancelled &) {
        }
    });
    passed &= run_test("native terminal modes restored after cancellation and an output exception"sv, [&] {
        Require(input.Modes() == original_modes, "normal exit or cancellation changed terminal modes"sv);
        try {
            auto console = make_console();
            const auto screen = console.EnterScreen();
            console.on_write = [](const auto) { throw InjectedFailure{"injected output failure"}; };
            std::ignore = SelectTestMenuItem(console, {}, std::array{"Installation"sv});
            Require(false, "the output exception did not propagate"sv);
        } catch (const InjectedFailure &) {
        }
        Require(input.Modes() == original_modes, "exception unwinding changed terminal modes"sv);
    });
    passed &= run_test("native text input decodes Unicode and modified Enter without consuming following text"sv, [&] {
        constexpr auto kKittyAltReturn = "\x1b[13;3u"sv;
        constexpr auto kKittyMetaReturn = "\x1b[13;33u"sv;
        constexpr auto kXtermAltReturn = "\x1b[27;3;13~"sv;
        constexpr auto kXtermMetaReturn = "\x1b[27;9;13~"sv;
        for (const auto sequence : std::array{
                "\x1b[13;2u"sv, "\x1b[13;66u"sv, "\x1b[57414;2u"sv,
                "\x1b[13;2:1u"sv, "\x1b[13;2:2u"sv,
                "\x1b[27;2;13~"sv, "\n"sv, vt::kMetaReturn,
                kKittyAltReturn, kKittyMetaReturn, kXtermAltReturn, kXtermMetaReturn}) {
            auto console = make_console();
            input.Feed(std::format("{}日😀", sequence));
            Require(console.ReadTextInput().key == MenuKey::Newline,
                std::format("modified Enter {:?} did not insert a newline", sequence));
            const auto japanese = console.ReadTextInput();
            const auto emoji = console.ReadTextInput();
            Require((japanese.key == MenuKey::Text) && (japanese.character == U'日') &&
                (emoji.key == MenuKey::Text) && (emoji.character == U'😀'),
                "decoding modified Enter damaged the following Unicode text"sv);
        }
    });
    passed &= run_test("keyboard reports cannot insert invalid Unicode or unhandled function keys"sv, [&] {
        auto console = make_console();
        input.Feed("\x1b[55296u\x1b[1114112u\x1b[57376uZ"sv);
        Require(console.ReadTextInput().character == U'Z',
            "a non-text key report reached the editor as text"sv);
    });
    passed &= run_test("encoded Ctrl+C cancels a query while preserving neighboring text"sv, [&] {
        for (const auto sequence : std::array{"\x1b[99;5u"sv, "\x1b[27;5;99~"sv}) {
            auto console = make_console();
            console.on_write = [&](const auto) { input.Feed(std::format("a{}z", sequence)); };
            try {
                std::ignore = console.QueryCursor();
                Require(false, "encoded Ctrl+C did not cancel the cursor query"sv);
            } catch (const InputCancelled &) {
            }
            Require((console.ReadTextInput().character == U'a') &&
                (console.ReadTextInput().character == U'z'),
                "query cancellation consumed neighboring text"sv);
        }
    });
    passed &= run_test("fragmented Shift+Enter and Meta Return survive an input deadline"sv, [&] {
        constexpr auto kShiftEnter = "\x1b[13;2u"sv;
        constexpr auto kShiftEnterRelease = "\x1b[13;2:3u"sv;
        for (const auto sequence : {kShiftEnter, vt::kMetaReturn}) {
            for (const auto split : std::views::iota(1uz, sequence.size())) {
                auto console = make_console();
                queue_prefix(console, sequence.substr(0, split));
                const auto deadline = TestClock::now() + 1ms;
                Require(console.ReadTextInput(deadline).key == MenuKey::Timeout,
                    "an incomplete modified Enter was treated as a key"sv);
                input.Feed(std::format("{}{}Z", sequence.substr(split), kShiftEnterRelease));
                TestClock::current += 100ms;
                Require(console.ReadTextInput().key == MenuKey::Newline,
                    "fragmenting modified Enter changed its action"sv);
                Require(console.ReadTextInput().character == U'Z',
                    "a key release inserted text or consumed the following key"sv);
            }
        }
    });
#ifdef _WIN32
    passed &= run_test("query cancellation preserves a resize inside an encoded Ctrl+C"sv, [&] {
        auto console = make_console();
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
        Require((console.NativeConsole::ReadTextInput().key ==
                MenuKey::Resize) &&
            (console.ReadTextInput().character == U'z'),
            "removing a cancellation report also removed unrelated input"sv);
    });
    passed &= run_test("Windows pasted emoji survive Alt key-up records and intervening modifiers"sv, [&] {
        auto console = make_console();
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
        const auto expected = Transcode<char32_t>(text);
        for (const auto character : std::u32string_view{expected}) {
            const auto decoded = console.ReadTextInput();
            Require((decoded.key == MenuKey::Text) && (decoded.character == character) &&
                (decoded.repeat == 1), "pasted Unicode was dropped or changed"sv);
        }
        Require(console.ReadTextInput().character == U'Z',
            "an ordinary key release inserted duplicate text"sv);
    });
#endif
    passed &= run_test("native query timeout, a late reply, and subsequent successful query"sv, [&] {
        auto console = make_console();
        Require(!console.QueryCursor(), "a missing report produced a cursor position"sv);
        // A late CPR reports row 1, column 1; Ctrl+L follows as ordinary input.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        input.Feed("\x1b[1;1R\x0c"sv);
        Require(console.ReadTextInput().key == MenuKey::Redraw,
            "a late report was interpreted as a menu shortcut"sv);
        // CPR reports row 2, column 3 in response to the next query.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        console.on_write = [&](const auto) { input.Feed("\x1b[2;3R"sv); };
        Require(console.QueryCursor() == CursorPosition{2, 3}, "a query could not recover after timeout"sv);
    });
#ifdef _WIN32
    passed &= run_test("successive menus and scrolling through native Console Host"sv, [&] {
        TestClock::wait_policy = WaitPolicy::Block;
        auto console = make_console.template operator()<MeasuringConsole<NativeConsole>>();
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
    passed &= run_test("arriving Unicode output and a command selection through native Console Host"sv, [&] {
        TestClock::wait_policy = WaitPolicy::Block;
        auto console = make_console.template operator()<OutputConsole>();
        const auto screen = console.EnterScreen();
        auto view = OutputMenu{};
        view.SetCommands(std::array{OutputCommand{7, "Open folder"sv}});
        auto updates = 0;
        const auto selected = view.Select(console, [](auto &frame) {
            frame.Write("Output menu native test\n"sv);
        }, [&input, &console, &updates](auto &output) {
            ++updates;
            if (updates == 1) {
                output.AppendLine("日本語 — café — é — 👩‍💻");
            } else if (updates == 2) {
                output.AppendLine("Mount ready.");
            } else if (console.timeouts == 2) {
                auto enter = INPUT_RECORD{.EventType = KEY_EVENT};
                enter.Event.KeyEvent = {.bKeyDown = TRUE, .wRepeatCount = 1,
                    .wVirtualKeyCode = VK_RETURN, .wVirtualScanCode = 0,
                    .uChar = {.UnicodeChar = L'\r'}, .dwControlKeyState = 0};
                input.FeedRecords(std::span{&enter, 1});
            }
        });
        Require(selected == 7, "the output view could not select its native command"sv);
        Require(console.timeouts == 2,
            "the output view did not resume after two idle input waits"sv);
    });
    passed &= run_test("multiline text editing through native Console Host"sv, [&] {
        TestClock::wait_policy = WaitPolicy::Block;
        auto console = make_console.template operator()<NativeConsole>();
        const auto screen = console.EnterScreen();
        input.Feed("é日👩‍💻\x1b[D\x1b[3~\x1b[13;2uZ\r"sv);
        const auto result = EditText(console, [](auto &frame) {
            frame.Write("Enter accepts; Shift+Enter inserts a newline.\n\n");
        });
        Require(result == "é日\nZ",
            "the native editor did not preserve Unicode around deletion and a newline"sv);
    });
    passed &= run_test("Windows console host emitting character-only cursor-reply events"sv, [&] {
        auto console = make_console.template operator()<NativeConsole>();
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
    passed &= run_test("Windows report removal preserving repeat counts, Unicode, and resize events"sv, [&] {
        auto console = make_console();
        const auto screen = console.EnterScreen();
        // This case supplies native key records, so detection must finish and
        // disable VT input before Console Host receives those records.
        TestClock::current += 5s;
        input.Feed("\t"sv);
        Require(console.ReadTextInput().key == MenuKey::SwitchArea,
            "native report-removal setup did not consume its Tab"sv);
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
        // Console Host can prepend incidental size notifications. The resize
        // supplied by this case follows its key and must remain there.
        const auto retained_record = [&console] {
            for (;;) {
                const auto record = console.ReadInput();
                if (record.EventType != WINDOW_BUFFER_SIZE_EVENT) {
                    return record;
                }
            }
        }();
        Require(retained_record.EventType == KEY_EVENT,
            "the saved key event was replaced by another event type"sv);
        const auto &retained = retained_record.Event.KeyEvent;
        Require((retained.bKeyDown == key.Event.KeyEvent.bKeyDown) &&
            (retained.wRepeatCount == 3) && (retained.wVirtualKeyCode == VK_DOWN) &&
            (retained.wVirtualScanCode == 80) && (retained.uChar.UnicodeChar == L'界') &&
            (retained.dwControlKeyState == SHIFT_PRESSED), "the saved key event changed"sv);
        const auto retained_resize = console.ReadInput();
        Require((retained_resize.EventType == WINDOW_BUFFER_SIZE_EVENT) &&
            (retained_resize.Event.WindowBufferSizeEvent.dwSize.X ==
                resize.Event.WindowBufferSizeEvent.dwSize.X) &&
            (retained_resize.Event.WindowBufferSizeEvent.dwSize.Y ==
                resize.Event.WindowBufferSizeEvent.dwSize.Y),
            "the supplied resize event was lost or changed"sv);
        // CPR (`CSI 1;1 R`) is split around a key event and followed by Ctrl+L.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        input.Feed("\x1b[1;"sv);
        input.FeedRecords(std::array{key});
        input.Feed("1R\x0c"sv);
        const auto navigation = console.ReadMenuInput();
        Require((navigation.key == MenuKey::Down) && (navigation.repeat == 3),
            "an unfinished late reply consumed physical navigation"sv);
        Require(console.ReadTextInput().key == MenuKey::Redraw,
            "the remainder of a late reply became a shortcut after navigation"sv);
    });
#endif
    passed &= run_test("native cancellation inside complete and fragmented escape sequences"sv, [&] {
        for (const auto fragmented : {false, true}) {
            auto console = make_console();
            if (!fragmented) {
                // CPR (`CSI 12;34 R`) contains an inserted Ctrl+C, then
                // Ctrl+L follows the report as the next command to the menu.
                // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
                input.Feed("\x1b[12;\x03" "34R\x0c"sv);
            } else {
                // The same CPR (`CSI 12;34 R`) arrives in separate fragments,
                // with Ctrl+C between the row and column and Ctrl+L afterward.
                // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
                queue_prefix(console, "\x1b[12;"sv);
                input.Feed("\x03"sv);
                input.Feed("34R\x0c"sv);
            }
            Require(console.ReadTextInput().key == MenuKey::Cancel,
                "a terminal reply swallowed Ctrl+C"sv);
            Require(console.ReadTextInput().key == MenuKey::Redraw,
                "cancellation lost a later key or exposed part of the reply as input"sv);
        }
    });
#ifndef _WIN32
    passed &= run_test("Unix interrupted waits retaining the query's original deadline"sv, [&] {
        auto console = make_console();
        interrupted_wait.emplace();
        const auto reset = ScopeExit{[] { interrupted_wait.reset(); }};
        Require(!console.QueryCursor(), "an interrupted missing reply produced a cursor position"sv);
        Require(interrupted_wait->calls == 2,
            "the interrupted native wait did not retry"sv);
        Require(std::ranges::equal(interrupted_wait->timeouts,
                std::array{5s, 0s}),
            "the interrupted native wait did not retain its original deadline"sv);
    });
#endif
    passed &= run_test("kitty keyboard protocol Escape, shortcuts, modifiers, and keypad navigation"sv, [&] {
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
            auto console = make_console();
            input.Feed(std::format("{}1", sequence));
            const auto start = std::chrono::steady_clock::now();
            Require(console.ReadMenuInput().key == expected,
                std::format("incorrect action for encoded key {:?}", sequence));
            if (sequence == "\x1b[27u"sv) {
                Println("Encoded Escape: {:.3f} ms", std::chrono::duration<double, std::milli>{
                    std::chrono::steady_clock::now() - start}.count());
            }
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'1'},
                "text immediately following the encoded key"sv);
        }
    });
    passed &= run_test("unrelated and malformed kitty keyboard reports leaving the next command intact"sv, [&] {
        // These CSI u sequences include unrelated modifiers, unknown keys,
        // invalid numeric fields, and optional enhancements we did not enable.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#an-overview
        for (const auto sequence : std::array{
                "\x1b[99;3u"sv, "\x1b[99;7u"sv,
                "\x1b[999999999999999999999u"sv, "\x1b[27;0u"sv,
                "\x1b[27;999999999999999999999u"sv, "\x1b[;5u"sv,
                "\x1b[99;5:3u"sv, "\x1b[?1u"sv}) {
            auto console = make_console();
            input.Feed(std::format("{}1", sequence));
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'1'},
                std::format("text following unrelated encoded input {:?}",
                    sequence));
        }
    });
    passed &= run_test("ordinary encoded text preserving the following character"sv,
        [&input, &make_console] {
            auto console = make_console();
            input.Feed("\x1b[99u1"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'c'},
                "ordinary encoded text"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'1'},
                "text following the encoded character"sv);
        });
    passed &= run_test("kitty keyboard protocol Escape surviving every input split"sv, [&] {
        // Kitty reports Escape as CSI 27 u, with a final byte distinguishing it
        // from both a lone legacy Escape and an incomplete sequence.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
        constexpr auto escape = "\x1b[27u"sv;
        for (const auto split : std::views::iota(1uz, escape.size())) {
            auto console = make_console();
            queue_prefix(console, escape.substr(0, split));
            const auto deadline = TestClock::now() + 1ms;
            Require(console.ReadTextInput(deadline).key == MenuKey::Timeout,
                "the partial encoded Escape did not reach its deadline"sv);
            input.Feed(std::format("{}1", escape.substr(split)));
            TestClock::current += 100ms;
            Require(console.ReadTextInput().key == MenuKey::Back,
                "a fragmented encoded Escape was not recognized"sv);
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'1'},
                "text immediately following the fragmented Escape"sv);
        }
    });
    passed &= run_test("fragmented kitty keyboard protocol Ctrl+C interrupting cursor and size queries"sv, [&] {
        // CSI 99;5 u is Ctrl+C; CSI 108;5 u is Ctrl+L. Leave Ctrl+L before
        // cancellation and the `1` shortcut after it to verify queue retention.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        constexpr auto cancel = "\x1b[99;5u"sv;
        for (const auto size_query : std::array{false, true}) {
            for (const auto split : std::views::iota(1uz, cancel.size())) {
                auto console = make_console();
                queue_prefix(console, std::format(
                    "\x1b[108;5u{}", cancel.substr(0, split)));
                console.on_write = [&input, split, cancel](const auto) {
                    input.Feed(std::format("{}1", cancel.substr(split)));
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
                Require(console.ReadTextInput().key == MenuKey::Redraw,
                    "cancellation lost the preceding Ctrl+L"sv);
                RequireInput(console.ReadTextInput(),
                    {.key = MenuKey::Text, .character = U'1'},
                    "text following fragmented query cancellation"sv);
            }
        }
    });
    passed &= run_test("native query replies retaining their order relative to kitty keyboard protocol Ctrl+C"sv, [&] {
        // A CPR reports (4, 9); CSI 99;5 u cancels. Test both arrival orders.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        for (const auto reply_first : std::array{false, true}) {
            auto console = make_console();
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
            RequireInput(console.ReadTextInput(),
                {.key = MenuKey::Text, .character = U'1'},
                "text following query cancellation"sv);
        }
    });
    passed &= run_test("native fragmented navigation and a lone Escape"sv, [&] {
        auto console = make_console();
        // The Down key is `CSI B` in normal cursor-key mode, split after `ESC [`.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
        queue_prefix(console, "\x1b["sv);
        const auto deadline = TestClock::now() + 1ms;
        Require(console.ReadTextInput(deadline).key == MenuKey::Timeout,
            "the partial arrow did not reach its deadline"sv);
        input.Feed("BZ"sv);
        TestClock::current += 100ms;
        Require(console.ReadTextInput().key == MenuKey::Down,
            "a fragmented arrow became Escape"sv);
        RequireInput(console.ReadTextInput(),
            {.key = MenuKey::Text, .character = U'Z'},
            "text immediately following the fragmented arrow"sv);
        // A standalone ESC (0x1B) represents the Escape key rather than a sequence.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        queue_prefix(console, "\x1b"sv);
        Require(console.ReadMenuInput().key == MenuKey::Back, "a lone Escape did not produce Back"sv);
        input.Feed("\r"sv);
        Require(console.ReadMenuInput().key == MenuKey::Accept, "Enter did not produce Accept"sv);
    });
    passed &= run_test("native terminal disconnection reporting I/O errors and preserving the primary failure"sv, [&] {
        try {
            auto console = make_console();
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
            Require((message.contains("terminal"sv)) &&
                (message.contains("input"sv)),
                std::format("expected a terminal input failure after "
                    "disconnection; received: {}", message));
            return;
        }
        Require(false, "disconnected terminal input returned normally"sv);
    });
    return passed;
}

#ifndef _WIN32
auto WithNativeInput(const auto &operation) -> void {
    // Closing the controlling terminal's primary sends `SIGHUP` to this session
    // leader. Ignoring it until after `NativeInput` is destroyed lets setup
    // exceptions reach `Test` instead of terminating during cleanup.
    const auto hangup = SetSignalHandlerScoped(SIGHUP, SIG_IGN);
    // Creating the pseudoterminal after `fork` keeps its primary descriptor out
    // of the parent, whose copy would otherwise prevent disconnection.
    // Standard output and error remain connected to CTest rather than the
    // controlling terminal, keeping drawing codes out of results.
    auto input = NativeInput{};
    input.Attach();
    std::invoke(operation, input);
}

[[nodiscard]] auto RunIsolatedNativeTest(const std::string_view name,
    const auto &operation) -> int {
    const auto child = fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not create the native test process");
    }
    if (child == 0) {
        // Returning would resume the parent's remaining tests in this child.
        std::exit(Test(name, operation) ? 0 : 1);
    }
    auto status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                "could not collect the native test process");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        const auto signal = WTERMSIG(status);
        Println(stderr, "FAIL: {}: child process {} terminated by signal {} "
            "({}).", name, child, signal, strsignal(signal));
    }
    return 1;
}
#endif

}

export auto RunNativeTests() -> int {
#ifdef _WIN32
    // The CTest launcher gives this process a hidden, private Windows console.
    auto input = NativeInput{};
    return TestNativeInput(input) ? 0 : 1;
#else
    const auto cleanup_status = RunIsolatedNativeTest(
        "Unix native fixture cleanup preserving a setup exception"sv, [] {
            constexpr auto message = "injected native fixture setup failure"sv;
            try {
                WithNativeInput([message](auto &) {
                    throw std::runtime_error(std::string{message});
                });
            } catch (const std::runtime_error &error) {
                Require(std::string_view{error.what()} == message,
                    std::format("expected the injected setup exception after "
                        "native fixture cleanup; received: {}", error.what()));
                return;
            }
            Require(false, "the injected setup exception was not reported"sv);
        });
    const auto native_status = RunIsolatedNativeTest(
        "isolated Unix native input tests"sv, [] {
            WithNativeInput([](auto &input) {
                Require(TestNativeInput(input, input.DevicePath()),
                    "one or more native input tests failed"sv);
            });
        });
    return ((cleanup_status == 0) && (native_status == 0)) ? 0 : 1;
#endif
}
