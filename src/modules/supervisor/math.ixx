// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.supervisor.math;

import std;

namespace devicefs::math::detail {

template <class Result, class T>
struct CountDigitsEntry {
    Result digits;
    T threshold;
};

template <std::uint64_t Base,
    std::unsigned_integral Result,
    std::unsigned_integral T>
    requires ((Base >= 2) &&
        (Base <= std::numeric_limits<T>::max()) &&
        requires(T value) { std::bit_width(value); })
constexpr auto CountDigitsTable = [] static consteval noexcept {
    constexpr auto maximum = std::numeric_limits<T>::max();
    constexpr auto base = T{Base};

    constexpr auto maximum_digits = [] static consteval noexcept {
        auto digits = std::uintmax_t{1};
        for (auto number = maximum; number >= base; number /= base) {
            ++digits;
        }
        return digits;
    }();
    static_assert(maximum_digits <= std::numeric_limits<Result>::max(),
        "result type cannot represent the maximum digit count");

    using Entry = CountDigitsEntry<Result, T>;
    auto entries = std::array<Entry,
        std::numeric_limits<T>::digits + std::size_t{1}>{
            Entry{1, 0}};
    auto digits = Result{1};
    auto threshold = base;

    // Each successive bit width doubles the minimum value. Since `Base`
    // is at least two, the digit count can increase by at most one.
    for (auto bits = std::size_t{1}; bits < entries.size(); ++bits) {
        const auto minimum = T{1} << (bits - 1);
        if ((threshold != 0) && (minimum >= threshold)) {
            threshold = (++digits < maximum_digits) ?
                static_cast<T>(threshold * base) : T{0};
        }
        entries[bits] = Entry{digits, threshold};
    }
    return entries;
}();

} // devicefs::math::detail

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

template <std::uint64_t Base = 10,
    std::unsigned_integral Result = std::uint8_t,
    std::unsigned_integral T>
    requires ((Base >= 2) &&
        (Base <= std::numeric_limits<T>::max()) &&
        requires(T value) { std::bit_width(value); })
constexpr auto CountDigits(const T value) noexcept {
    const auto entry = detail::CountDigitsTable<Base, Result, T>[
        std::bit_width(value)];
    [[gsl::suppress("26472", justification:
        "The static assertion in `CountDigitsTable` proves that the result "
        "fits in `Result`.")]]
    return static_cast<Result>(
        entry.digits +
            ((entry.threshold != 0) && (value >= entry.threshold)));
}

} // namespace devicefs::math
