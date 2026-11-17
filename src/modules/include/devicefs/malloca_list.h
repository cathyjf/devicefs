// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

import std;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import devicefs.object_creator;

namespace malloca_list_detail {

template <class Buffer>
class MallocaList {
    using T = std::remove_reference_t<decltype(std::declval<Buffer>()[0])>;
public:
    [[msvc::forceinline]]
    MallocaList(Buffer &&buffer, auto &&...args)
        noexcept((std::is_nothrow_convertible_v<decltype(args), T> && ...))
        requires ((sizeof...(args) == 0) ||
            (alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)) :
        buffer_{std::forward<Buffer>(buffer)} {
        if constexpr (sizeof...(args) > 0) {
            ::new (buffer_.get()) T[sizeof...(args)]{
                std::forward<decltype(args)>(args)...};
        }
        buffer_.size_ = CompileTimeCast<decltype(buffer_.size_),
            sizeof...(args)>();
    }

    [[msvc::forceinline]]
    constexpr auto data() const noexcept -> const T * {
        return buffer_.get();
    }

    [[msvc::forceinline]]
    constexpr auto size() const noexcept {
        return buffer_.size_;
    }

    // Moving this object is not recommended but MSVC fails to compile certain
    // code that should use guaranteed copy elision (and not move construction)
    // unless this constructor is defined and accessible.
    [[msvc::forceinline]]
    MallocaList(MallocaList &&other) noexcept :
        buffer_{std::move(other.buffer_)} {
        buffer_.size_ = std::exchange(other.buffer_.size_, 0);
    }

    [[msvc::forceinline]]
    ~MallocaList() noexcept(noexcept(
            ::devicefs::DestroyObjects(data(), size()))) {
        ::devicefs::DestroyObjects(data(), size());
    }
private:
    MallocaList(const MallocaList &) = delete;
    auto operator=(const MallocaList &) -> MallocaList & = delete;
    auto operator=(MallocaList &&) -> MallocaList & = delete;

    Buffer buffer_;
};

template <class... Args>
auto MallocaListSizeHelper(Args &&...) -> char(&)[sizeof...(Args) + 1];

} // namespace malloca_list_detail

#define PRIVATE_MALLOCA_LIST_SIZE(...) \
    (sizeof(malloca_list_detail::MallocaListSizeHelper(__VA_ARGS__)) - 1)

// This macro is a workaround for the fact that MSVC's implementation of
// `std::initializer_list` is broken in that when an `initializer_list` is
// created in a conditional expression, MSVC ends the lifetime of the objects
// in the backing array at the end of the statement that declares the
// `initializer_list` instead of when the `initializer_list` goes out of scope,
// contrary to the C++ standard.
//
// Warning C4244 complains about an alleged possible loss of data when
// assigning the value of `buffer_.size_` in the `MallocaList` constructor.
// However, no data can be lost because the assignment uses `CompileTimeCast`.
#define MALLOCA_LIST(T, ...) \
    __pragma(warning(push)) \
    __pragma(warning(disable : 4244)) \
    malloca_list_detail::MallocaList{ \
        NEW_MALLOCA_BUFFER(T, PRIVATE_MALLOCA_LIST_SIZE(__VA_ARGS__)) \
            __VA_OPT__(,) __VA_ARGS__} \
    __pragma(warning(pop))
