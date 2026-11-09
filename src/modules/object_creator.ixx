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

template <class T, bool NoExceptionGuard = std::is_nothrow_destructible_v<T>>
auto DestroyObjects(T *, std::size_t) noexcept(NoExceptionGuard) -> void;

__pragma(warning(push))
__pragma(warning(disable : 26432, justification:
    "Deleting the move and copy constructors and assignment operators as "
    "suggested by the analyzer would disable aggregate initialization without "
    "gaining any corresponding benefit."))
template <class T>
struct ExceptionGuard {
    T *const buffer;
    std::size_t index;
    auto Release() noexcept -> std::size_t {
        return std::exchange(index, 0);
    }
    ~ExceptionGuard() noexcept {
        if (index != 0) {
            DestroyObjects<T, true>(buffer, index);
        }
    }
};
__pragma(warning(pop))

// Destroy a sequence of objects using the same algorithm as if the objects
// were stored in a fixed-size array that is going out of scope.
__pragma(warning(push))
__pragma(warning(disable : 26457, justification:
    "Destructors do not return values."))
template <class T, bool NoExceptionGuard>
auto DestroyObjects(T *const buffer, const std::size_t length)
    noexcept(NoExceptionGuard) -> void {
    if constexpr (NoExceptionGuard) {
        for (auto i = length; i > 0;) {
            (buffer + --i)->~T();
        }
    } else {
        auto guard = ExceptionGuard{buffer, length};
        while (guard.index > 0) {
            (buffer + --guard.index)->~T();
        }
    }
}
__pragma(warning(pop))

} // namespace devicefs::object_creator_detail

namespace devicefs {

export enum class ObjectCreationDisposition {
    ObjectCreated,
    StopCreatingObjects,
    SkipThisObject
};

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
auto CreateObjects(T *const buffer_, auto &&collection, auto &&callback) {
    using ConstructedType = std::remove_const_t<T>;
    auto *const buffer = const_cast<ConstructedType *>(buffer_);
    auto guard = object_creator_detail::ExceptionGuard{buffer, 0};
    for (auto &entry : collection) {
        auto *const target = buffer + guard.index;
        if constexpr (std::is_invocable_v<
                decltype(callback), decltype(entry)>) {
            ::new (target) ConstructedType{callback(entry)};
        } else if constexpr (std::is_invocable_v<
                decltype(callback), decltype(entry), std::size_t>) {
            ::new (target) ConstructedType{callback(entry, guard.index)};
        } else {
            static_assert(std::is_same_v<decltype(callback(target, entry)),
                ObjectCreationDisposition>,
                "this callback must return an `ObjectCreationDisposition`");
            const auto result = callback(target, entry);
            if (result == ObjectCreationDisposition::StopCreatingObjects) {
                break;
            } else if (result == ObjectCreationDisposition::SkipThisObject) {
                continue;
            }
        }
        ++guard.index;
    }
    __pragma(warning(push))
    __pragma(warning(disable : 26456, justification:
        "Under the stated restriction, the hidden non-virtual operators "
        "should not be called."))
    __pragma(warning(disable : 26432, justification :
        "All of the identified member functions are defined or deleted."))
    struct ManagedObjects : std::span<T> {
        ManagedObjects(T *const buffer, const std::size_t length) noexcept:
            std::span<T>{buffer, length} {}
        ~ManagedObjects() noexcept(std::is_nothrow_destructible_v<T>) {
            object_creator_detail::DestroyObjects(
                std::span<T>::data(), std::span<T>::size());
        }

        ManagedObjects(const ManagedObjects &) = delete;
        ManagedObjects(ManagedObjects &&) = delete;
        auto operator=(const ManagedObjects &) -> ManagedObjects & = delete;
        auto operator=(ManagedObjects &&) -> ManagedObjects & = delete;
    };
    __pragma(warning(pop))
    return std::conditional_t<
        std::is_trivially_destructible_v<T>, std::span<T>, ManagedObjects>{
            buffer, guard.Release()};
}

} // namespace devicefs
