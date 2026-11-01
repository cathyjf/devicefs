// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace malloca_detail {

struct Allocation {
    void *buffer = nullptr;
    bool is_stack_allocation = true;
};

template <class T, auto FreeFunction>
class UniqueBuffer {
public:
    [[msvc::forceinline]]
    UniqueBuffer(const Allocation allocation) noexcept :
        buffer_{static_cast<T *>(allocation.buffer)},
        is_stack_allocation_{allocation.is_stack_allocation} {}

    [[msvc::forceinline]]
    auto get() const noexcept {
        return buffer_;
    }

    [[msvc::forceinline]]
    auto operator[](const auto index) const noexcept -> T & {
        return buffer_[index];
    }

    [[msvc::forceinline]]
    explicit operator bool() const noexcept {
        return buffer_ != nullptr;
    }

    [[msvc::forceinline]]
    ~UniqueBuffer() noexcept {
        if (!is_stack_allocation_) {
            FreeFunction(buffer_);
        }
    }
private:
    T *const buffer_;
    const bool is_stack_allocation_;

    UniqueBuffer(const UniqueBuffer &) = delete;
    auto operator=(const UniqueBuffer &) -> UniqueBuffer & = delete;
    UniqueBuffer(UniqueBuffer &&) = delete;
    auto operator=(UniqueBuffer &&) -> UniqueBuffer & = delete;
};

template <class T>
[[msvc::forceinline]]
constexpr auto ShouldUseStack(const auto length) noexcept {
    return (sizeof(T) * length) <= 1024;
}

} // namespace malloca_detail

// Allocate a buffer of `sizeof(T) * length` bytes, using stack or heap storage
// according to the policy defined by `malloca_detail::ShouldUseStack` above.
//
// The returned smart pointer exposes a `T *` buffer. The macro returns a
// `nullptr` owner if the byte count overflows or allocation fails.
// Unlike `_malloca`, this macro avoids calls to out-of-line helpers.
//
// Warning C6255 complains about the use of `_alloca`. That function is used
// to implement the advertised stack allocation policy.
//
// Warning C26408 complains about the use of `std::malloc` and `std::free`.
// Those functions are used to implement heap allocation instead of
// `operator new` and `operator delete` because MSVC's implementation of the
// nonthrowing `operator new` contains a `try` block that MSVC is unable to
// inline into the calling function. The suppression contained within this
// macro is effective to reach the call to `std::free` that occurs within the
// destructor of the `malloca_detail::UniqueBuffer` class.
//
#define NEW_MALLOCA_BUFFER(T, length) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 6255 26408)) \
    malloca_detail::UniqueBuffer<T, std::free>{ \
        ((length) > (SIZE_MAX / sizeof(T))) ? \
            malloca_detail::Allocation{} : \
            malloca_detail::ShouldUseStack<T>(length) ? \
                malloca_detail::Allocation{ \
                    ::_alloca(sizeof(T) * (length)), true} : \
                malloca_detail::Allocation{ \
                    std::malloc(sizeof(T) * (length)), false}} \
    __pragma(warning(pop))
