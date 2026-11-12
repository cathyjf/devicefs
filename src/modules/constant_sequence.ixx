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
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import devicefs.allocation;
import devicefs.supervisor.math;
import devicefs.terminal.safecast;

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

template <class T>
class SequenceJoiner {
public:
    [[msvc::forceinline]]
    constexpr SequenceJoiner(T *const cursor) noexcept : cursor_{cursor} {}

    [[msvc::forceinline]]
    constexpr auto operator+(auto &&operand) const noexcept {
        static_assert(
            sizeof(*GenericData(operand)) == sizeof(*cursor_),
            "source and destination elements must have the same size");
        static_assert(
            std::is_trivially_copyable_v<std::remove_cvref_t<
                decltype(*GenericData(operand))>>,
            "source type must be trivially copyable");
        static_assert(
            std::is_implicit_lifetime_v<std::remove_cvref_t<
                decltype(*cursor_)>>,
            "destination type must be an implicit lifetime type");
        const auto size = GenericSize(operand);
        if (size != 0) {
            std::memcpy(cursor_, GenericData(operand),
                sizeof(*GenericData(operand)) * size);
        }
        return SequenceJoiner{cursor_ + size};
    }
private:
    T *const cursor_;
};

export
[[msvc::forceinline]]
constexpr auto JoinSequences(auto *const buffer, auto &&...args) {
    (SequenceJoiner{buffer} + ... + std::forward<decltype(args)>(args));
}

template <class Target, class Source>
concept IsNarrowingRequired = requires {
    devicefs::terminal::TryCast(std::declval<Source>(),
        std::declval<Target &>());
};

template <class Target, class Source>
[[msvc::forceinline]]
auto SafeNarrow(const Source source)
    noexcept (!IsNarrowingRequired<Target, Source>) {
    if constexpr (IsNarrowingRequired<Target, Source>) {
        /* [[indeterminate]] */ Target output;
        if (!devicefs::terminal::TryCast(source, output)) {
            throw std::bad_alloc{};
        }
        return output;
    } else {
        return source;
    }
}

__pragma(warning(push))
__pragma(warning(disable : 26409, justification:
    "`std::make_unique_for_overwrite` cannot allocate uninitialized bytes "
    "with a specified alignment."))
template <class T>
[[msvc::forceinline]]
auto NewUninitializedHeapAllocation(const std::size_t size) {
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

export template <class T, std::size_t InlineCapacity,
    class SizeType = std::size_t>
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
    using HeapType = devicefs::HeapType<T>;
    constexpr static auto layout = [] static consteval noexcept {
        constexpr auto kBytesNeededForStorage = std::max(
            sizeof(HeapType), sizeof(T) * InlineCapacity);
        constexpr auto kSizeBegin =
            devicefs::math::Ceil<kBytesNeededForStorage, alignof(SizeType)>() *
                alignof(SizeType);

        struct Layout {
            std::size_t size_begin = kSizeBegin;
            std::size_t array_bytes = kSizeBegin + sizeof(SizeType);
            std::size_t array_alignment =
                std::max({alignof(T), alignof(HeapType), alignof(SizeType)});
        };
        return Layout{};
    }();
public:
    [[msvc::forceinline]]
    ConstantSequence(auto &&...args) noexcept(sizeof...(args) == 0)
        requires ((!std::is_base_of_v<ConstantSequenceTag,
            std::remove_cvref_t<decltype(args)>>) && ...) {
        const auto &size = SetSize(SafeNarrow<SizeType>(
            (devicefs::math::SafeAugend{0uz} + ... +
                GenericSize(args)).integer));
        JoinSequences([this, &size] [[msvc::forceinline]] {
            if (size <= InlineCapacity) {
                return GetInlineStorage();
            } else {
                return (::new (storage_.data()) HeapType{
                    NewUninitializedHeapAllocation<T>(size)})->get();
            }
        }(), std::forward<decltype(args)>(args)...);
    }

    [[msvc::forceinline]]
    ConstantSequence(ConstantSequence &&other) noexcept {
        const auto &size = SetSize(other.size());
        if (size <= InlineCapacity) {
            std::memcpy(GetInlineStorage(), other.GetInlineStorage(),
                sizeof(T) * size);
        } else {
            ::new (storage_.data()) HeapType{std::move(other.GetHeap())};
        }
    }

    ConstantSequence(const ConstantSequence &) = delete;
    auto operator=(const ConstantSequence &) -> ConstantSequence & = delete;
    auto operator=(ConstantSequence &&) -> ConstantSequence & = delete;

    [[msvc::forceinline]]
    auto data() const noexcept -> const T * {
        if (size() <= InlineCapacity) {
            return GetInlineStorage();
        } else {
            return GetHeap().get();
        }
    }

    [[msvc::forceinline]]
    auto size() const noexcept {
        return *std::launder(reinterpret_cast<const SizeType *>(
            storage_.data() + layout.size_begin));
    }

    [[msvc::forceinline]]
    explicit constexpr operator std::span<const T>() const noexcept {
        return {data(), size()};
    }

    [[gsl::suppress("26457", justification:
        "Destructors do not return values.")]]
    [[msvc::forceinline]]
    ~ConstantSequence() {
        if (size() > InlineCapacity) {
            GetHeap().~HeapType();
        }
    }
private:
    [[msvc::forceinline]]
    auto GetInlineStorage(this auto &self) noexcept {
        using Element = std::conditional_t<
            std::is_const_v<std::remove_reference_t<decltype(self)>>,
                const T, T>;
        return std::launder(
            reinterpret_cast<Element *>(self.storage_.data()));
    }

    [[msvc::forceinline]]
    auto GetHeap(this auto &self) noexcept -> decltype(auto) {
        using Element = std::conditional_t<
            std::is_const_v<std::remove_reference_t<decltype(self)>>,
            const HeapType, HeapType>;
        return *std::launder(
            reinterpret_cast<Element *>(self.storage_.data()));
    }

    [[msvc::forceinline]]
    auto SetSize(const SizeType size) noexcept -> decltype(auto) {
        return *::new (storage_.data() + layout.size_begin) SizeType{size};
    }

    alignas(layout.array_alignment) std::array<std::byte, layout.array_bytes>
        storage_;
};
__pragma(warning(pop))

template <class U, class T>
concept IsRawArray =
    std::is_array_v<std::remove_reference_t<U>> &&
    std::same_as<
        std::remove_cv_t<
            std::remove_extent_t<std::remove_reference_t<U>>>,
        std::remove_cv_t<T>>;

export template <class T, std::size_t InlineCapacity,
    class SizeType = std::size_t>
class ConstantString : private ConstantSequence<T, InlineCapacity, SizeType> {
    using Base = ConstantSequence<T, InlineCapacity, SizeType>;
public:
    [[msvc::forceinline]]
    explicit ConstantString(auto &&...args) noexcept(sizeof...(args) == 0)
        requires (!IsRawArray<decltype(args), T> && ...) :
        Base{std::forward<decltype(args)>(args)..., T{}} {}

    using Base::data;

    [[msvc::forceinline]]
    constexpr auto c_str() const noexcept {
        return data();
    }

    [[msvc::forceinline]]
    constexpr auto size() const noexcept {
        return Base::size() - 1uz;
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

template <class T, std::size_t InlineCapacity, class SizeType>
class std::formatter<devicefs::ConstantString<T, InlineCapacity, SizeType>> :
    public std::formatter<std::string_view> {};
