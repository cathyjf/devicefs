// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.text_tests;

import std;
import devicefs.terminal;
import devicefs.terminal.text;
import devicefs.terminal.safecast;
import devicefs.terminal.test_support;

using namespace std::string_view_literals;
using namespace devicefs::terminal;
using namespace devicefs::terminal::tests;

namespace {

constexpr auto kSeed = 0x54455854u;
constexpr auto kCaseCount = 2048uz;

// Compare the block-based predicate with a byte-at-a-time reference. Report
// the input and its offset in the test buffer so failures can be reproduced
// with the same bytes and alignment.
auto CheckAscii(const std::string_view text, const std::size_t offset = 0) -> void {
    const auto expected = std::ranges::all_of(text,
        [](const unsigned char byte) { return byte <= 127; });
    if (IsEntirelyAscii(text) != expected) {
        throw std::runtime_error(std::format(
            "ASCII check disagreed with the byte-at-a-time reference; offset {}, length {}, input {:?}",
            offset, text.size(), text));
    }
}

// Combining arbitrary bytes with recognizable fragments exercises transitions
// between prose, malformed UTF-8, and partly received terminal commands. A fixed
// seed makes a failing case reproducible; diagnostics also print its input.
[[nodiscard]] auto GeneratedInputs() {
    constexpr auto fragments = std::array{
        "ordinary text"sv, "日本語 — é — 👩‍💻 — ©️"sv,
        // CAN/SUB cancel commands; ESC starts one. `ESC [` is CSI; `ESC ]` is
        // OSC; `ESC P/X/^/_` introduce DCS/SOS/PM/APC strings. `ESC \` is ST,
        // their terminator; BEL can also terminate OSC.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        "\0\t\n\r\\"sv, "\x18\x1a"sv, "\x1b"sv, "\x1b["sv,
        "\x1b]"sv, "\x1bP"sv, "\x1bX"sv, "\x1b^"sv, "\x1b_"sv,
        // CSI suffixes: SGR 31 selects red, DECRST 25 hides the cursor, and
        // SGR 38:2::1:2:3 selects RGB foreground (1, 2, 3).
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        "\x1b\\"sv, "\a"sv, "31m"sv, "?25l"sv, "38:2::1:2:3m"sv,
        // UTF-8 encodings of the C1 forms of CSI, OSC, ST, and DCS, respectively.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        "\u009b"sv, "\u009d"sv, "\u009c"sv, "\u0090"sv,
        "\u061c\u200e\u202e\u2066\u2069"sv, "\u2028\u2029"sv,
        "\xc0\xaf"sv, "\xed\xa0\x80"sv, "\xf4\x90\x80\x80"sv,
        "\xf0\x9f"sv, "\xff"sv,
    };
    auto random = std::mt19937{kSeed};
    auto inputs = std::vector<std::string>(kCaseCount);
    for (auto &input : inputs) {
        const auto pieces = random() % 32;
        for (auto piece = 0u; piece < pieces; ++piece) {
            if ((random() % 4) == 0) {
                input.push_back(std::bit_cast<char>(FailFastCast<unsigned char>(random() % 256)));
            } else {
                input.append(fragments.at(random() % fragments.size()));
            }
        }
    }
    return inputs;
}

[[nodiscard]] auto FilterChunks(const std::string_view input, const auto &next_length) {
    auto storage = input | std::views::transform([](const char byte) {
        return std::bit_cast<char8_t>(byte);
    }) | std::ranges::to<std::u8string>();
    auto remaining = std::span{storage};
    auto filter = VtFilter{};
    auto output = std::u8string{};
    do {
        Require(filter.Remove(remaining.first(0)).empty(), "an empty chunk produced output"sv);
        const auto length = std::min(remaining.size(), std::invoke(next_length));
        output.append(filter.Remove(remaining.first(length)));
        remaining = remaining.subspan(length);
    } while (!remaining.empty());
    return output;
}

}

export [[nodiscard]] auto TestAsciiPredicate() -> bool {
    static_assert(IsEntirelyAscii(std::string_view{}));
    static_assert(IsEntirelyAscii("Installation"sv));
    static_assert(IsEntirelyAscii("\0\x7f"sv));
    static_assert(!IsEntirelyAscii("\x80"sv));
    static_assert(!IsEntirelyAscii("Installation\xff"sv));

    auto passed = Test("ASCII predicate matching the naive check for every byte value, lengths 0–33, and offsets 0–15"sv, [] {
        CheckAscii(std::string_view{});
        auto storage = std::string(49, '\x80');
        for (auto size = 0uz; size <= 33; ++size) {
            for (auto offset = 0uz; offset < 16; ++offset) {
                std::ranges::fill(storage, '\x80');
                const auto text = std::span{storage}.subspan(offset, size);
                std::ranges::fill(text, 'a');
                const auto view = std::string_view{text.data(), text.size()};
                // Non-ASCII bytes immediately outside the view expose loads
                // that include neighboring bytes in the result.
                CheckAscii(view, offset);
                for (auto index = 0uz; index < size; ++index) {
                    for (auto byte = 0u; byte <= 255; ++byte) {
                        text[index] = std::bit_cast<char>(FailFastCast<unsigned char>(byte));
                        CheckAscii(view, offset);
                    }
                    text[index] = 'a';
                }
            }
        }
    });
    passed &= Test("ASCII predicate matching the naive check for 2048 generated buffers up to 8192 bytes"sv, [] {
        auto random = std::mt19937{kSeed};
        for (auto index = 0uz; index < kCaseCount; ++index) {
            const auto size = random() % 8193uz;
            const auto offset = random() % 16uz;
            auto storage = std::string(size + offset + 1, '\xff');
            const auto text = std::span{storage}.subspan(offset, size);
            const auto view = std::string_view{text.data(), text.size()};
            for (auto &byte : text) {
                byte = FailFastCast<char>(random() % 128);
            }
            CheckAscii(view, offset);
            if (!text.empty()) {
                text[random() % text.size()] = '\x80';
                CheckAscii(view, offset);
            }
            for (auto &byte : text) {
                byte = std::bit_cast<char>(FailFastCast<unsigned char>(random() % 256));
            }
            CheckAscii(view, offset);
        }
    });
    return passed;
}

export [[nodiscard]] auto TestGeneratedText() -> bool {
    const auto inputs = GeneratedInputs();
    auto passed = Test("2048 generated labels containing malformed UTF-8 and terminal commands"sv, [&inputs] {
        for (const auto &[index, input] : std::views::enumerate(inputs)) {
            try {
                const auto prepared = PrepareTerminalText(input);
                // CAN (0x18) cancels an unfinished command so the suffix is display text.
                // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
                Require(PrepareTerminalText(input + "\x18" "VISIBLE_END").ends_with("VISIBLE_END"sv),
                    "sequence cancellation did not restore ordinary label text"sv);
                auto remaining = std::string_view{prepared};
                auto state = std::mbstate_t{};
                while (!remaining.empty()) {
                    auto character = char32_t{};
                    const auto length = DecodeNextCodePoint(character, remaining, state);
                    Require((length > 0) && (length <= remaining.size()),
                        "prepared text contains invalid UTF-8 or a NUL"sv);
                    Require((character <= U'\U0010ffff') &&
                        !((character >= 0xd800) && (character <= 0xdfff)),
                        "prepared text contains a non-scalar Unicode value"sv);
                    Require((character >= U' ') &&
                        !((character >= U'\x7f') && (character <= U'\x9f')),
                        "prepared text contains a raw terminal control"sv);
                    Require((character != U'\u061c') &&
                        !((character >= U'\u200e') && (character <= U'\u200f')) &&
                        !((character >= U'\u2028') && (character <= U'\u202e')) &&
                        !((character >= U'\u2066') && (character <= U'\u2069')),
                        "prepared text contains an unescaped directional or line control"sv);
                    remaining.remove_prefix(length);
                }
            } catch (const std::exception &error) {
                throw std::runtime_error(std::format("case {}, seed {}, input {:?}: {}",
                    index, kSeed, input, error.what()));
            }
        }
    });
    passed &= Test("2048 generated logs retaining the same output across byte and random chunk boundaries"sv,
        [&inputs] {
            auto random = std::mt19937{kSeed};
            for (const auto &[index, input] : std::views::enumerate(inputs)) {
                const auto whole = FilterChunks(input, [&input] { return input.size(); });
                const auto bytes = FilterChunks(input, [] { return 1uz; });
                const auto chunks = FilterChunks(input, [&random] {
                    return CompileTimeCast<std::size_t>(random() % 32) + 1;
                });
                if ((whole != bytes) || (whole != chunks)) {
                    throw std::runtime_error(std::format(
                        "chunking changed log output in case {}, seed {}, input {:?}", index, kSeed, input));
                }
                Require(std::ranges::all_of(whole, [](const char8_t byte) {
                    return (byte == u8'\t') || (byte == u8'\n') ||
                        ((byte >= u8' ') && (byte != u8'\x7f'));
                }), "filtered log contains an unexpected ASCII control"sv);
            }
        });
    return passed;
}
