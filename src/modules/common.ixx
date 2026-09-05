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

module;

export module devicefs.common;

import std;
import <windows.h>;

export struct ExplicitWin32Error final {
    DWORD value;

    [[nodiscard]] static constexpr auto FromHresult(const HRESULT hr) {
        // An HRESULT's facility identifies the source of its error code.
        // Only `FACILITY_WIN32` wraps a Win32 error number; stripping any
        // other facility could report an unrelated Win32 failure.
        if (HRESULT_FACILITY(hr) != FACILITY_WIN32) {
            return ExplicitWin32Error{std::bit_cast<DWORD>(hr)};
        }
        [[gsl::suppress("26472",
            justification:
                "The definition of HRESULT_CODE ensures that the result will "
                "always fit in a DWORD. `static_cast` is used here instead "
                "of `wil::safe_cast_failfast` because this translation unit "
                "does not otherwise depend on WIL.")]]
        return ExplicitWin32Error{static_cast<DWORD>(HRESULT_CODE(hr))};
    }
};

namespace detail {

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
        return std::filesystem::path{argument}.string();
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
constexpr auto kHasExplicitWin32Error = [] {
    if constexpr (sizeof...(Arguments) == 0) {
        return false;
    } else {
        using LastArgument = std::tuple_element_t<
            sizeof...(Arguments) - 1, std::tuple<Arguments...>>;
        return std::is_same_v<
            std::remove_cvref_t<LastArgument>, ExplicitWin32Error>;
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
        sizeof...(Arguments) - kHasExplicitWin32Error<Arguments...>>>::type;

} // namespace detail

export template <class... Arguments>
[[noreturn]] auto WinError(
    const detail::WinErrorFormatString<Arguments...> format,
    Arguments &&...arguments) {
    const auto last_error = GetLastError();
    static_assert(
        (!std::is_same_v<std::remove_cvref_t<Arguments>, DWORD> && ...),
        "wrap an explicit Win32 error in ExplicitWin32Error; "
        "cast a DWORD that is intentionally being formatted");
    constexpr auto has_explicit_error =
        detail::kHasExplicitWin32Error<Arguments...>;
    constexpr auto format_argument_count =
        sizeof...(Arguments) - has_explicit_error;
    auto argument_tuple = std::forward_as_tuple(
        std::forward<Arguments>(arguments)...);
    const auto error = [&] {
        if constexpr (has_explicit_error) {
            return std::get<sizeof...(Arguments) - 1>(argument_tuple).value;
        } else {
            return last_error;
        }
    }();
    const auto operation = [&]<std::size_t... Index>(
        std::index_sequence<Index...>) {
        return std::format(format,
            detail::AdaptWinErrorFormatArgument(
                std::get<Index>(std::move(argument_tuple)))...);
    }(std::make_index_sequence<format_argument_count>{});
    throw std::system_error(
        std::bit_cast<int>(error), std::system_category(),
        operation);
}

export auto HardenProcess() {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        WinError("could not restrict DLL search directories");
    }

    auto dynamic_code = PROCESS_MITIGATION_DYNAMIC_CODE_POLICY{};
    dynamic_code.ProhibitDynamicCode = 1;
    if (!SetProcessMitigationPolicy(
            ProcessDynamicCodePolicy, &dynamic_code, sizeof(dynamic_code))) {
        WinError("could not prohibit dynamic code");
    }

    auto strict_handles = PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY{};
    strict_handles.RaiseExceptionOnInvalidHandleReference = 1;
    strict_handles.HandleExceptionsPermanentlyEnabled = 1;
    if (!SetProcessMitigationPolicy(
            ProcessStrictHandleCheckPolicy, &strict_handles, sizeof(strict_handles))) {
        WinError("could not enable strict handle checking");
    }

    auto extension_points = PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY{};
    extension_points.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy,
            &extension_points, sizeof(extension_points))) {
        WinError("could not disable legacy extension points");
    }

    auto image_load = PROCESS_MITIGATION_IMAGE_LOAD_POLICY{};
    image_load.NoRemoteImages = 1;
    image_load.NoLowMandatoryLabelImages = 1;
    if (!SetProcessMitigationPolicy(
            ProcessImageLoadPolicy, &image_load, sizeof(image_load))) {
        WinError("could not restrict image loading");
    }
}
