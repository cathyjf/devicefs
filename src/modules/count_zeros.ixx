// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.count_zeros;

import std;
import <devicefs/windows_imports.h>;

export namespace devicefs {

// Return the number of low-order zero bits in `value`.
// This function uses the bit-scan intrinsic because it emits `RBIT` and `CLZ`
// on ARM64, while MSVC 14.51 implements `std::countr_zero` with a software loop.
[[nodiscard, msvc::forceinline]]
_Post_satisfies_(return < 64)
auto CountTrailingZeros(_Pre_satisfies_(value != 0) const std::uint64_t value)
    noexcept {
    // The nonzero-input precondition ensures that `_BitScanForward64` writes
    // to `bit_index`.
    [[assume(value != 0)]];
    /* [[indeterminate]] */ unsigned long bit_index;
    std::ignore = _BitScanForward64(&bit_index, value);
    [[assume(bit_index < 64)]];
    return bit_index;
}

} // namespace devicefs
