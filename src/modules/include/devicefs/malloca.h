// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace malloca_detail {

template <auto FreeFunction>
struct MallocaDeleter {
    static void operator()(auto *const buffer) noexcept {
        FreeFunction(buffer);
    }
    MallocaDeleter() = default;
    MallocaDeleter(const MallocaDeleter &) = delete;
    auto operator=(const MallocaDeleter &) -> MallocaDeleter & = delete;
    MallocaDeleter(MallocaDeleter &&) = delete;
    auto operator=(MallocaDeleter &&) -> MallocaDeleter & = delete;
};

} // namespace malloca_detail

// Allocate a buffer for `length` elements of type `T`, using stack or heap
// storage according to `_malloca`'s size threshold. The returned smart pointer
// exposes a `T *` buffer and releases the storage with `_freea`. The macro
// returns a `nullptr` owner if the byte count overflows or allocation fails.
//
// `_malloca` records whether the allocation used the stack or the heap so
// that `_freea` knows whether to call `free`. The CRT allocates space for
// that record, but C6386 incorrectly reports writing to it as a buffer
// overrun. C26408 and C26477 complain about the presence of `malloc` and
// `NULL` inside of the CRT `_malloca` macro. C6255 complains about `_alloca`
// inside `_malloca`, despite recommending `_malloca` as its replacement.
//
// In order for the suppressions to be effective, `NEW_MALLOCA_BUFFER` must
// appear on the same line of source code as the variable initialization.
//
// For example, this works:
//     const auto buffer = NEW_MALLOCA_BUFFER(std::byte,
//         length);
//
// By contrast, this raises analyzer errors:
//     const auto buffer =
//         NEW_MALLOCA_BUFFER(std::byte, length);
//
#define NEW_MALLOCA_BUFFER(T, length) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 6255 6386 26408 26477)) \
    std::unique_ptr<T[], \
        malloca_detail::MallocaDeleter<_freea>>{ \
            ((length) > \
                (std::numeric_limits<std::size_t>::max() / sizeof(T))) \
                ? nullptr \
                : static_cast<T *>(_malloca(sizeof(T) * (length)))} \
    __pragma(warning(pop))
