// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.formatting;

import std;
import devicefs.terminal.text;

export namespace devicefs::terminal {

// `PreparedText` owns formatted text that can be reused in another frame write.
// Its formatter preserves both the visible notation produced by
// `PrepareTerminalText` and deliberate line feeds supplied by the format string.
struct PreparedText {
    std::string text;
};

}

namespace devicefs::terminal::formatting_detail {

struct TextArgument {
    std::string_view text;
};

template <typename T>
using FormatArgument = std::conditional_t<std::convertible_to<T, std::string_view> ||
    std::same_as<std::remove_cvref_t<T>, char>, TextArgument, T>;

template <typename Options, typename... Arguments>
constexpr auto kHasTrailingOptions = [] {
    if constexpr (sizeof...(Arguments) == 0) {
        return false;
    } else {
        using LastArgument = std::tuple_element_t<
            sizeof...(Arguments) - 1, std::tuple<Arguments...>>;
        return std::same_as<std::remove_cvref_t<LastArgument>, Options>;
    }
}();

template <typename Arguments, typename Indices>
struct FormatPrefix;

template <typename Arguments, std::size_t... Index>
struct FormatPrefix<Arguments, std::index_sequence<Index...>> {
    using type = std::format_string<FormatArgument<
        std::tuple_element_t<Index, Arguments>>...>;
};

template <typename T>
[[nodiscard]] auto AdaptArgument(T &&argument) noexcept(
    !std::convertible_to<T, std::string_view> ||
    std::is_nothrow_constructible_v<std::string_view, T>) -> decltype(auto) {
    if constexpr (std::convertible_to<T, std::string_view>) {
        return TextArgument{std::string_view{std::forward<T>(argument)}};
    } else if constexpr (std::same_as<std::remove_cvref_t<T>, char>) {
        return TextArgument{std::string_view{std::addressof(argument), 1}};
    } else {
        return std::forward<T>(argument);
    }
}

}

export template <>
struct std::formatter<devicefs::terminal::formatting_detail::TextArgument>
    : std::formatter<std::string_view> {
    auto format(const devicefs::terminal::formatting_detail::TextArgument argument,
        std::format_context &context) const {
        return std::formatter<std::string_view>::format(
            devicefs::terminal::PrepareTerminalText(argument.text), context);
    }
};

export template <>
struct std::formatter<devicefs::terminal::PreparedText>
    : std::formatter<std::string_view> {
    auto format(const devicefs::terminal::PreparedText &argument,
        std::format_context &context) const {
        return std::formatter<std::string_view>::format(argument.text, context);
    }
};

export namespace devicefs::terminal {

template <typename... Arguments>
using TerminalFormatString = std::format_string<
    formatting_detail::FormatArgument<std::type_identity_t<Arguments>>...>;

template <typename Options, typename... Arguments>
using TerminalFormatStringWithOptions = typename formatting_detail::FormatPrefix<
    std::tuple<Arguments...>,
    std::make_index_sequence<sizeof...(Arguments) -
        formatting_detail::kHasTrailingOptions<Options, Arguments...>>>::type;

template <typename Options>
struct FormattedTerminalText {
    std::string text;
    Options options;
};

// `FormatTerminalText` separates application-authored formatting from supplied
// text. The format string can contain deliberate line feeds; string-like and
// character arguments pass through `PrepareTerminalText` before string formatting
// applies alignment or precision. Other argument types retain their ordinary
// formatters. `PreparedText` allows previously prepared labels and unwritten
// suffixes to be reused without preparing their visible notation a second time.
template <typename... Arguments>
[[nodiscard]] auto FormatTerminalText(const TerminalFormatString<Arguments...> format,
    Arguments &&...arguments) -> std::string {
    return std::format(format,
        formatting_detail::AdaptArgument(std::forward<Arguments>(arguments))...);
}

// Frame writes can accept their own options object after the formatting
// arguments. `FormatTerminalTextWithOptions` removes that final object from
// both format checking and formatting, returning the text and options together.
// Only an exact `Options` type in the final position has this meaning; an absent
// options object supplies `Options{}`.
template <typename Options, typename... Arguments>
[[nodiscard]] auto FormatTerminalTextWithOptions(
    const TerminalFormatStringWithOptions<Options, Arguments...> format,
    Arguments &&...arguments) -> FormattedTerminalText<Options> {
    constexpr auto has_options = formatting_detail::kHasTrailingOptions<Options, Arguments...>;
    constexpr auto format_argument_count = sizeof...(Arguments) - has_options;
    auto argument_tuple = std::forward_as_tuple(std::forward<Arguments>(arguments)...);
    return {
        .text = [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            return FormatTerminalText(format,
                std::get<Index>(std::move(argument_tuple))...);
        }(std::make_index_sequence<format_argument_count>{}),
        .options = [&] {
            if constexpr (has_options) {
                return std::get<format_argument_count>(std::move(argument_tuple));
            } else {
                return Options{};
            }
        }(),
    };
}

}
