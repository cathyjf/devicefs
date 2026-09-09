// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.safecast;

import std;
#ifdef _WIN32
import <intrin.h>;
#endif

export namespace devicefs::terminal {

// `FailFastCast` converts an integer after checking that its value fits in the
// target type. A value outside that range violates the caller's invariant, so
// failure terminates the process. The constraint admits only conversions for
// which the source type's full range does not fit in the target; conversions
// already safe for every source value use ordinary initialization instead.
template <std::integral Target, std::integral Source>
    requires ((std::numeric_limits<Target>::digits <
        std::numeric_limits<Source>::digits) ||
        (std::is_signed_v<Source> && !std::is_signed_v<Target>))
[[nodiscard]]
#ifdef _MSC_VER
[[msvc::forceinline]]
#endif
constexpr auto FailFastCast(const Source input) noexcept -> Target {
    // Unary `+` promotes character operands to ordinary integer types without
    // changing their values. The standard comparison functions can then accept
    // those values even though the functions exclude `char` and `wchar_t`.
    if (std::cmp_less(+input, +std::numeric_limits<Target>::lowest()) ||
        std::cmp_greater(+input, +std::numeric_limits<Target>::max())) {
#ifdef _WIN32
        // Reason code 8 is FAST_FAIL_RANGE_CHECK_FAILURE.
        __fastfail(8);
#else
        std::terminate();
#endif
    }
#ifdef _MSC_VER
    [[gsl::suppress("26472",
        justification:
            "The range check establishes that the input is representable in "
            "the target type. A failed check terminates before this cast.")]]
#endif
    return static_cast<Target>(input);
}

template <class Target, auto... Constant, class... Source>
    requires ((sizeof...(Constant) + sizeof...(Source)) == 1)
[[nodiscard]]
#ifdef _MSC_VER
[[msvc::forceinline]]
#endif
constexpr decltype(auto) CompileTimeCast(Source &&...input) {
#ifdef _MSC_VER
    [[gsl::suppress("26493",
        justification:
            "C26493 misidentifies this braced initialization as a C-style "
            "cast. The language rejects narrowing conversions here, including "
            "constant values that do not fit in the target type. This "
            "centralized helper function allows the suppression to exist in "
            "only one place.")]]
#endif
    return Target{Constant..., std::forward<Source>(input)...};
}

}
