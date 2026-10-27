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
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;

// Query the alignment required for I/O buffers used with `file`. Returns the
// alignment in bytes on success. On failure, returns `std::nullopt` and leaves
// the Windows error code available through `GetLastError()`.
//
// A buffer used for reading from the file must have an address that is a
// multiple of the returned value. To obtain such a buffer, callers can use the
// `NewAlignedBuffer` function defined below.
//
// For unbuffered reads, the starting position in the file and the number of
// bytes requested must also be multiples of the volume's sector size.
// This function returns the buffer alignment, not that sector size.
export [[nodiscard]] auto QueryBufferAlignment(const HANDLE file) noexcept
    -> std::optional<std::align_val_t> {
    auto information = FILE_ALIGNMENT_INFO{};
    if (!GetFileInformationByHandleEx(file, FileAlignmentInfo,
            &information, sizeof(information))) {
        return std::nullopt;
    }
    // `AlignmentRequirement` encodes an N-byte alignment as `N - 1`. Adding one
    // converts that encoding to the byte alignment accepted by `operator new`.
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/initializing-a-device-object
    return std::align_val_t{std::size_t{information.AlignmentRequirement} + 1};
}

namespace devicefs::allocation_detail {

struct AlignedDeleter {
    std::align_val_t alignment;
    auto operator()(auto *const allocation) const noexcept -> void {
        [[gsl::suppress("26409", justification:
            "Invoking `operator delete` is necessary to free the memory "
            "allocated with `operator new` below.")]]
        ::operator delete(allocation, alignment);
    }
};

} // namespace devicefs::allocation_detail

// Allocate a buffer of `size` bytes whose starting address is divisible by
// `alignment`. This is useful for raw volume reads because such reads require
// specially-aligned buffers.
// https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering#alignment-and-file-access-requirements
//
// The function returns `std::unique_ptr{nullptr}` on allocation failure.
export template <devicefs::ByteType T = BYTE>
[[nodiscard, msvc::forceinline]]
auto NewAlignedBuffer(const std::size_t size, const std::align_val_t alignment) noexcept {
    using devicefs::allocation_detail::AlignedDeleter;
    [[gsl::suppress("26409",
        justification:
            "The analyzer suggests using `std::make_unique`, but that "
            "function cannot allocate an over-aligned byte array.")]]
    return std::unique_ptr<T, AlignedDeleter>{
        static_cast<T *>(::operator new(size, alignment, std::nothrow)),
        AlignedDeleter{alignment}};
};
