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
    // The new destroyer captures `index_` by value instead of by reference.
    auto new_destroyer = [index_](auto *const buffer) noexcept {
        std::destroy_n(buffer, index_);
    };
    using NewOwner = std::unique_ptr<T[], decltype(new_destroyer)>;
    struct ReturnedObjects : std::span<T> {
        NewOwner objects;

        ReturnedObjects(std::span<T> &&span, NewOwner &&objects)
            noexcept :
            std::span<T>{std::forward<std::span<T>>(span)},
            objects{std::forward<NewOwner>(objects)} {}
    };
    return ReturnedObjects{std::span{buffer, index_},
        NewOwner{unique_objects.release(), std::move(new_destroyer)}};
}

} // namespace devicefs
