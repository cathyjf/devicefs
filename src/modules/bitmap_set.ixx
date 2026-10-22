// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.bitmap_set;

import std;
import <devicefs/windows_imports.h>;
import devicefs.terminal.safecast;

export namespace devicefs {

// Store a set of byte offsets using one bit per `BlockSize` bytes.
//
// Precondition of each method that accepts an offset: each offset must be a
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
            auto &word = words_[index];
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
    constexpr auto merge(BitmapSet &source) -> void {
        if ((this == std::addressof(source)) || source.empty()) {
            return;
        }
        if (words_.size() < source.words_.size()) {
            words_.resize(source.words_.size());
        }
        for (auto index = 0uz; index < source.words_.size(); ++index) {
            const auto transferred = source.words_[index] & ~words_[index];
            const auto count = std::popcount(transferred);
            size_ += count;
            words_[index] |= transferred;
            source.words_[index] &= ~transferred;
            source.size_ -= count;
        }
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

    std::vector<std::uint64_t> words_;
    size_type size_ = 0;
};

} // namespace devicefs
