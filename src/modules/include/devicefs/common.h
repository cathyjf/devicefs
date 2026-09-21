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

import std;

// These types use the fundamental types underlying `DWORD` and `HRESULT` so
// that importing this header unit does not require compiling Windows headers.
// The implementation checks those type identities with `static_assert`.
struct ExplicitWin32Error final {
    unsigned long value;

    [[nodiscard]] constexpr operator unsigned long() const noexcept {
        return value;
    }
};

struct ExplicitHresult final {
    long value;

    [[nodiscard]] operator unsigned long() const noexcept;
};

namespace detail {

[[nodiscard]] auto LastWin32Error() noexcept -> ExplicitWin32Error;
[[nodiscard]] auto FormatHresult(long) -> std::string;
[[nodiscard]] auto TranscodeString(std::wstring_view) -> std::string;
[[noreturn]] auto ThrowWinError(unsigned long, const std::string &) -> void;

template <class Argument>
constexpr auto kIsWideStringView = std::is_same_v<
    std::remove_cvref_t<Argument>, std::wstring_view>;

template <class Argument>
using WinErrorFormatArgument = std::conditional_t<
    kIsWideStringView<Argument>, std::string, Argument>;

template <class Argument>
[[nodiscard]] decltype(auto) AdaptWinErrorFormatArgument(
    Argument &&argument) noexcept(!kIsWideStringView<Argument>) {
    if constexpr (kIsWideStringView<Argument>) {
        return TranscodeString(argument);
    } else {
        [[gsl::suppress("26445",
            justification:
                "When `Argument` is an lvalue `std::string_view`, "
                "`decltype(auto)` preserves its reference and triggers "
                "`C26445`. This helper does not store that reference. "
                "It preserves the value category of every non-wide format "
                "argument so `std::format` receives the same argument type "
                "used for compile-time format checking. The returned reference "
                "is consumed immediately by `std::format` in the same "
                "full-expression, while the original `WinError` argument is "
                "still alive. Returning view types by value only to silence "
                "the warning would require a second type transformation and "
                "would make this generic forwarding branch treat views "
                "differently from other arguments.")]]
        return std::forward<Argument>(argument);
    }
}

template <class... Arguments>
constexpr auto kHasExplicitError = [] {
    if constexpr (sizeof...(Arguments) == 0) {
        return false;
    } else {
        using LastArgument = std::remove_cvref_t<std::tuple_element_t<
            sizeof...(Arguments) - 1, std::tuple<Arguments...>>>;
        return std::is_same_v<LastArgument, ExplicitWin32Error> ||
            std::is_same_v<LastArgument, ExplicitHresult>;
    }
}();

template <class Arguments, class Indices>
struct WinErrorFormat;

template <class Arguments, std::size_t... Index>
struct WinErrorFormat<Arguments, std::index_sequence<Index...>> {
    using type = std::format_string<WinErrorFormatArgument<
        std::tuple_element_t<Index, Arguments>>...>;
};

template <class... Arguments>
using WinErrorFormatString = typename WinErrorFormat<
    std::tuple<Arguments...>,
    std::make_index_sequence<
        sizeof...(Arguments) - kHasExplicitError<Arguments...>>>::type;

} // namespace detail

template <class... Arguments>
[[noreturn]] auto WinError(
    const detail::WinErrorFormatString<Arguments...> format,
    Arguments &&...arguments) {
    const auto last_error = detail::LastWin32Error();
    static_assert(
        (!std::is_same_v<std::remove_cvref_t<Arguments>, unsigned long> && ...),
        "wrap an explicit Win32 error in ExplicitWin32Error; "
        "cast a DWORD that is intentionally being formatted");
    constexpr auto has_explicit_error =
        detail::kHasExplicitError<Arguments...>;
    constexpr auto format_argument_count =
        sizeof...(Arguments) - has_explicit_error;
    auto argument_tuple = std::forward_as_tuple(
        std::forward<Arguments>(arguments)...);
    const auto error = [&] {
        if constexpr (has_explicit_error) {
            return std::get<sizeof...(Arguments) - 1>(argument_tuple);
        } else {
            return last_error;
        }
    }();
    const auto details = [&error] {
        if constexpr (std::is_same_v<decltype(error), const ExplicitHresult>) {
            return detail::FormatHresult(error.value);
        }
        return std::string{};
    }();
    const auto operation = [&]<std::size_t... Index>(
        std::index_sequence<Index...>) {
        const auto text = std::format(format,
            detail::AdaptWinErrorFormatArgument(
                std::get<Index>(std::move(argument_tuple)))...);
        if constexpr (std::is_same_v<decltype(error), const ExplicitHresult>) {
            return text + details;
        }
        return text;
    }(std::make_index_sequence<format_argument_count>{});
    detail::ThrowWinError(error, operation);
}

auto HardenProcess() -> void;

template <class Target, auto... Constant, class... Source>
    requires ((sizeof...(Constant) + sizeof...(Source)) == 1)
[[nodiscard, msvc::forceinline]]
constexpr decltype(auto) CompileTimeCast(Source &&...input) {
    [[gsl::suppress("26493",
        justification:
            "C26493 misidentifies this braced initialization as a C-style "
            "cast. The language rejects narrowing conversions here, including "
            "constant values that do not fit in the target type. This "
            "centralized helper function allows the suppression to exist in "
            "only one place.")]]
    return Target{Constant..., std::forward<Source>(input)...};
}
