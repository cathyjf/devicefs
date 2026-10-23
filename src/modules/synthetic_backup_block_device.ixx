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

export module devicefs.synthetic_backup_block_device;

import std;
import <devicefs/windows_imports.h>;
import devicefs.count_zeros;
import devicefs.filesystem;

export namespace devicefs {

template <BlockDevice DeviceType, std::uint64_t BlockSize>
    requires (BlockSize > 0)
class SyntheticBackupBlockDevice {
public:
    const std::uint64_t length;

    // Return a reader that takes each `BlockSize`-byte block from `payload`
    // when its bit in `payload_bitmap` is set, and from `baseline` otherwise.
    // The reader interprets each word from its least significant bit to its
    // most significant bit, starting with block zero in the first word.
    // The function throws if the two source lengths differ.
    //
    // Precondition: the bitmap's backing storage must remain alive and unchanged
    // until the returned reader is destroyed and all its reads have completed.
    [[nodiscard]] static auto FromBlockDevices(
        DeviceType baseline, DeviceType payload,
        const std::span<const std::uint64_t> payload_bitmap) {
        if (baseline.length != payload.length) {
            throw std::runtime_error(std::format(
                "the synthetic backup baseline is {} bytes, but the payload is {} bytes",
                baseline.length, payload.length));
        }
        return SyntheticBackupBlockDevice{
            std::move(baseline), std::move(payload),
            payload_bitmap};
    }

    // Read the requested bytes, combining adjacent blocks from the same source
    // into one source read. On a failed or short source read, the function
    // returns its status. It sets `transferred` to the total number of bytes
    // read, including any supplied by a failed or short final source read.
    _Success_(return >= 0)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        auto &...observers) const noexcept -> NTSTATUS {
        const auto output = std::span{
            static_cast<std::byte *>(buffer), wanted};
        const auto request_end = offset + wanted;
        const auto block_limit = ((request_end - 1) / BlockSize) + 1;
        auto position = offset;
        auto block = offset / BlockSize;
        const auto word = block / kBitsPerWord;
        auto use_payload = (word < payload_bitmap_.size()) &&
            ((payload_bitmap_.data()[word] &
                (std::uint64_t{1} << (block % kBitsPerWord))) != 0);

        [[gsl::suppress("6001",
            justification:
                "The `_Pre_equal_to_(0)` contract establishes that "
                "`transferred` is initialized before this read.")]]
        while (transferred != wanted) {
            const auto next_block = use_payload
                ? FindNextBlock<false>(block + 1, block_limit)
                : FindNextBlock<true>(block + 1, block_limit);
            const auto segment_end = next_block == block_limit
                ? request_end : next_block * BlockSize;

            [[gsl::suppress("26472", justification:
                "Each segment is contained in the original read request. The read "
                "precondition `wanted <= length - offset` prevents overflow in "
                "`request_end = offset + wanted`. The loop keeps "
                "`offset <= position <= segment_end <= request_end`, so "
                "`segment_end - position` is between zero and `wanted`. "
                "Because `wanted` is an `ULONG`, this cast preserves the value.")]]
            const auto segment_wanted =
                static_cast<ULONG>(segment_end - position);
            auto segment_transferred = ULONG{};
            const auto &source = use_payload ? payload_ : baseline_;
            // Construction requires both sources to have the exposed length.
            _Analysis_assume_(source.length == length);
            // Completed segments advance `position` and `transferred` equally,
            // so another iteration has bytes remaining. The next transition
            // is after the current block, and the search is bounded by the
            // request's exclusive block limit.
            _Analysis_assume_((position < request_end) && (request_end <= length));
            _Analysis_assume_((segment_wanted > 0) &&
                (segment_wanted <= (request_end - position)));
            const auto status = source.Read(
                output.subspan(transferred).data(),
                position, segment_wanted, segment_transferred,
                observers...);
            transferred += segment_transferred;

            // The source owns short-read policy. Preserve its partial result
            // instead of retrying or reading a later selection segment.
            if (!NT_SUCCESS(status) ||
                (segment_transferred != segment_wanted)) {
                return status;
            }
            position = segment_end;
            block = next_block;
            use_payload = !use_payload;
        }
        return STATUS_SUCCESS;
    }

private:
    static constexpr auto kBitsPerWord =
        std::numeric_limits<std::uint64_t>::digits;

    // Return the first block index in [first, limit) whose bitmap bit equals
    // `payload`, or `limit` if no block matches.
    template <bool payload>
    _Pre_satisfies_(first <= limit)
    _Post_satisfies_((first <= return) && (return <= limit))
    [[nodiscard]] auto FindNextBlock(
        const std::uint64_t first, const std::uint64_t limit) const noexcept
        -> std::uint64_t {
        if (first == limit) {
            return limit;
        }
        constexpr auto all_bits = std::numeric_limits<std::uint64_t>::max();
        constexpr auto nonmatching = payload ? std::uint64_t{} : all_bits;
        const auto last_word = (limit - 1) / kBitsPerWord;
        auto mask = all_bits << (first % kBitsPerWord);
        for (auto word = first / kBitsPerWord; word <= last_word; ++word) {
            const auto word_begin = word * kBitsPerWord;
            if (word >= payload_bitmap_.size()) {
                return payload ? limit : std::max(first, word_begin);
            }
            const auto matching =
                (payload_bitmap_.data()[word] ^ nonmatching) & mask;
            if (matching != 0) {
                return std::min(limit,
                    word_begin + CountTrailingZeros(matching));
            }
            mask = all_bits;
        }
        return limit;
    }

    SyntheticBackupBlockDevice(
        DeviceType baseline, DeviceType payload,
        const std::span<const std::uint64_t> payload_bitmap)
        noexcept(std::is_nothrow_move_constructible_v<DeviceType>)
        : length{payload.length},
          baseline_{std::move(baseline)},
          payload_{std::move(payload)},
          payload_bitmap_{payload_bitmap} {}

    [[msvc::no_unique_address]] DeviceType baseline_;
    [[msvc::no_unique_address]] DeviceType payload_;
    std::span<const std::uint64_t> payload_bitmap_;
};

} // namespace devicefs
