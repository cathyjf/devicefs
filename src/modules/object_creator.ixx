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

namespace devicefs::object_creator_detail {

// Destroy a sequence of objects using the same algorithm as if the objects
// were stored in a fixed-size array that is going out of scope.
__pragma(warning(push))
__pragma(warning(disable : 26457, justification:
    "Destructors do not return values."))
template <class T, bool NoExceptionGuard = std::is_nothrow_destructible_v<T>>
auto DestroyObjects(T *const buffer, const std::size_t length)
    noexcept(NoExceptionGuard) -> void {
    if constexpr (NoExceptionGuard) {
        for (auto i = length; i > 0;) {
            (buffer + --i)->~T();
        }
    } else {
        struct ExceptionGuard {
            T *const buffer;
            std::size_t remaining;
            ~ExceptionGuard() noexcept {
                if (remaining != 0) {
                    DestroyObjects<T, true>(buffer, remaining);
                }
            }
        };
        auto guard = ExceptionGuard{buffer, length};
        while (guard.remaining > 0) {
            (buffer + --guard.remaining)->~T();
        }
    }
}
__pragma(warning(pop))

} // namespace devicefs::object_creator_detail

namespace devicefs {

// Create objects in a buffer.
//
// Preconditions:
//   (1) `buffer` points to a buffer of at least
//       `sizeof(T) * collection.size()` writable bytes; and
//   (2) `buffer` is suitably aligned for creation of objects of type `T`.
//
// Restriction:
//   The behavior of calling `std::span<T>::operator=` on the returned object
//   is undefined.
export template <class T>
auto CreateObjects(T *const buffer, auto &collection, auto &&callback) {
    auto index_ = std::size_t{};
    auto destroyer = [&index_](auto *const buffer) noexcept {
        object_creator_detail::DestroyObjects<T, true>(buffer, index_);
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
        ~ReturnedObjects() noexcept(std::is_nothrow_destructible_v<T>) {
            object_creator_detail::DestroyObjects(
                std::span<T>::data(), std::span<T>::size());
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
