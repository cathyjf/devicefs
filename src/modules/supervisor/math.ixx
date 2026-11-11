// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.supervisor.math;

import std;

export namespace devicefs::math {

// Ceiling division requires a nonzero divisor. Rounding the quotient up after
// division avoids the overflow possible when adding `divisor - 1` beforehand.
template <std::size_t Dividend, std::size_t Divisor>
    requires (Divisor != 0)
[[nodiscard]] constexpr auto Ceil() {
    return (Dividend / Divisor) + ((Dividend % Divisor) != 0);
}

template <std::unsigned_integral T>
[[msvc::forceinline]]
constexpr auto SafeSum(const T left, const T right) {
    constexpr auto max = std::numeric_limits<T>::max();
    if (right > (max - left)) {
        throw std::bad_alloc{};
    }
    return left + right;
}

template <std::unsigned_integral T>
[[msvc::forceinline]]
constexpr auto SafeProduct(const T left, const T right) {
    constexpr auto max = std::numeric_limits<T>::max();
    if ((left != 0) && (right > (max / left))) {
        throw std::bad_alloc{};
    }
    return left * right;
}

template <std::unsigned_integral T>
struct SafeAugend {
    T integer{};
    [[msvc::forceinline]]
    constexpr auto operator+(const T right) const {
        return SafeAugend{SafeSum(integer, right)};
    }
};

} // namespace devicefs::math
