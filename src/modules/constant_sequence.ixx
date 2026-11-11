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

export module devicefs.constant_sequence;

import std;
import <devicefs/windows_imports.h>;
import devicefs.allocation;
import devicefs.supervisor.math;

namespace devicefs {

template <class T>
concept IsSequence = requires { std::size(std::declval<T>()); };

[[msvc::forceinline]]
constexpr auto GenericSize(const auto &object) {
    if constexpr (IsSequence<decltype(object)>) {
        return std::size(object);
    } else {
        return 1uz;
    }
}

[[msvc::forceinline]]
constexpr auto GenericData(const auto &object) {
    if constexpr (IsSequence<decltype(object)>) {
        return std::data(object);
    } else {
        return std::addressof(object);
    }
}

template <class Span>
class SequenceJoiner {
public:
    [[msvc::forceinline]]
    constexpr SequenceJoiner(const Span span) noexcept : span_{span} {}

    [[msvc::forceinline]]
    constexpr auto operator+(auto &&operand) const noexcept {
        static_assert(
            sizeof(*GenericData(operand)) == sizeof(typename Span::value_type),
            "source and destination elements must have the same size");
        static_assert(
            std::is_trivially_copyable_v<std::remove_cvref_t<
                decltype(*GenericData(operand))>>,
            "source type must be trivially copyable");
        static_assert(
            std::is_implicit_lifetime_v<typename Span::value_type>,
            "destination type must be an implicit lifetime type");
        const auto size = GenericSize(operand);
        if (size != 0) {
            memcpy(span_.data(), GenericData(operand),
                sizeof(*GenericData(operand)) * size);
        }
        return SequenceJoiner{span_.subspan(size)};
    }
private:
    const Span span_;
};

export
[[msvc::forceinline]]
constexpr auto JoinSequences(const auto span, auto &&...args) {
    (SequenceJoiner{span} + ... + std::forward<decltype(args)>(args));
}

__pragma(warning(push))
__pragma(warning(disable : 26409, justification:
    "`std::make_unique_for_overwrite` cannot allocate uninitialized bytes "
    "with a specified alignment."))
template <class T>
[[msvc::forceinline]]
auto NewUninitializedHeapAllocation(const auto size) {
    using AlignedDeleter = decltype([] [[msvc::forceinline]]
        (auto *const buffer) static {
        ::operator delete(buffer, std::align_val_t{alignof(T)});
    });
    return std::unique_ptr<T[], AlignedDeleter>{
        static_cast<T *>(::operator new(
            devicefs::math::SafeProduct(sizeof(T), size),
            std::align_val_t{alignof(T)}))};
}
__pragma(warning(pop))

template <class T>
using HeapType = decltype(NewUninitializedHeapAllocation<T>(0uz));

struct ConstantSequenceTag {};

export template <class T, std::size_t InlineCapacity>
    requires (std::is_implicit_lifetime_v<T> &&
        std::is_trivially_copyable_v<T> &&
        (InlineCapacity > 0))
__pragma(warning(push))
__pragma(warning(disable : 26490, justification :
    "`reinterpret_cast` is used to provide typed pointers to objects created in "
    "raw storage without needing to retain typed pointers as class members."))
__pragma(warning(disable : 26495, justification :
    "Uninitialized elements are not exposed through the public interface."))
class ConstantSequence : ConstantSequenceTag {
public:
    [[msvc::forceinline]]
    ConstantSequence(auto &&...args) noexcept(sizeof...(args) == 0)
        requires ((!std::is_base_of_v<ConstantSequenceTag,
            std::remove_cvref_t<decltype(args)>>) && ...) :
        size_{(devicefs::math::SafeAugend{0uz} + ... +
            GenericSize(args)).integer} {
        JoinSequences(std::span{[this] {
            if (size_ <= InlineCapacity) {
                return reinterpret_cast<T *>(ConstructInlineArray().data());
            } else {
                return std::construct_at(std::addressof(data_.external_),
                    NewUninitializedHeapAllocation<T>(size_))->get();
            }
        }(), size_}, std::forward<decltype(args)>(args)...);
    }

    [[msvc::forceinline]]
    ConstantSequence(ConstantSequence &&other) noexcept : size_{other.size_} {
        if (size_ <= InlineCapacity) {
            memcpy(ConstructInlineArray().data(), other.data_.inline_.data(),
                sizeof(T) * size_);
        } else {
            std::construct_at(std::addressof(data_.external_),
                std::move(other.data_.external_));
        }
    }

    ConstantSequence(const ConstantSequence &) = delete;
    auto operator=(const ConstantSequence &) -> ConstantSequence & = delete;
    auto operator=(ConstantSequence &&) -> ConstantSequence & = delete;

    [[msvc::forceinline]]
    auto data() const noexcept -> const T * {
        if (size_ <= InlineCapacity) {
            return std::launder(reinterpret_cast<const T *>(
                data_.inline_.data()));
        } else {
            return data_.external_.get();
        }
    }

    [[msvc::forceinline]]
    constexpr auto size() const noexcept {
        return size_;
    }

    [[msvc::forceinline]]
    explicit constexpr operator std::span<const T>() const noexcept {
        return {data(), size()};
    }

    [[msvc::forceinline]]
    ~ConstantSequence() {
        if (size_ > InlineCapacity) {
            std::destroy_at(std::addressof(data_.external_));
        }
    }
private:
    using ByteArray = std::array<std::byte, sizeof(T) * InlineCapacity>;

    [[msvc::forceinline]]
    auto ConstructInlineArray() noexcept -> decltype(auto) {
        // Unlike `std::construct_at`, placement `new` avoids zero-initializing
        // the `ByteArray` object.
        return *::new (std::addressof(data_.inline_)) ByteArray;
    }

    union Allocation {
        alignas(T) ByteArray inline_;
        HeapType<T> external_;

        Allocation() noexcept {}
        Allocation(const Allocation &) = delete;
        auto operator=(const Allocation &) -> Allocation & = delete;
        Allocation(Allocation &&) = delete;
        auto operator=(Allocation &&) -> Allocation & = delete;
        ~Allocation() {}
    } data_{};
    std::size_t size_{};
};
__pragma(warning(pop))

template <class U, class T>
concept IsRawArray =
    std::is_array_v<std::remove_reference_t<U>> &&
    std::same_as<
        std::remove_cv_t<
            std::remove_extent_t<std::remove_reference_t<U>>>,
        std::remove_cv_t<T>>;

export template <class T, std::size_t InlineCapacity>
class ConstantString : private ConstantSequence<T, InlineCapacity> {
public:
    [[msvc::forceinline]]
    explicit ConstantString(auto &&...args) noexcept(sizeof...(args) == 0)
        requires (!IsRawArray<decltype(args), T> && ...) :
        ConstantSequence<T, InlineCapacity>{
            std::forward<decltype(args)>(args)..., T{}} {}

    using ConstantSequence<T, InlineCapacity>::data;

    [[msvc::forceinline]]
    constexpr auto c_str() const noexcept {
        return data();
    }

    [[msvc::forceinline]]
    constexpr auto size() const noexcept {
        return ConstantSequence<T, InlineCapacity>::size() - 1;
    }

    [[msvc::forceinline]]
    constexpr auto length(this auto &&self) noexcept {
        return self.size();
    }

    [[msvc::forceinline]]
    constexpr operator wil::zstring_view(this auto &&self) noexcept {
        return {self.data(), self.size()};
    }

    [[msvc::forceinline]]
    constexpr operator std::string_view(this auto &&self) noexcept {
        return {self.data(), self.size()};
    }
};

} // namespace devicefs

template <class T, std::size_t InlineCapacity>
class std::formatter<devicefs::ConstantString<T, InlineCapacity>> :
    public std::formatter<std::string_view> {};
