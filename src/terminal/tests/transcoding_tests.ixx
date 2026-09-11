// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "../compat/gsl_suppress.h"

export module devicefs.terminal.transcoding_tests;

import std;
import devicefs.terminal.transcoding;
import devicefs.terminal.test_support;

using namespace std::string_view_literals;
using namespace devicefs::terminal;
using namespace devicefs::terminal::tests;

namespace {

template <typename Output, typename Input>
auto CheckConversion(const std::basic_string_view<Input> input,
    const std::basic_string_view<Output> expected) -> void {
    const auto converted = Transcode<Output>(input);
    Require(std::basic_string_view<Output>{converted} == expected,
        "transcoding changed Unicode text or its code-unit count"sv);
    Require(converted.data()[converted.size()] == Output{},
        "transcoded result has no final NUL"sv);
}

template <typename Output, typename Input>
auto CheckRejected(const std::basic_string_view<Input> input) -> void {
    try {
        const auto converted = Transcode<Output>(input);
        throw std::runtime_error(std::format(
            "malformed input produced {} output code units", converted.size()));
    } catch (const std::invalid_argument &error) {
        Require(std::string_view{error.what()}.contains("offset"sv),
            "transcoding error did not identify the input position"sv);
    }
}

}

export auto TestTranscoding() -> bool {
    auto passed = Test("UTF-8, UTF-16 and UTF-32 conversions preserving Unicode and embedded NULs"sv, [] {
        constexpr auto utf8 = "ASCII café 日本語 é 👩‍💻 ©️\0\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"sv;
        constexpr auto utf16 = u"ASCII café 日本語 é 👩‍💻 ©️\0\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"sv;
        constexpr auto utf32 = U"ASCII café 日本語 é 👩‍💻 ©️\0\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"sv;
        CheckConversion(utf8, utf16);
        CheckConversion(utf8, utf32);
        CheckConversion(utf16, utf8);
        CheckConversion(utf16, utf32);
        CheckConversion(utf32, utf8);
        CheckConversion(utf32, utf16);
        CheckConversion(utf8, utf8);
        CheckConversion(utf16, utf16);
        CheckConversion(utf32, utf32);
        CheckConversion(u8"café 👩‍💻"sv, u"café 👩‍💻"sv);
        CheckConversion(u"café 👩‍💻"sv, u8"café 👩‍💻"sv);
        CheckConversion(u8"café 👩‍💻"sv, "café 👩‍💻"sv);
    });
    passed &= Test("transcoding empty views, strings, literals and pointers"sv, [] {
        CheckConversion(std::string_view{}, std::u16string_view{});
        CheckConversion(std::u16string_view{}, std::string_view{});
        CheckConversion(std::u32string_view{}, std::u16string_view{});
        const auto from_string = Transcode<char16_t>(std::string{"hello"});
        const auto from_literal = Transcode<char16_t>("hello");
        const auto pointer = "hello";
        const auto from_pointer = Transcode<char16_t>(pointer);
        Require(std::u16string_view{from_string} == u"hello"sv &&
            std::u16string_view{from_literal} == u"hello"sv &&
            std::u16string_view{from_pointer} == u"hello"sv,
            "a supported string input was converted incorrectly"sv);
    });
    passed &= Test("native wide text preserving Unicode and embedded NULs in both directions"sv, [] {
        constexpr auto utf8 = "café 日本語 👩‍💻\0\U00010000\U0010ffff"sv;
        constexpr auto wide = L"café 日本語 👩‍💻\0\U00010000\U0010ffff"sv;
        CheckConversion(utf8, wide);
        CheckConversion(wide, utf8);
        CheckConversion(wide, wide);
        CheckConversion(wide, u"café 日本語 👩‍💻\0\U00010000\U0010ffff"sv);
        CheckConversion(U"café 日本語 👩‍💻\0\U00010000\U0010ffff"sv, wide);
        CheckConversion(std::wstring_view{}, std::string_view{});
        CheckRejected<char>(L"\xd800"sv);
        CheckRejected<wchar_t>("\xed\xa0\x80"sv);
    });
    passed &= Test("transcoding directly into requested string and allocator types"sv, [] {
        const auto wide = Transcode<std::wstring>("café\0👩‍💻"sv);
        Require(wide == L"café\0👩‍💻"sv,
            "requested wide string lost Unicode or embedded NULs"sv);
        Require(Transcode<std::string>(wide) == "café\0👩‍💻"sv,
            "requested narrow string changed the text"sv);
        const auto allocated = Transcode<std::pmr::u8string>(wide);
        static_assert(std::same_as<decltype(allocated), const std::pmr::u8string>);
        Require(allocated == u8"café\0👩‍💻"sv,
            "conversion into the requested allocator changed the text"sv);
        for (const auto length : {0uz, 1uz, 255uz, 256uz, 4096uz}) {
            const auto input = std::wstring(length, L'界');
            const auto encoded = Transcode<std::pmr::u8string>(input);
            Require(Transcode<std::wstring>(encoded) == input,
                "requested string round trip failed across a storage boundary"sv);
        }
        try {
            const auto invalid = Transcode<std::wstring>("\xf4\x90\x80\x80"sv);
            throw std::runtime_error(std::format(
                "malformed UTF-8 produced {} wide code units", invalid.size()));
        } catch (const std::invalid_argument &) {
        }
    });
    passed &= Test("transcoding and moving results around the inline storage boundary"sv, [] {
        for (const auto length : {0uz, 1uz, 254uz, 255uz, 256uz, 257uz, 4096uz}) {
            const auto input = std::string(length, 'x');
            auto converted = Transcode<char16_t>(input);
            const auto moved = std::move(converted);
            Require(std::u16string_view{moved} == std::u16string(length, u'x'),
                "moving a transcoded result changed its contents"sv);
            Require(moved.data()[moved.size()] == u'\0', "moving lost the terminator"sv);
            GSL_SUPPRESS("26800", "TranscodedText promises an empty, null-terminated moved-from result.")
            {
                Require(converted.size() == 0 && converted.data()[0] == u'\0',
                    "moved-from result is not an empty string"sv);
            }
            const auto restored = Transcode<char>(std::u16string_view{moved});
            Require(std::string_view{restored} == input, "round-trip conversion changed text"sv);
        }
    });
    passed &= Test("transcoding long composed text across SIMD block boundaries"sv, [] {
        auto utf8 = std::string{};
        auto utf16 = std::u16string{};
        auto utf32 = std::u32string{};
        auto wide = std::wstring{};
        for (auto repetitions = 0; repetitions < 100; ++repetitions) {
            utf8 += "a日本語👩‍💻é";
            utf16 += u"a日本語👩‍💻é";
            utf32 += U"a日本語👩‍💻é";
            wide += L"a日本語👩‍💻é";
            CheckConversion(std::string_view{utf8}, std::u16string_view{utf16});
            CheckConversion(std::u16string_view{utf16}, std::string_view{utf8});
            CheckConversion(std::string_view{utf8}, std::u32string_view{utf32});
            CheckConversion(std::u32string_view{utf32}, std::string_view{utf8});
            CheckConversion(std::wstring_view{wide}, std::string_view{utf8});
            CheckConversion(std::string_view{utf8}, std::wstring_view{wide});
        }
    });
    passed &= Test("transcoding rejecting malformed UTF-8, UTF-16 and UTF-32"sv, [] {
        for (const auto invalid : {"\x80"sv, "\xc0\xaf"sv, "\xe2\x82"sv,
                "\xed\xa0\x80"sv, "\xf4\x90\x80\x80"sv, "\xf8\x88\x80\x80\x80"sv}) {
            CheckRejected<char16_t>(invalid);
            CheckRejected<char32_t>(invalid);
            CheckRejected<char>(invalid);
            const auto long_input = std::string(300, 'a') + std::string{invalid};
            CheckRejected<char16_t>(std::string_view{long_input});
        }
        for (const auto invalid : {u"\xd800"sv, u"\xdc00"sv, u"\xd800x"sv}) {
            CheckRejected<char>(invalid);
            CheckRejected<char32_t>(invalid);
            CheckRejected<char16_t>(invalid);
        }
        for (const auto invalid : {U"\xd800"sv, U"\x110000"sv, U"\xffffffff"sv}) {
            CheckRejected<char>(invalid);
            CheckRejected<char16_t>(invalid);
            CheckRejected<char32_t>(invalid);
        }
    });
    passed &= Test("native wide input rejecting invalid code units in scalar and SIMD conversions"sv, []<typename Wide = wchar_t> {
        for (const auto length : {0uz, 1uz, 15uz, 16uz, 31uz, 32uz, 255uz, 256uz}) {
            auto text = std::basic_string<Wide>(length, L'a');
            text += Wide{0xd800};
            CheckRejected<char>(std::basic_string_view{text});
            if constexpr (std::numeric_limits<Wide>::is_signed) {
                text.back() = Wide{-1};
                CheckRejected<char>(std::basic_string_view{text});
                CheckRejected<char16_t>(std::basic_string_view{text});
                CheckRejected<Wide>(std::basic_string_view{text});
            }
        }
    });
    return passed;
}
