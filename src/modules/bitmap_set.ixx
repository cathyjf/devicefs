// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.bitmap_set;

import std;
import <devicefs/windows_imports.h>;
import devicefs.terminal.safecast;

namespace devicefs::bitmap_set_detail {

const auto free_deleter = [](auto *const buffer) static noexcept {
    [[gsl::suppress("26408", justification:
        "`free` is required to free memory allocated with `realloc` below.")]]
    std::free(buffer);
};

// Constraint on `T`: an all-zero object representation must be valid.
template <class T>
    requires (std::is_implicit_lifetime_v<T> &&
        std::is_trivially_copyable_v<T> &&
        (alignof(T) <= alignof(std::max_align_t)))
class ReallocVector {
    constexpr static auto max_size =
        std::numeric_limits<std::size_t>::max() / sizeof(T);

public:
    explicit constexpr ReallocVector(
        const std::size_t maximum_size = max_size) noexcept
        : max_size_{std::min(maximum_size, max_size)} {}

    _Pre_satisfies_(new_size <= max_size_)
    auto resize(const std::size_t new_size) -> void {
        if (new_size == 0) {
            clear();
            return;
        }
        if (std::cmp_greater(new_size, max_size_)) {
            throw std::bad_alloc{};
        }
        if ((new_size > reservation_) || (new_size < size_)) {
            const auto new_reservation = new_reservation_size(new_size);
            [[gsl::suppress("26408", justification:
                "`realloc` is required to resize a memory allocation.")]]
            auto *const buffer = static_cast<T *>(std::realloc(
                storage_.get(), new_reservation * sizeof(T)));
            if (!buffer) {
                throw std::bad_alloc{};
            }
            storage_.release();
            storage_.reset(buffer);
            if (std::cmp_greater(new_reservation, reservation_)) {
                std::memset(buffer + reservation_, 0,
                    (new_reservation - reservation_) * sizeof(T));
            }
            reservation_ = new_reservation;
        }
        size_ = new_size;
    }

    constexpr auto clear() noexcept -> void {
        size_ = reservation_ = 0;
        storage_.reset();
    }

    auto operator[](const std::size_t index) noexcept -> T & {
        return storage_[index];
    }

    auto operator[](const std::size_t index) const noexcept -> const T & {
        return storage_[index];
    }

    operator std::span<const T>() const noexcept {
        return {storage_.get(), size_};
    }

    auto size() const noexcept -> std::size_t {
        return size_;
    }

private:
    auto new_reservation_size(const std::size_t new_size) const noexcept
        -> std::size_t {
        if (new_size < size_) {
            return new_size;
        }
        const auto minimum_reservation = std::min(10uz, max_size_);
        constexpr auto geometric_factor = 2uz;
        const auto fraction_max_size = max_size_ / geometric_factor;
        const auto proposed_max = (reservation_ > fraction_max_size) ?
            max_size_ : (reservation_ * geometric_factor);
        if (new_size > minimum_reservation) {
            return (new_size > proposed_max) ? new_size : proposed_max;
        }
        return (minimum_reservation > proposed_max) ?
            minimum_reservation : proposed_max;
    }

    std::size_t max_size_;
    std::size_t size_{};
    std::size_t reservation_{};
    std::unique_ptr<T[], std::remove_cv_t<decltype(free_deleter)>> storage_{};
};

}

export namespace devicefs {

// Store a set of byte offsets using one bit per `BlockSize` bytes.
//
// Precondition of insertion, erasure, and lookup: each offset must be a
// multiple of `BlockSize`.
//
// Storage grows with the largest inserted offset, not the number of members.
//
// Moving an object into `*this` leaves the source in an unspecified state.
template <std::uint64_t BlockSize>
    // Requiring at least two bytes per block limits the set to at most
    // 2**63 keys, which fits in the 64-bit `size_type`.
    requires (BlockSize > 1)
class BitmapSet {
public:
    using key_type = std::uint64_t;
    using value_type = key_type;
    using size_type = std::uint64_t;

    BitmapSet() = default;

    // Bound reservation growth by the inclusive `maximum_offset`, which need
    // not be block-aligned. Storage is still allocated only when needed.
    explicit constexpr BitmapSet(const key_type maximum_offset)
        : words_{devicefs::terminal::FailFastMaybeCast<std::size_t>(
            ((maximum_offset / BlockSize) >> kWordShift) + 1)} {}

    constexpr BitmapSet(const std::initializer_list<value_type> values) {
        insert_range(values);
    }

    template <std::input_iterator Iterator,
        std::sentinel_for<Iterator> Sentinel>
    constexpr BitmapSet(Iterator first, const Sentinel last) {
        insert(first, last);
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_type {
        return size_;
    }

    // The low bit of the first word represents offset zero. Subsequent bits
    // represent consecutive blocks of `BlockSize` bytes.
    [[nodiscard]] constexpr auto storage() const noexcept
        -> std::span<const std::uint64_t> {
        return words_;
    }

    [[nodiscard]] constexpr auto contains(const key_type value) const noexcept
        -> bool {
        const auto bit = block_index(value);
        const auto word = bit >> kWordShift;
        return (word < words_.size()) &&
            ((words_[devicefs::terminal::FailFastMaybeCast<std::size_t>(word)] &
                mask(bit)) != 0);
    }

    constexpr auto insert(const value_type value) -> bool {
        const auto bit = block_index(value);
        const auto index =
            devicefs::terminal::FailFastMaybeCast<std::size_t>(
                bit >> kWordShift);
        if (index >= words_.size()) {
            words_.resize(index + 1);
        }
        const auto bit_mask = mask(bit);
        const auto inserted = (words_[index] & bit_mask) == 0;
        if (inserted) {
            ++size_;
            words_[index] |= bit_mask;
        }
        return inserted;
    }

    template <std::input_iterator Iterator,
        std::sentinel_for<Iterator> Sentinel>
    constexpr auto insert(Iterator first, const Sentinel last) -> void {
        for (; first != last; ++first) {
            insert(*first);
        }
    }

    constexpr auto insert_range(std::ranges::input_range auto &&values)
        -> void {
        insert(std::ranges::begin(values), std::ranges::end(values));
    }

    // Insert every block offset in [first, last).
    _Pre_satisfies_(first <= last)
    constexpr auto insert_range(
        const key_type first, const key_type last) -> void {
        if (first > last) [[unlikely]] {
            __fastfail(FAST_FAIL_INVALID_ARG);
        }
        const auto first_bit = block_index(first);
        const auto limit_bit = block_index(last);
        if (first_bit == limit_bit) {
            return;
        }
        const auto last_bit = limit_bit - 1;
        const auto first_word =
            devicefs::terminal::FailFastMaybeCast<std::size_t>(
                first_bit >> kWordShift);
        const auto last_word =
            devicefs::terminal::FailFastMaybeCast<std::size_t>(
                last_bit >> kWordShift);
        if (last_word >= words_.size()) {
            words_.resize(last_word + 1);
        }

        const auto insert_bits = [this](const std::size_t index,
            const std::uint64_t bits) {
            // `words_` owns a separate allocation, so `word` cannot alias
            // the membership count `size_`.
            auto &__restrict word = words_[index];
            size_ += std::popcount(bits & ~word);
            word |= bits;
        };
        constexpr auto all_bits = std::numeric_limits<std::uint64_t>::max();
        const auto first_mask =
            all_bits << (first_bit & (kBitsPerWord - 1));
        const auto last_mask = all_bits >>
            ((kBitsPerWord - 1) - (last_bit & (kBitsPerWord - 1)));
        if (first_word == last_word) {
            insert_bits(first_word, first_mask & last_mask);
            return;
        }
        insert_bits(first_word, first_mask);
        for (auto index = first_word + 1; index < last_word; ++index) {
            insert_bits(index, all_bits);
        }
        insert_bits(last_word, last_mask);
    }

    constexpr auto erase(const key_type value) noexcept -> size_type {
        if (!contains(value)) {
            return 0;
        }
        const auto bit = value / BlockSize;
        words_[devicefs::terminal::FailFastMaybeCast<std::size_t>(
            bit >> kWordShift)] &= ~mask(bit);
        --size_;
        return 1;
    }

    constexpr auto clear() noexcept -> void {
        words_.clear();
        size_ = 0;
    }

    // Move elements from `source` into this set. Elements that are already
    // present in this set remain in `source`, just as with `std::set::merge`.
    [[gsl::suppress("26429", justification:
        "Nullable pointers (as opposed to references) are required to perform "
        "pointer arithmetic.")]]
    constexpr auto merge(BitmapSet &source) -> void {
        if ((this == std::addressof(source)) || source.empty()) {
            return;
        }
        if (words_.size() < source.words_.size()) {
            words_.resize(source.words_.size());
        }
        // Each set's bitmap words are stored in a separate heap allocation,
        // outside either `BitmapSet` object. The self-merge check ensures that
        // the two word arrays are also separate.
        auto *__restrict const destination_words = std::addressof(words_[0]);
        auto *__restrict const source_words = std::addressof(source.words_[0]);
        auto transferred_count = size_type{};
        for (auto index = 0uz; index < source.words_.size(); ++index) {
            const auto transferred =
                source_words[index] & ~destination_words[index];
            transferred_count += std::popcount(transferred);
            destination_words[index] |= transferred;
            source_words[index] &= ~transferred;
        }
        size_ += transferred_count;
        source.size_ -= transferred_count;
    }

private:
    static constexpr auto kBitsPerWord =
        std::numeric_limits<std::uint64_t>::digits;
    static constexpr auto kWordShift =
        std::countr_zero(devicefs::terminal::CompileTimeCast<
            std::uint64_t, kBitsPerWord>());

    [[nodiscard]] static constexpr auto block_index(const key_type value)
        noexcept -> key_type {
        if ((value % BlockSize) != 0) [[unlikely]] {
            __fastfail(FAST_FAIL_INVALID_ARG);
        }
        return value / BlockSize;
    }

    [[nodiscard]] static constexpr auto mask(const key_type bit) noexcept {
        return std::uint64_t{1} << (bit & (kBitsPerWord - 1));
    }

    bitmap_set_detail::ReallocVector<std::uint64_t> words_;
    size_type size_ = 0;
};

template <std::uint64_t BlockSize>
class MaybeBitmapSet {
    using value_type = std::uint64_t;
    using set_type = std::set<value_type>;
    using bitmap_set_type = BitmapSet<BlockSize>;
    static_assert(std::is_same_v<
        value_type, typename bitmap_set_type::value_type>);

public:
    explicit MaybeBitmapSet(const value_type maximum_offset)
        : maximum_offset_{maximum_offset} {}

    [[nodiscard]]
    _Pre_satisfies_((value % BlockSize) == 0)
    auto insert(const value_type value) -> bool {
        if (std::holds_alternative<bitmap_set_type>(set_)) {
            return std::get<bitmap_set_type>(set_).insert(value);
        }
        if ((value % BlockSize) != 0) [[unlikely]] {
            __fastfail(FAST_FAIL_INVALID_ARG);
        }
        auto &set = std::get<set_type>(set_);
        if (!set.insert(value).second) {
            return false;
        }
        maybe_convert_to_bitmap();
        return true;
    }

private:
    // Precondition: `set_` holds a non-empty `set_type`.
    auto maybe_convert_to_bitmap() -> void {
        constexpr auto magic_byte_threshold = 5242880u; // 5 MiB
        constexpr auto approximate_bytes_per_node = 32u;
        // This assertion allows most of the divisions in the calculation of
        // `comparison_target` below to be replaced with a shift.
        // If `BlockSize` is also a power of two, the entire calculation can
        // be replaced with a bit shift.
        static_assert(std::has_single_bit(approximate_bytes_per_node));
        constexpr auto magic_size_threshold =
            magic_byte_threshold / approximate_bytes_per_node;

        auto &set = std::get<set_type>(set_);
        if (std::cmp_less(set.size(), magic_size_threshold)) {
            return;
        }
        // The precondition ensures that the set is not empty.
        [[assume(set.rbegin() != set.rend())]];
        const auto comparison_target =
            *set.rbegin() / BlockSize /
            std::numeric_limits<std::uint8_t>::digits /
            approximate_bytes_per_node;
        if (std::cmp_less(set.size(), comparison_target)) {
            return;
        }
        convert_to_bitmap();
    }

    // Precondition: `set_` holds a `set_type`.
    // Postcondition: `set_` holds a `bitmap_set_type`.
    auto convert_to_bitmap() -> void {
        auto &set = std::get<set_type>(set_);
        auto bitmap = bitmap_set_type(maximum_offset_);
        bitmap.insert(set.rbegin(), set.rend());
        set_.template emplace<bitmap_set_type>(std::move(bitmap));
    }

    value_type maximum_offset_;
    std::variant<set_type, bitmap_set_type> set_;
};

} // namespace devicefs
