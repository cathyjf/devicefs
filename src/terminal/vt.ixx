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

// Seven-bit prefixes for control sequences and application-mode key reports.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
constexpr auto kEscape = '\x1b';
constexpr auto kCsiPrefix = "\x1b["sv;
constexpr auto kSs3Prefix = "\x1bO"sv;

// Apple Terminal's default "Shift Return sends Meta Return" binding sends
// ESC followed by CR. Recognizing that pair permits Shift+Return to insert a
// newline even though Apple Terminal supports neither keyboard extension.
// A separately pressed Escape followed quickly by Return sends the same bytes.
// https://github.com/anomalyco/opencode/issues/43286
constexpr auto kMetaReturn = "\x1b\r"sv;

// Xterm's modified-key reports start with `CSI 27;` and end with `~`.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Alt-and-Meta-Keys
constexpr auto kModifiedKeyPrefix = "\x1b[27;"sv;

// Kitty and xterm assign different modifier bits to Meta. Their Shift, Alt,
// and Control bits agree, so only Meta needs a different value when an xterm
// report is decoded through the shared keyboard decoder.
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
constexpr auto kKittyMetaModifier = 32u;
constexpr auto kXtermMetaModifier = 8u;

// Express an xterm modified-key report in CSI-u order so that the keyboard
// decoder can parse both forms. The numeric fields retain their original text.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Alt-and-Meta-Keys
[[nodiscard]] auto CsiKeyReport(const std::string_view key, const std::string_view modifiers) {
    return std::format("\x1b[{};{}u", key, modifiers);
}

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

// Kitty keyboard protocol: `CSI > 1 u` saves the current keyboard mode and
// selects flag 1, "Disambiguate escape codes". Escape and modified keys then
// arrive as complete key reports, allowing Escape to be recognized without a
// timeout. Push after entering the alternate screen, whose keyboard-mode stack
// is separate from the main screen.
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#progressive-enhancement
constexpr auto kPushDisambiguatedKeys = "\x1b[>1u"sv;

// Kitty keyboard protocol: `CSI < u` pops one saved keyboard mode and restores
// it. Send this before leaving the screen on which the mode was pushed.
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#quickstart
constexpr auto kPopKeyboardMode = "\x1b[<u"sv;

// Kitty replies to `CSI ? u` with `CSI ? flags u`. A reply with zero flags
// still establishes support: the protocol is available but currently disabled.
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#detection-of-support-for-this-protocol
constexpr auto kRequestKeyboardFlags = "\x1b[?u"sv;
constexpr auto kKeyboardFlagsReplyPrefix = "\x1b[?"sv;

// XTQMODKEYS asks for the current `modifyOtherKeys` setting. The reply is
// `CSI > 4 ; value m`, including when the current setting is zero.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
constexpr auto kRequestModifiedKeys = "\x1b[?4m"sv;
constexpr auto kModifiedKeysReplyPrefix = "\x1b[>4;"sv;

constexpr auto kRequestKeyboardSupport = Concatenate<kRequestKeyboardFlags,
    kRequestModifiedKeys>();

// Xterm's `modifyOtherKeys` level 2 reports modified ordinary keys, including
// Shift+Enter, on terminals that support this older protocol. Omitting the
// value on exit restores xterm's initial setting.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Alt-and-Meta-Keys
constexpr auto kEnableModifiedKeys = "\x1b[>4;2m"sv;
constexpr auto kResetModifiedKeys = "\x1b[>4m"sv;

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
