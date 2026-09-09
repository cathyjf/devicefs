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

[[nodiscard, msvc::forceinline]]
constexpr auto TransformDecodingResult(
    const std::size_t result,
    [[maybe_unused]] const std::size_t input_size,
    [[maybe_unused]] const char32_t &output) {
    // Microsoft's UCRT rejects surrogate values and values above U+10FFFF
    // before reporting a successful conversion, so its result already meets
    // the scalar-value requirement for label preparation and width measurement.
    // The checks are in `__mbrtoc32_utf8`, in the Windows SDK source file
    // `ucrt/convert/mbrtoc32.cpp`; the documented contract requires valid UTF-8:
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mbrtoc16-mbrtoc323
    //
    // Therefore, with MSVC++, the result of `std::mbrtoc32` can be directly
    // relied upon. The same analysis does not apply to glibc. The explanation
    // follows.
    //
    // UTF-8 encodes Unicode scalar values. U+10FFFF is the greatest Unicode
    // code point. The lower bound `0xd800` starts the high-surrogate range
    // U+D800..U+DBFF, and `0xdfff` ends the adjacent low-surrogate range
    // U+DC00..U+DFFF. Those ranges reserve values for the two halves of UTF-16
    // surrogate pairs. The individual surrogate values are excluded from Unicode
    // scalar values, so none of U+D800..U+DFFF can be encoded in valid UTF-8.
    // Unicode section 3.8 defines the surrogate ranges in D71 and D73; section 3.9
    // defines scalar values in D76 and permitted UTF-8 encodings in D92 and Table 3-7:
    // https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/
    //
    // The UTF-8 specification changed: RFC 2279 (1998) allowed the 31-bit UCS-4
    // range through 0x7FFFFFFF, using up to six bytes per value. RFC 3629 (2003)
    // restricted UTF-8 to Unicode's U+10FFFF limit and made Unicode the normative
    // authority for the encoding. Section 12 explicitly records that change:
    // https://www.rfc-editor.org/rfc/rfc2279.html#section-2
    // https://www.rfc-editor.org/rfc/rfc3629.html#section-12
    //
    // glibc retains support for values from the older, broader range. Without this
    // range check, `PrepareTerminalText` copies bytes that are invalid under the
    // modern definition into its output as ordinary text. `MeasureText` then
    // converts that output to UTF-16, which cannot represent those values. The
    // conversion can throw, causing a malformed label to abort menu rendering.
    // Returning the decoder's invalid-sequence result, `size_t{-1}`, makes
    // `PrepareTerminalText` display the original bytes as `\xHH` instead. GNU's
    // portability documentation records glibc's decoding beyond U+10FFFF:
    // https://www.gnu.org/software/gnulib/manual/html_node/mbrtowc.html
#if !defined(_MSC_VER)
    if ((result <= input_size) &&
        ((output > U'\U0010ffff') || ((output >= 0xd800) && (output <= 0xdfff)))) {
        return -1uz;
    }
#endif
    return result;
}

// `DecodeNextCodePoint` decodes the next UTF-8 character for label preparation
// and width measurement.
export
[[nodiscard, msvc::forceinline]]
constexpr auto DecodeNextCodePoint(
    char32_t &output, const std::string_view input, std::mbstate_t &state) {
#if !defined(__APPLE__)
    return TransformDecodingResult(
        std::mbrtoc32(&output, input.data(), input.size(), &state),
        input.size(), output);
#else
    // macOS lacks `std::mbrtoc32`. However, with the UTF-8 locale required
    // by our callers, the `std::mbrtowc` function decodes each character into
    // a Unicode code point stored in a `wchar_t`. We can therefore use that
    // function and assign the decoded value directly to a `char32_t`.
    //
    // Apple's UTF-8 decoder rejects encodings of surrogate values and values
    // above U+10FFFF before returning a decoded character. A successful
    // `std::mbrtowc` conversion therefore needs no additional scalar-range check:
    // https://github.com/apple-oss-distributions/Libc/blob/main/locale/FreeBSD/utf8.c
    auto wide_char = wchar_t{};
    const auto result = std::mbrtowc(
        &wide_char, input.data(), input.length(), &state);
    if ((result != -1uz) && (result != -2uz)) {
        static_assert(std::ranges::less_equal{}(
            std::numeric_limits<decltype(wide_char)>::max(),
            std::numeric_limits<std::remove_reference_t<decltype(output)>>::max()));
        output = wide_char;
    }
    return result;
#endif
}

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

} // namespace

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
// since the application's locale determines how `DecodeNextCodePoint`
// interprets bytes (except on Windows, where `DecodeNextCodePoint` always
// interprets its input as UTF-8).
export [[nodiscard]] auto PrepareTerminalText(std::string_view input)
    -> std::string {
    auto filter = VtFilter{};
    auto conversion = std::mbstate_t{};
    auto output = std::string{};
    output.reserve(input.size());
    while (!input.empty()) {
        auto character = char32_t{};
        const auto length = DecodeNextCodePoint(character, input, conversion);
        // Malformed UTF-8 is displayed byte by byte as \xHH, except inside a
        // removed command. `DecodeNextCodePoint` reports invalid or incomplete
        // input as -1 or -2 represented as size_t; both exceed the supplied
        // input length. Each byte of malformed input is processed
        // independently, so decoding the next byte starts with a fresh
        // conversion state.
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

} // namespace devicefs::terminal
