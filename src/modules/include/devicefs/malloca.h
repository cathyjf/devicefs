// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace malloca_detail {

struct Allocation {
    void *buffer = nullptr;
    bool is_stack_allocation = true;
};

template <class T>
[[msvc::forceinline]]
auto RemoveConst(T *const pointer) noexcept {
    return pointer;
}

template <class T>
[[msvc::forceinline]]
auto RemoveConst(const T *const pointer) noexcept {
    [[gsl::suppress("26465", justification:
        "`T` is deduced without the pointee's top-level `const` qualification. "
        "This cast removes that qualification.")]]
    return const_cast<T *>(pointer);
}

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
            FreeFunction(RemoveConst(buffer_));
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
concept IsError = requires {
    T{}.what();
};

template <class Error>
[[msvc::forceinline]]
auto HandleNull(void *const buffer = nullptr) noexcept(!IsError<Error>) {
    if constexpr (IsError<Error>) {
        if (!buffer) {
            throw Error{};
        }
    }
    return buffer;
}

template <class T>
[[msvc::forceinline]]
constexpr auto ShouldUseStack(const auto length) noexcept {
    return (sizeof(T) * length) <= 1024;
}

} // namespace malloca_detail

// PRIVATE_IMPLEMENTATION_DETAIL_NEW_MALLOCA_BUFFER(T, Error, length)
//
// This macro is a private implementation detail and should not be directly
// used by callers. Instead, callers should choose one of the following two
// macros: `NEW_MALLOCA_BUFFER_NOTHROW` or `NEW_MALLOCA_BUFFER`.
//
// Unlike `_malloca`, these macros avoid calls to out-of-line helpers.
//
// Here is an explanation of the warning suppressions:
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
#define PRIVATE_IMPLEMENTATION_DETAIL_NEW_MALLOCA_BUFFER(T, Error, length) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 6255 26408)) \
    malloca_detail::UniqueBuffer<T, std::free>{ \
        ((length) > (SIZE_MAX / sizeof(T))) ? \
            malloca_detail::Allocation{malloca_detail::HandleNull<Error>()} : \
            malloca_detail::ShouldUseStack<T>(length) ? \
                malloca_detail::Allocation{ \
                    ::_alloca(sizeof(T) * (length)), true} : \
                malloca_detail::Allocation{ \
                    malloca_detail::HandleNull<Error>( \
                        std::malloc(sizeof(T) * (length))), false}} \
    __pragma(warning(pop))

// NEW_MALLOCA_BUFFER_NOTHROW(T, length)
//
// Allocate a buffer of `sizeof(T) * length` bytes, using stack or heap storage
// according to the policy defined by `malloca_detail::ShouldUseStack` above.
//
// The returned smart pointer exposes a `T *` buffer. The macro returns a
// `nullptr` owner if the byte count overflows or allocation fails.
//
#define NEW_MALLOCA_BUFFER_NOTHROW(T, length) \
    PRIVATE_IMPLEMENTATION_DETAIL_NEW_MALLOCA_BUFFER(T, void, length)

// NEW_MALLOCA_BUFFER(T, length)
//
// This macro is the same as `NEW_MALLOCA_BUFFER_NOTHROW`, except that this
// macro never returns a `nullptr` owner. Instead, it throws `std::bad_alloc`
// on failure.
//
#define NEW_MALLOCA_BUFFER(T, length) \
    PRIVATE_IMPLEMENTATION_DETAIL_NEW_MALLOCA_BUFFER(T, std::bad_alloc, length)
