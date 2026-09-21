// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.windows;

import std;
#if __has_include(<devicefs/windows_imports.h>)
    import <devicefs/windows_imports.h>;
#else
    import <windows.h>;
    import <wil/resource.h>;
    import <wil/stl.h>;
#endif
import devicefs.terminal;
import devicefs.terminal.base_console;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.transcoding;
import devicefs.terminal.vt;

using namespace wil::literals;
using namespace std::chrono_literals;

namespace devicefs::terminal::detail {

[[nodiscard]] auto SetConsoleModeScoped(
    const HANDLE handle, const DWORD enable, const DWORD disable) {
    auto previous = DWORD{};
    if (!GetConsoleMode(handle, &previous)) {
        throw std::system_error(GetLastError(),
            std::system_category(), "could not read the terminal's console mode");
    }
    auto restore = wil::scope_exit([handle, previous] {
        std::ignore = SetConsoleMode(handle, previous);
    });
    if (!SetConsoleMode(handle, (previous | enable) & ~disable)) {
        throw std::system_error(GetLastError(),
            std::system_category(), "could not enable terminal input or output");
    }
    return restore;
}

[[nodiscard]] auto OpenConsole(const wil::zstring_view name) {
    auto handle = wil::unique_hfile{CreateFileA(name.c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!handle) {
        const auto error = GetLastError();
        throw std::system_error(std::bit_cast<int>(error),
            std::system_category(), std::format("could not open '{}'",
                std::string_view{name}));
    }
    return handle;
}

}

export namespace devicefs::terminal {

// `WindowsConsole` supplies terminal output, cursor and size queries, and input
// events for the Windows console attached to this process. Cursor positions and
// dimensions come from VT replies; the Windows console or its hosting terminal
// supplies those reports. Keyboard and resize events are available through
// `ReadInput`.
//
// The object opens the console handles and enables the modes needed for these
// operations. Destruction restores the previous modes before closing the handles.
// A query returns an empty optional when a usable report is unavailable within
// its timeout. Failures to open, configure, read, or write the console throw
// exceptions. Ctrl+C during a query raises InputCancelled.
class WindowsConsole : public BaseConsole {
public:
    WindowsConsole() = default;

    auto Write(const std::string_view text) -> void {
        if (text.empty()) {
            return;
        }
        // `Write` accepts UTF-8 regardless of the console's output code page.
        // `WriteConsoleW` accepts UTF-16 directly, so this conversion lets the
        // caller display Unicode text while preserving the console code page
        // shared with other programs. Microsoft's WriteConsole documentation
        // describes the separate encodings accepted by the A and W functions:
        // https://learn.microsoft.com/en-us/windows/console/writeconsole
        const auto wide = Transcode<wchar_t>(text);
        if (wide.size() > std::numeric_limits<DWORD>::max()) {
            throw std::length_error(std::format(
                "terminal text has {} UTF-16 code units; WriteConsoleW's "
                "count parameter can represent at most {}",
                wide.size(), std::numeric_limits<DWORD>::max()));
        }
        const auto length = FailFastCast<DWORD>(wide.size());
        auto written = DWORD{};
        if (!WriteConsoleW(output_.get(), wide.data(), length,
                &written, nullptr)) {
            throw std::system_error(GetLastError(),
                std::system_category(), "could not write terminal text");
        }
        if (written != length) {
            throw std::runtime_error(std::format(
                "the terminal accepted {} of {} UTF-16 code units",
                written, length));
        }
    }

    // `ReadInput` supplies the events used to operate the interface. Queries may
    // have encountered keyboard or resize events while waiting for a report, so
    // those saved events are delivered before reading more from the console.
    [[nodiscard]] auto ReadInput() -> INPUT_RECORD {
        if (!pending_.empty()) {
            const auto record = pending_.front();
            pending_.pop_front();
            return record;
        }
        return ReadConsoleRecord();
    }

    // Wait for text or navigation until `deadline`. Timeout returns control to callers
    // that also display arriving output; omitting the deadline waits for input.
    [[nodiscard]] auto ReadTextInput(const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max()) -> MenuInput {
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return {.key = MenuKey::Timeout};
            }
            if (pending_.empty()) {
                if (keyboard_detection_deadline_ &&
                    (std::chrono::steady_clock::now() >= *keyboard_detection_deadline_)) {
                    SelectKeyboardProtocol(detail::KeyboardProtocol::Legacy);
                }
                if (!ReceiveUntil(std::min(deadline,
                        keyboard_detection_deadline_.value_or(deadline)))) {
                    continue;
                }
            }
            if (IsSequenceCharacter(pending_.front()) &&
                (pending_.front().Event.KeyEvent.uChar.UnicodeChar == vt::kEscape)) {
                if (const auto action = ReadEscape(deadline)) {
                    return *action;
                }
                continue;
            }
            const auto record = ReadInput();
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                return {.key = MenuKey::Resize};
            }
            if (record.EventType != KEY_EVENT) {
                continue;
            }
            const auto &key = record.Event.KeyEvent;
            // Console Host can represent pasted characters as Alt+numpad input,
            // with the character on the `VK_MENU` key-up record. Discarding all
            // key-up records would therefore lose pasted symbols and emoji.
            // Ordinary key releases repeat previously delivered text and are ignored.
            // https://github.com/microsoft/terminal/blob/main/src/interactivity/base/EventSynthesis.cpp
            if (!key.bKeyDown &&
                ((key.wVirtualKeyCode != VK_MENU) || (key.uChar.UnicodeChar == L'\0'))) {
                continue;
            }
            if (key.uChar.UnicodeChar == L'\x03') {
                return {.key = MenuKey::Cancel};
            }
            // Console input supplies UTF-16 code units. A supplementary character
            // arrives as two records, which must become one editing operation.
            const auto character = key.uChar.UnicodeChar;
            if ((character >= 0xd800) && (character <= 0xdbff)) {
                high_surrogate_ = character;
                continue;
            }
            if ((character >= 0xdc00) && (character <= 0xdfff)) {
                if (const auto high = std::exchange(high_surrogate_, std::nullopt)) {
                    const auto pair = std::array{*high, character};
                    const auto decoded = Transcode<char32_t>(std::wstring_view{pair.data(), pair.size()});
                    return {.key = MenuKey::Text, .repeat = key.wRepeatCount,
                        .character = decoded.data()[0]};
                }
                continue;
            }
            // Alt+numpad's modifier and digit records contain no character.
            // Those records can occur between the two UTF-16 code units of an
            // emoji, so they must not discard the saved high surrogate.
            if (character != L'\0') {
                high_surrogate_.reset();
            }
            escape_deadline_.reset();
            auto action = [&key]() -> std::optional<MenuInput> {
                switch (key.wVirtualKeyCode) {
                case VK_LEFT:
                    return MenuInput{MenuKey::Left};
                case VK_RIGHT:
                    return MenuInput{MenuKey::Right};
                case VK_BACK:
                    return MenuInput{MenuKey::Backspace};
                case VK_DELETE:
                    return MenuInput{MenuKey::Delete};
                case VK_UP:
                    return MenuInput{MenuKey::Up};
                case VK_DOWN:
                    return MenuInput{MenuKey::Down};
                case VK_PRIOR:
                    return MenuInput{MenuKey::PageUp};
                case VK_NEXT:
                    return MenuInput{MenuKey::PageDown};
                case VK_HOME:
                    return MenuInput{MenuKey::Home};
                case VK_END:
                    return MenuInput{MenuKey::End};
                case VK_RETURN:
                    // With VT input disabled, Console Host represents ESC+CR
                    // as Return with `LEFT_ALT_PRESSED`. Apple Terminal sends
                    // that sequence for Shift+Return, so the native decoder
                    // must recognize it after keyboard detection selects this mode.
                    // https://github.com/microsoft/terminal/blob/main/src/terminal/parser/InputStateMachineEngine.cpp#L156-L258
                    return MenuInput{((key.dwControlKeyState &
                        (SHIFT_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0) ?
                        MenuKey::Newline : MenuKey::Accept};
                case VK_ESCAPE:
                    return MenuInput{MenuKey::Back};
                case VK_TAB:
                    return MenuInput{MenuKey::SwitchArea};
                default:
                    break;
                }
                return CharacterInput(key.uChar.UnicodeChar);
            }();
            if (action) {
                action->repeat = key.wRepeatCount;
                return *action;
            }
        }
    }

private:
    friend class BaseConsole;

    static constexpr auto kEnterScreen =
        vt::Concatenate<vt::kEnterAlternateScreen, vt::kHideCursor>();

protected:
    // These control sequences contain only ASCII, so WriteConsoleA can send
    // them under any console code page without allocating a UTF-16 copy.
    // Cleanup ignores output failure to preserve an exception already in flight.
    auto WriteControlSequenceNoThrow(const std::string_view sequence) const noexcept -> void {
        auto written = DWORD{};
        std::ignore = WriteConsoleA(output_.get(), sequence.data(),
            FailFastCast<DWORD>(sequence.size()), &written, nullptr);
    }

private:
    [[nodiscard]] auto RestoreScreenOnExit() {
        auto previous_cursor = CONSOLE_CURSOR_INFO{};
        if (!GetConsoleCursorInfo(output_.get(), &previous_cursor)) {
            throw std::system_error(GetLastError(),
                std::system_category(), "could not read the console cursor visibility");
        }
        return wil::scope_exit([this, previous_cursor] {
            keyboard_detection_deadline_.reset();
            WriteControlSequenceNoThrow(keyboard_restore_);
            keyboard_restore_ = {};
            WriteControlSequenceNoThrow(vt::kLeaveAlternateScreen);
            std::ignore = SetConsoleCursorInfo(output_.get(), &previous_cursor);
        });
    }

    // Windows must receive VT input to read Kitty and xterm keyboard reports.
    // When neither extension is available, native key records provide more
    // information: local Shift+Enter has a Shift modifier that legacy VT input
    // cannot express. Detection begins on screen entry; the ordinary input
    // loop receives the replies while the caller renders and uses the screen.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#input-sequences
    //
    // Some Console Host versions answer device-attributes and window-size
    // queries themselves before the remote terminal's keyboard reply arrives.
    // Those replies therefore cannot establish that the keyboard extensions
    // are unsupported. Only the timeout permits fallback to native input.
    auto ConfigureKeyboard(this auto &self) -> void {
        constexpr auto kDetectionTimeout = 5s;
        auto mode = DWORD{};
        if (!GetConsoleMode(self.input_.get(), &mode) ||
            !SetConsoleMode(self.input_.get(), mode | ENABLE_VIRTUAL_TERMINAL_INPUT)) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not enable VT input for keyboard detection");
        }
        self.keyboard_detection_deadline_ = std::chrono::steady_clock::now() + kDetectionTimeout;
        self.Write(vt::kRequestKeyboardSupport);
    }

    // Keyboard replies share the input queue with keystrokes. `ReadEscape`
    // assembles each complete sequence before passing it here, so a reply can
    // finish detection without adding a separate input wait or discarding keys.
    auto HandleTerminalReply(const std::string_view sequence) -> void {
        if (!keyboard_detection_deadline_) {
            return;
        }
        if (detail::ParseTerminalReport(sequence, detail::TerminalReport::KeyboardFlags)) {
            SelectKeyboardProtocol(detail::KeyboardProtocol::Kitty);
        } else if (detail::ParseTerminalReport(sequence, detail::TerminalReport::ModifiedKeys)) {
            SelectKeyboardProtocol(detail::KeyboardProtocol::Xterm);
        }
    }

    auto SelectKeyboardProtocol(const detail::KeyboardProtocol protocol) -> void {
        using enum detail::KeyboardProtocol;
        keyboard_detection_deadline_.reset();
        switch (protocol) {
        case Kitty:
            keyboard_restore_ = vt::kPopKeyboardMode;
            Write(vt::kPushDisambiguatedKeys);
            break;
        case Xterm:
            keyboard_restore_ = vt::kResetModifiedKeys;
            Write(vt::kEnableModifiedKeys);
            break;
        case Legacy: {
            auto mode = DWORD{};
            if (!GetConsoleMode(input_.get(), &mode) ||
                !SetConsoleMode(input_.get(), mode & ~ENABLE_VIRTUAL_TERMINAL_INPUT)) {
                throw std::system_error(GetLastError(), std::system_category(),
                    "could not enable native keyboard input");
            }
            break;
        }
        }
    }

    [[nodiscard]] auto ReadConsoleRecord() const -> INPUT_RECORD {
        auto record = INPUT_RECORD{};
        auto read = DWORD{};
        if (!ReadConsoleInputW(input_.get(), &record, 1, &read)) {
            throw std::system_error(GetLastError(),
                std::system_category(), "could not read terminal input");
        }
        if (read != 1) {
            throw std::runtime_error("the terminal returned no input record");
        }
        return record;
    }

    // Receive one console record before `deadline`, retaining its native key
    // and resize information for the keyboard reader or a pending VT query.
    [[nodiscard]] auto ReceiveUntil(const std::chrono::steady_clock::time_point deadline)
        -> bool {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= remaining.zero()) {
            return false;
        }
        const auto timeout = (deadline == std::chrono::steady_clock::time_point::max()) ?
            INFINITE : FailFastCast<DWORD>(std::min<std::int64_t>(INFINITE - 1,
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
        const auto wait = WaitForSingleObject(input_.get(), timeout);
        if (wait == WAIT_TIMEOUT) {
            return false;
        }
        if (wait == WAIT_FAILED) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not wait for terminal input");
        }
        pending_.push_back(ReadConsoleRecord());
        return true;
    }

    [[nodiscard]] static auto IsSequenceCharacter(const INPUT_RECORD &record) noexcept -> bool {
        return (record.EventType == KEY_EVENT) && record.Event.KeyEvent.bKeyDown &&
            (record.Event.KeyEvent.wVirtualKeyCode == 0);
    }

    [[nodiscard]] auto SequenceCharacters() const noexcept {
        return pending_ | std::views::filter(IsSequenceCharacter) |
            std::views::transform([](const INPUT_RECORD &record) {
                return record.Event.KeyEvent.uChar.UnicodeChar;
            });
    }

    auto DiscardSequenceCharacters(std::size_t count) noexcept -> void {
        for (auto position = pending_.begin(); (position != pending_.end()) && (count != 0);) {
            if (IsSequenceCharacter(*position)) {
                position = pending_.erase(position);
                --count;
            } else {
                ++position;
            }
        }
    }

    // Cancel a pending query for either a native Ctrl+C record or a terminal
    // keyboard report. Removing only the cancellation leaves neighboring input
    // available to the next control.
    [[nodiscard]] auto ConsumeCancellation() -> bool {
        auto sequence = std::string{};
        auto positions = std::vector<decltype(pending_)::iterator>{};
        for (auto position = pending_.begin(); position != pending_.end(); ++position) {
            const auto &record = *position;
            if ((record.EventType != KEY_EVENT) || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto character = record.Event.KeyEvent.uChar.UnicodeChar;
            if (character == L'\x03') {
                pending_.erase(position);
                return true;
            }
            if (record.Event.KeyEvent.wVirtualKeyCode != 0) {
                continue;
            }
            if (character == vt::kEscape) {
                positions.assign(1, position);
                sequence.assign(1, vt::kEscape);
                continue;
            }
            if (positions.empty()) {
                continue;
            }
            if (character >= 128) {
                positions.clear();
                continue;
            }
            positions.push_back(position);
            sequence.push_back(FailFastCast<char>(character));
            if (KeySequenceLength(sequence) != 0) {
                if (SequenceInput(sequence).transform(
                        [](const auto input) { return input.key; }) == MenuKey::Cancel) {
                    for (const auto &position_to_erase : positions) {
                        pending_.erase(position_to_erase);
                    }
                    return true;
                }
                positions.clear();
            }
        }
        return false;
    }

    [[nodiscard]] auto ReceiveReport(const detail::TerminalReport report,
        const std::chrono::steady_clock::time_point deadline)
        -> std::optional<std::array<int, 3>> {
        auto reader = detail::ReportReader<std::list<INPUT_RECORD>::iterator>{report};
        for (;;) {
            // Queries preserve other input for ReadInput. Ctrl+C instead ends
            // the query immediately; consuming that event here prevents the
            // same cancellation from being delivered again to a later menu.
            if (ConsumeCancellation()) {
                throw InputCancelled{};
            }
            if (!ReceiveUntil(deadline)) {
                return std::nullopt;
            }
            const auto position = std::prev(pending_.end());
            const auto &record = *position;
            if ((record.EventType != KEY_EVENT) ||
                !record.Event.KeyEvent.bKeyDown ||
                (record.Event.KeyEvent.wRepeatCount != 1) ||
                (record.Event.KeyEvent.uChar.UnicodeChar == L'\0')) {
                continue;
            }
            if (const auto values = reader.Push(record.Event.KeyEvent.uChar.UnicodeChar, position)) {
                for (const auto &position_to_erase : reader.Positions()) {
                    pending_.erase(position_to_erase);
                }
                return values;
            }
        }
    }

    // Restoring console modes requires open handles. Members are destroyed in
    // reverse declaration order, so the handles precede their mode guards here.
    wil::unique_hfile input_ = detail::OpenConsole("CONIN$"_zv);
    wil::unique_hfile output_ = detail::OpenConsole("CONOUT$"_zv);
    // Detection initially needs VT input because Console Host otherwise
    // discards extension replies. `ReadTextInput` disables VT input when
    // the terminal has neither extension.
    // https://github.com/microsoft/terminal/blob/main/src/terminal/parser/InputStateMachineEngine.cpp
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) input_mode_ =
        detail::SetConsoleModeScoped(input_.get(),
            ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT,
            ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) output_mode_ =
        detail::SetConsoleModeScoped(output_.get(),
            ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                ENABLE_VIRTUAL_TERMINAL_PROCESSING,
            0);
    std::list<INPUT_RECORD> pending_;
    std::optional<wchar_t> high_surrogate_;
    std::string_view keyboard_restore_;
    std::optional<std::chrono::steady_clock::time_point> keyboard_detection_deadline_;
};

}
