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

// `_malloca` records whether the allocation used the stack or the heap so
// that `_freea` knows whether to call `free`. The CRT allocates space for
// that record, but C6386 incorrectly reports writing to it as a buffer
// overrun. C26408 and C26477 complain about the presence of `malloc` and
// `NULL` inside of the CRT `_malloca` macro.
#define NEW_MALLOCA_BUFFER(T, bytes) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 6386 26408 26477)) \
    std::unique_ptr<T[], \
        malloca_detail::MallocaDeleter<_freea>>{ \
            static_cast<T *>(_malloca(bytes))} \
    __pragma(warning(pop))
