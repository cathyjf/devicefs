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

} // namespace devicefs::math
