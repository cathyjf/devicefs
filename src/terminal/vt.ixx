// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.vt;

import std;

using namespace std::string_view_literals;

namespace devicefs::terminal::vt::detail {

template <const std::string_view &... Parts>
constexpr auto kConcatenated = [] {
    auto result = std::array<char, (0uz + ... + Parts.size())>{};
    auto output = result.begin();
    ((output = std::ranges::copy(Parts, output).out), ...);
    return result;
}();

}

// These commands encode terminal operations as text for a console or output
// buffer. A caller can combine several commands before submitting a write.
export namespace devicefs::terminal::vt {

// `Concatenate` joins constant strings at compile time. The returned view
// refers to an array with static storage duration, so it can itself be used
// as a constant command wherever the combined operations are needed.
template <const std::string_view &... Parts>
[[nodiscard]] consteval auto Concatenate() noexcept {
    return std::string_view{detail::kConcatenated<Parts...>};
}

// CUP (`CSI row;column H`) positions the cursor using one-based coordinates.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
[[nodiscard]] auto MoveCursor(const int row, const int column) -> std::string {
    return std::format("\x1b[{};{}H", row, column);
}

// CUP with omitted parameters moves the cursor to row 1, column 1.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
constexpr auto kMoveCursorHome = "\x1b[H"sv;

// ICH (`CSI count @`) inserts spaces at the cursor, shifting the following
// characters right and discarding characters beyond the row's right edge.
// The cursor stays in place.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification
[[nodiscard]] auto InsertCharacters(const int count) -> std::string {
    return std::format("\x1b[{}@", count);
}

// ECH (`CSI count X`) replaces cells with spaces without moving the cursor.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification
[[nodiscard]] auto EraseCells(const int count) -> std::string {
    return std::format("\x1b[{}X", count);
}

// EL (`CSI K`, default parameter 0) erases from the cursor through the row's end.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification
constexpr auto kEraseToEndOfLine = "\x1b[K"sv;

// ED 2 erases the entire display without moving the cursor.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification
constexpr auto kEraseDisplay = "\x1b[2J"sv;

// SGR 0 resets text attributes, including reverse video.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-formatting
constexpr auto kResetAttributes = "\x1b[0m"sv;

// SGR 7 swaps foreground and background for selection highlighting.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-formatting
constexpr auto kReverseVideo = "\x1b[7m"sv;

// Clear the display with normal attributes and position the cursor at its top
// left corner, ready for a new frame or a recovery message.
constexpr auto kClearScreen = Concatenate<kResetAttributes, kEraseDisplay, kMoveCursorHome>();

// DECSET 1049 saves the cursor and enters a cleared alternate screen.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-The-Alternate-Screen-Buffer
constexpr auto kEnterAlternateScreen = "\x1b[?1049h"sv;

// DECRST 1049 returns to the normal screen and restores the cursor saved when
// the alternate screen was entered.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-The-Alternate-Screen-Buffer
constexpr auto kLeaveAlternateScreen = "\x1b[?1049l"sv;

// DECRST 25 (DECTCEM) hides the cursor.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-visibility
constexpr auto kHideCursor = "\x1b[?25l"sv;

// DECSET 25 (DECTCEM) shows the cursor.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-visibility
constexpr auto kShowCursor = "\x1b[?25h"sv;

// XTSAVE (`CSI ? 25 s`) saves the cursor-visibility setting for restoration.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
constexpr auto kSaveCursorVisibility = "\x1b[?25s"sv;

// XTRESTORE (`CSI ? 25 r`) restores the saved cursor-visibility setting.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
constexpr auto kRestoreCursorVisibility = "\x1b[?25r"sv;

// BSU (`CSI ? 2026 h`) begins a synchronized update: the terminal processes
// output while keeping the previous rendered frame visible.
// https://github.com/contour-terminal/vt-extensions/blob/master/synchronized-output.md
constexpr auto kBeginSynchronizedUpdate = "\x1b[?2026h"sv;

// ESU (`CSI ? 2026 l`) ends the synchronized update, allowing the terminal
// to display the accumulated changes.
// https://github.com/contour-terminal/vt-extensions/blob/master/synchronized-output.md
constexpr auto kEndSynchronizedUpdate = "\x1b[?2026l"sv;

// DSR 6 requests the cursor position, answered by `CSI row;column R` with
// one-based cell coordinates.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
constexpr auto kRequestCursorPosition = "\x1b[6n"sv;

// XTWINOPS 18 requests text-area dimensions in cells. The terminal replies
// with `CSI 8;rows;columns t`.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
constexpr auto kRequestTextAreaSize = "\x1b[18t"sv;

}
