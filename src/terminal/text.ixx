// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

export module devicefs.terminal.text;

import std;

using namespace std::string_view_literals;

namespace devicefs::terminal {

// `VtFilter` removes terminal commands from text, including commands that move
// the cursor, change colors, or set the window title. The filter recognizes the
// command boundaries and discards both the instructions and their payloads.
// `Remove` filters a byte buffer in place. `Preserve` accepts one decoded Unicode
// code point at a time so the caller can decide how to represent retained text.
//
// The filter retains unfinished commands between calls. A log reader reuses
// one instance across its reads, while each label gets a separate instance;
// otherwise an unfinished command in one label could hide later entries.
// Command parsing is based on the supervisor's log filter in
// `src/modules/supervisor/logging_console.ixx`.
export class VtFilter {
public:
    // `Remove` strips ESC-prefixed terminal commands and their payloads from
    // `input`. Retained bytes are compacted in place, and the returned view covers
    // that prefix of the caller's buffer. An unfinished command is remembered
    // for the next call, allowing a command to span several input buffers.
    //
    // Ordinary text retains tabs and line feeds, but loses carriage returns
    // and other ASCII controls. This suits captured logs: CRLF becomes LF, and
    // carriage-return progress updates append instead of overwriting earlier
    // output. Non-ASCII bytes outside commands pass through unchanged. This byte
    // interface does not decode Unicode control characters; label preparation
    // uses the decoded-code-point `Preserve` interface below.
    [[nodiscard]] auto Remove(const std::span<char8_t> input) noexcept {
        auto output_size = 0uz;
        for (const auto character : input) {
            if (Preserve<false>(+character)) {
                input[output_size++] = character;
            }
        }
        return std::u8string_view{input.data(), output_size};
    }

    // `Preserve` processes one decoded code point and returns whether the caller
    // should retain it. False identifies part of a terminal command to discard.
    // True includes standalone controls such as tabs; `PrepareTerminalText`
    // converts those controls to visible notation before displaying a label.
    // Decoded input distinguishes U+009B, a command introducer, from the byte
    // 0x9B occurring within an ordinary UTF-8 character.
    [[nodiscard]] auto Preserve(const char32_t character) noexcept -> bool {
        return Preserve<true>(character);
    }

private:
    enum class State {
        Text,
        Escape,
        EscapeIntermediate,
        ControlSequence,
        ControlSequenceIntermediate,
        OscString,
        StString,
    };

    // This parser tracks whether each input value belongs to ordinary text or
    // to a terminal command. A command can include parameters or a string
    // payload, so recognizing its end is necessary to discard the entire command
    // and resume retaining text afterward. ECMA-48 defines those boundaries;
    // xterm also permits BEL to end an Operating System Command (OSC) string,
    // as used for title and hyperlink commands.
    // https://ecma-international.org/publications-and-standards/standards/ecma-48/
    // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
    template <bool ForDisplay>
    [[nodiscard]] auto Preserve(const char32_t character) noexcept -> bool {
        if ((state_ == State::OscString) || (state_ == State::StString)) {
            if (IsSequenceCancellation(character) ||
                ((state_ == State::OscString) && (character == U'\x07')) ||
                (ForDisplay && (character == U'\x9c'))) {
                state_ = State::Text;
            } else if (character == U'\x1b') {
                state_ = State::Escape;
            }
            return false;
        }
        if (character == U'\x1b') {
            state_ = State::Escape;
            return false;
        }
        if (IsSequenceCancellation(character)) {
            const auto was_text = state_ == State::Text;
            state_ = State::Text;
            return ForDisplay && was_text;
        }
        if (IsControl(character)) {
            if constexpr (ForDisplay) {
                return state_ == State::Text;
            } else {
                return (character == U'\t') || (character == U'\n');
            }
        }
        if constexpr (ForDisplay) {
            switch (character) {
            case U'\x9b':
                state_ = State::ControlSequence;
                return false;
            case U'\x9d':
                state_ = State::OscString;
                return false;
            case U'\x90':
            case U'\x98':
            case U'\x9e':
            case U'\x9f':
                state_ = State::StString;
                return false;
            default:
                if ((character >= U'\x80') && (character <= U'\x9f')) {
                    return state_ == State::Text;
                }
            }
        }

        switch (state_) {
        case State::Text:
            return true;

        case State::Escape:
            switch (character) {
            case U'[':
                state_ = State::ControlSequence;
                break;
            case U']':
                state_ = State::OscString;
                break;
            case U'P':
            case U'X':
            case U'^':
            case U'_':
                state_ = State::StString;
                break;
            default:
                if (character <= U'/') {
                    state_ = State::EscapeIntermediate;
                } else if (character <= U'~') {
                    state_ = State::Text;
                } else {
                    state_ = State::Text;
                    return true;
                }
            }
            return false;

        case State::EscapeIntermediate:
            if (character <= U'/') {
                return false;
            }
            state_ = State::Text;
            return character > U'~';

        case State::ControlSequence:
            if (character >= U'@') {
                state_ = State::Text;
                return character > U'~';
            }
            if constexpr (ForDisplay) {
                if (character <= U'/') {
                    state_ = State::ControlSequenceIntermediate;
                }
            }
            return false;

        case State::ControlSequenceIntermediate:
            if (character <= U'/') {
                return false;
            }
            state_ = State::Text;
            return (character < U'@') || (character > U'~');

        default:
            std::unreachable();
        }
    }

    [[nodiscard]] static constexpr auto IsControl(
        const char32_t character) noexcept -> bool {
        return (character < U' ') || (character == U'\x7f');
    }

    [[nodiscard]] static constexpr auto IsSequenceCancellation(
        const char32_t character) noexcept -> bool {
        return (character == U'\x18') || (character == U'\x1a');
    }

    State state_ = State::Text;
};

namespace {

auto AppendDisplayCharacter(std::string &output,
    const std::uint_least32_t character, const std::string_view bytes) -> void {
    switch (character) {
    case U'\t':
        output.append("\\t"sv);
        return;
    case U'\n':
        output.append("\\n"sv);
        return;
    case U'\r':
        output.append("\\r"sv);
        return;
    case U'\\':
        output.append("\\\\"sv);
        return;
    default:
        break;
    }
    if ((character < U' ') || (character == U'\x7f')) {
        output.append(std::format("\\x{:02X}", character));
        return;
    }
    // C1 controls and Unicode line, paragraph, and directional controls become
    // visible code-point numbers so they cannot change the label's layout.
    // `PrepareTerminalText` documents how this policy affects legitimate names.
    if (((character >= U'\x80') && (character <= U'\x9f')) ||
        (character == U'\u061c') ||
        ((character >= U'\u200e') && (character <= U'\u200f')) ||
        ((character >= U'\u2028') && (character <= U'\u202e')) ||
        ((character >= U'\u2066') && (character <= U'\u2069'))) {
        output.append(std::format("\\u{{{:04X}}}", character));
        return;
    }
    output.append(bytes);
}

}

// `PrepareTerminalText` prepares a supplied label for display inside an
// interface. Embedded terminal commands could move the cursor or change the
// display, so the operation removes those commands and represents selected
// controls as readable ASCII notation. Outside removed commands, tabs, line
// feeds, and carriage returns become \t, \n, and \r. Other C0 controls and DEL
// become \xHH; C1 controls, Unicode line and paragraph separators, and
// directional controls become \u{XXXX}. Literal backslashes are doubled so a
// visible \n can be distinguished from an original backslash followed by n.
// Malformed UTF-8 outside removed commands is shown as one \xHH per byte.
//
// This policy can garble legitimate filenames. It replaces all twelve Unicode
// directional controls, including the marks and isolates used to arrange
// mixed-direction text correctly. For example, a Hebrew or Arabic filename
// containing a Latin product name, a number, or parentheses can rely on those
// controls. Replacing them with visible notation adds text and can change the
// intended order of the name's parts. Retaining the letters' original bytes
// therefore does not preserve the filename's intended presentation. This is a
// limitation of the filtering policy, not a Unicode requirement;
// UAX #9 describes the controls' legitimate role in filenames and labels.
// https://www.unicode.org/reports/tr9/#Directional_Formatting_Characters
//
// The caller selects a UTF-8 LC_CTYPE locale before invoking this operation,
// since the application's locale determines how `std::mbrtoc32` interprets bytes.
export [[nodiscard]] auto PrepareTerminalText(std::string_view input)
    -> std::string {
    auto filter = VtFilter{};
    auto conversion = std::mbstate_t{};
    auto output = std::string{};
    output.reserve(input.size());
    while (!input.empty()) {
        auto character = char32_t{};
        const auto length = std::mbrtoc32(
            &character, input.data(), input.size(), &conversion);
        // Malformed UTF-8 is displayed byte by byte as \xHH, except inside a
        // removed command. `mbrtoc32` reports invalid or incomplete input as -1
        // or -2 represented as size_t; both exceed the supplied input length.
        // Each byte of malformed input is processed independently, so decoding
        // the next byte starts with a fresh conversion state.
        // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbrtoc16-mbrtoc323
        if (length > input.size()) {
            conversion = {};
            // A command's string payload is discarded even when its bytes are
            // malformed. Passing U+FFFD through the command parser determines
            // whether the invalid byte belongs to such a payload. U+FFFD serves
            // only that classification: outside a command, the invalid byte's
            // numeric value is appended to the output.
            if (filter.Preserve(U'\ufffd')) {
                output.append(std::format("\\x{:02X}",
                    std::bit_cast<unsigned char>(input.front())));
            }
            input.remove_prefix(1);
            continue;
        }
        // Consuming a NUL must advance the input by one byte. The decoding API
        // returns zero for NUL, so its return value needs that adjustment.
        const auto bytes = input.substr(0, length == 0 ? 1 : length);
        if (filter.Preserve(character)) {
            AppendDisplayCharacter(output, character, bytes);
        }
        input.remove_prefix(bytes.size());
    }
    return output;
}

}
