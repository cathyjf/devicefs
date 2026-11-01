// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace malloca_detail {

struct MallocaDeleter {
    bool is_stack_allocation{};

    [[msvc::forceinline]]
    auto operator()(auto *const buffer) noexcept {
        if (!is_stack_allocation) {
            ::operator delete(buffer);
        }
    }
};

template <class T>
[[msvc::forceinline]]
constexpr auto ShouldUseAlloca(const auto length) noexcept {
    return (sizeof(T) * length) <= 1024;
}

} // namespace malloca_detail

// Allocate a buffer for `length` elements of type `T`, using stack or heap
// storage according to the policy defined by `ShouldUseAlloca` above.
//
// The returned smart pointer exposes a `T *` buffer. The macro returns a
// `nullptr` owner if the byte count overflows or allocation fails.
//
// Unlike `_malloca`, this macro avoids calls to out-of-line helpers.
//
// Warnings C6255 and C26409 complain about the use of `_alloca` and
// `operator new`, respectively. Those functions are used to implement the
// advertised allocation policy.
//
#define NEW_MALLOCA_BUFFER(T, length) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 6255 26409)) \
    std::unique_ptr<T[], malloca_detail::MallocaDeleter>{ \
        ((length) > (SIZE_MAX / sizeof(T))) ? \
            nullptr : static_cast<T *>( \
                malloca_detail::ShouldUseAlloca<T>(length) ? \
                    ::_alloca(sizeof(T) * (length)) : \
                    ::operator new(sizeof(T) * (length), std::nothrow)), \
            malloca_detail::MallocaDeleter{ \
                malloca_detail::ShouldUseAlloca<T>(length)}} \
    __pragma(warning(pop))
