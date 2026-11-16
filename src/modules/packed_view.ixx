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

export module devicefs.packed_view;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;

namespace devicefs {

// This type is similar to an `std::span` or `std::string_view` except that
// the number of bytes used to store the length is customizable.
export template <class Element, std::size_t SizeBytes>
    requires ((SizeBytes >= 1) && (SizeBytes <= 8))
__pragma(warning(push))
__pragma(warning(disable : 26495, justification :
    "Uninitialized storage is not exposed through the public interface."))
class PackedView {
    using Size = decltype([] static {
        if constexpr (SizeBytes == 1) {
            return std::uint8_t{};
        } else if constexpr (SizeBytes == 2) {
            return std::uint16_t{};
        } else if constexpr (SizeBytes <= 4) {
            return std::uint32_t{};
        } else {
            return std::uint64_t{};
        }
    }());
    static_assert((sizeof(Size) == SizeBytes) ||
        (std::endian::native == std::endian::little) ||
        (std::endian::native == std::endian::big));
    constexpr static auto kMaxSize =
        std::numeric_limits<std::uint64_t>::max() >> ((8 - SizeBytes) * 8);
    constexpr static auto kPointerSize = sizeof(Element *);
public:
    constexpr PackedView(const Element *const pointer, const auto size)
        noexcept {
        std::memcpy(storage_.data(),
            std::addressof(pointer), kPointerSize);
        if (std::cmp_greater(+size, kMaxSize) || std::cmp_less(+size, 0)) {
            __fastfail(FAST_FAIL_RANGE_CHECK_FAILURE);
        }
        [[gsl::suppress("26472", justification:
            "The above range check proves that this cast is safe.")]]
        const auto size_ = static_cast<Size>(size);
        constexpr auto offset = (std::endian::native == std::endian::little) ?
            0 : (sizeof(Size) - SizeBytes);
        std::memcpy(storage_.data() + kPointerSize,
            ByteViewCast<const std::byte *>(std::addressof(size_)) + offset,
            SizeBytes);
    }
    constexpr PackedView(auto &&view)
        noexcept (noexcept(view.data()) && noexcept(view.size())) :
        PackedView{view.data(), view.size()} {}
    operator std::basic_string_view<Element>() const noexcept {
        return {data(), size()};
    }
    auto data() const noexcept {
        return BitPrefixCast<const Element *>(*storage_.data());
    }
    auto size() const noexcept -> Size {
        constexpr auto extra_bytes = sizeof(Size) - SizeBytes;
        const auto word = BitPrefixCast<Size>(
            std::get<kPointerSize - extra_bytes>(storage_));
        if constexpr (std::endian::native == std::endian::little) {
            return word >> (extra_bytes * 8);
        } else {
            return word & kMaxSize;
        }
    }
    auto operator[](const auto index) const noexcept -> decltype(auto) {
        return data()[index];
    }
private:
    std::array<std::byte, kPointerSize + SizeBytes> storage_;
};
__pragma(warning(pop))

} // namespace devicefs
