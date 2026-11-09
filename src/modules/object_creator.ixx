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

export module devicefs.object_creator;

import std;

namespace devicefs {

// Create objects in a buffer.
//
// Preconditions:
//   (1) `buffer` points to a buffer of at least
//       `sizeof(T) * collection.size()` writable bytes; and
//   (2) `buffer` is suitably aligned for creation of objects of type `T`.
//
// Objects need not be trivially destructible. However, if destruction of a
// created object throws an exception, the program will terminate.
//
// Restriction:
//   The behavior of calling `std::span<T>::operator=` on the returned object
//   is undefined.
export template <class T>
auto CreateObjects(T *const buffer, auto &collection, auto &&callback) {
    auto index_ = std::size_t{};
    auto destroyer = [&index_](auto *const buffer) noexcept {
        std::destroy_n(buffer, index_);
    };
    auto unique_objects = std::unique_ptr<T[], decltype(destroyer)>{
        buffer, std::move(destroyer)};
    for (auto &entry : collection) {
        if constexpr (std::is_invocable_v<
                decltype(callback), decltype(entry)>) {
            ::new (buffer + index_) T{callback(entry)};
        } else if (!callback(buffer + index_, entry)) {
            break;
        }
        ++index_;
    }
    struct ReturnedObjects : std::span<T> {
        ReturnedObjects(T *const buffer, const std::size_t length) noexcept:
            std::span<T>{buffer, length} {}
        ~ReturnedObjects() noexcept {
            std::destroy_n(std::span<T>::data(), std::span<T>::size());
        }

        __pragma(warning(push))
        __pragma(warning(disable : 26456, justification:
            "Under the stated restriction, the hidden non-virtual operators "
            "should not be called."))
        ReturnedObjects(const ReturnedObjects &) = delete;
        ReturnedObjects(ReturnedObjects &&) = delete;
        auto operator=(const ReturnedObjects &) -> ReturnedObjects & = delete;
        auto operator=(ReturnedObjects &&) -> ReturnedObjects & = delete;
        __pragma(warning(pop))
    };
    return ReturnedObjects{unique_objects.release(), index_};
}

} // namespace devicefs
