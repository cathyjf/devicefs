// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.windows;

import std;
import <windows.h>;
import <wil/resource.h>;
import <wil/stl.h>;
import devicefs.terminal;
import devicefs.terminal.base_console;
import devicefs.terminal.menu;
import devicefs.terminal.safecast;
import devicefs.terminal.reports;

using namespace std::string_view_literals;
using namespace wil::literals;

namespace devicefs::terminal::detail {

[[nodiscard]] auto SetConsoleModeScoped(
    const HANDLE handle, const DWORD enable, const DWORD disable) {
    auto previous = DWORD{};
    if (!GetConsoleMode(handle, &previous)) {
        throw std::system_error(std::bit_cast<int>(GetLastError()),
            std::system_category(), "could not read the terminal's console mode");
    }
    auto restore = wil::scope_exit([handle, previous] {
        std::ignore = SetConsoleMode(handle, previous);
    });
    if (!SetConsoleMode(handle, (previous | enable) & ~disable)) {
        throw std::system_error(std::bit_cast<int>(GetLastError()),
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
        const auto wide = std::filesystem::path{text}.wstring();
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
            throw std::system_error(std::bit_cast<int>(GetLastError()),
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

    [[nodiscard]] auto ReadMenuInput() -> MenuInput {
        for (;;) {
            const auto record = ReadInput();
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                return {.key = MenuKey::Resize};
            }
            if ((record.EventType != KEY_EVENT) || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto &key = record.Event.KeyEvent;
            if (key.uChar.UnicodeChar == L'\x03') {
                return {.key = MenuKey::Cancel};
            }
            // A reply arriving after its query timed out reaches this input
            // loop. Its characters describe a VT command, not menu shortcuts;
            // for example, the 1 in a cursor report must not open a full name.
            // Filter character-only events while preserving physical key events
            // and their virtual-key information. Ctrl+C above also works during
            // an unfinished sequence.
            if ((key.wVirtualKeyCode == 0) && !unclaimed_sequences_.Preserve(key.uChar.UnicodeChar)) {
                continue;
            }
            const auto action = [&key]() -> std::optional<MenuKey> {
                switch (key.wVirtualKeyCode) {
                case VK_UP:
                    return MenuKey::Up;
                case VK_DOWN:
                    return MenuKey::Down;
                case VK_PRIOR:
                    return MenuKey::PageUp;
                case VK_NEXT:
                    return MenuKey::PageDown;
                case VK_HOME:
                    return MenuKey::Home;
                case VK_END:
                    return MenuKey::End;
                case VK_RETURN:
                    return MenuKey::Accept;
                case VK_ESCAPE:
                    return MenuKey::Back;
                default:
                    break;
                }
                return CharacterMenuKey(key.uChar.UnicodeChar);
            }();
            if (action) {
                return {.key = *action, .repeat = key.wRepeatCount};
            }
        }
    }

private:
    friend class BaseConsole;
    static constexpr auto kEnterMenu = "\x1b[?1049h\x1b[?25l"sv;

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
            throw std::system_error(std::bit_cast<int>(GetLastError()),
                std::system_category(), "could not read the console cursor visibility");
        }
        return wil::scope_exit([this, previous_cursor] {
            WriteControlSequenceNoThrow(kLeaveMenu);
            std::ignore = SetConsoleCursorInfo(output_.get(), &previous_cursor);
        });
    }

    [[nodiscard]] auto ReadConsoleRecord() const -> INPUT_RECORD {
        auto record = INPUT_RECORD{};
        auto read = DWORD{};
        if (!ReadConsoleInputW(input_.get(), &record, 1, &read)) {
            throw std::system_error(std::bit_cast<int>(GetLastError()),
                std::system_category(), "could not read terminal input");
        }
        if (read != 1) {
            throw std::runtime_error("the terminal returned no input record");
        }
        return record;
    }

    [[nodiscard]] auto ReceiveReport(const detail::TerminalReport report,
        const std::chrono::steady_clock::time_point deadline)
        -> std::optional<std::array<int, 3>> {
        auto reader = detail::ReportReader<std::list<INPUT_RECORD>::iterator>{report};
        for (;;) {
            // Queries preserve other input for ReadInput. Ctrl+C instead ends
            // the query immediately; consuming that event here prevents the
            // same cancellation from being delivered again to a later menu.
            const auto cancellation = std::ranges::find_if(pending_, [](const auto &record) {
                return (record.EventType == KEY_EVENT) && record.Event.KeyEvent.bKeyDown &&
                    (record.Event.KeyEvent.uChar.UnicodeChar == L'\x03');
            });
            if (cancellation != pending_.end()) {
                pending_.erase(cancellation);
                throw InputCancelled{};
            }
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= remaining.zero()) {
                return std::nullopt;
            }
            // The query's deadline bounds this wait to a few seconds, so its
            // positive, rounded millisecond count fits the Windows parameter.
            const auto wait = WaitForSingleObject(input_.get(),
                std::chrono::ceil<std::chrono::duration<DWORD, std::milli>>(
                    remaining).count());
            if (wait == WAIT_TIMEOUT) {
                continue;
            }
            if (wait == WAIT_FAILED) {
                throw std::system_error(std::bit_cast<int>(GetLastError()),
                    std::system_category(), "could not wait for a terminal reply");
            }
            const auto position = pending_.insert(pending_.end(), ReadConsoleRecord());
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
    // Keyboard input is delivered as structured events, including arrow keys,
    // by leaving `ENABLE_VIRTUAL_TERMINAL_INPUT` disabled. Microsoft documents
    // that terminal query replies enter the input buffer in either mode, so the
    // same input handle also receives the cursor and size reports used here.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) input_mode_ =
        detail::SetConsoleModeScoped(input_.get(), ENABLE_WINDOW_INPUT,
            ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                ENABLE_VIRTUAL_TERMINAL_INPUT);
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) output_mode_ =
        detail::SetConsoleModeScoped(output_.get(),
            ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                ENABLE_VIRTUAL_TERMINAL_PROCESSING,
            0);
    std::list<INPUT_RECORD> pending_;
    VtFilter unclaimed_sequences_;
};

}
