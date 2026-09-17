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

export module devicefs.allocation;

import std;
import <devicefs/windows_imports.h>;

namespace devicefs::allocation_detail {

const auto page_size = [] {
    auto information = SYSTEM_INFO{};
    GetSystemInfo(&information);
    return std::align_val_t{information.dwPageSize};
}();

} // devicefs::allocation_detail

// Return an array of `size` objects of type `T` aligned to the system page
// size. This is useful for raw volume reads because such reads can require
// sector-aligned buffers. Microsoft recommends page-aligned allocations
// for these buffers.
// https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering#alignment-and-file-access-requirements
//
// If `terminate_on_failure` is true, allocation failures terminate the process
// Otherwise, the function returns `std::unique_ptr{nullptr}` on failure.
export
template <class T = BYTE, bool terminate_on_failure = true>
    requires std::is_trivially_destructible_v<T>
[[msvc::forceinline]]
auto NewPageAlignedArray(const std::size_t size) noexcept {
    const auto deleter = [](T *const allocation) noexcept {
        [[gsl::suppress("26409",
            justification:
                "Invoking `operator delete[]` is necessary to free the memory "
                "allocated below.")]]
        ::operator delete[](allocation, devicefs::allocation_detail::page_size);
    };
    return std::unique_ptr<T[], decltype(deleter)>{
        [size] noexcept {
            [[gsl::suppress("26409",
                justification:
                    "The analyzer suggests using `std::make_unique`, but that "
                    "function cannot allocate an over-aligned byte array.")]]
            const auto buffer = ::new (devicefs::allocation_detail::page_size, std::nothrow) T[size];
            if constexpr (terminate_on_failure) {
                if (!buffer) {
                    std::terminate();
                }
            }
            return buffer;
        }(),
        deleter
    };
};
