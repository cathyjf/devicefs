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

module;

export module devicefs.synthetic_backup_block_device;

import std;
import <sal.h>;
import <devicefs/windows_imports.h>;
import devicefs.filesystem;

export namespace devicefs {

template <BlockDevice DeviceType>
class SyntheticBackupBlockDevice {
  public:
    const std::uint64_t length;

    [[nodiscard]] static auto FromBlockDevices(
        DeviceType baseline, DeviceType payload,
        const std::uint64_t block_size,
        std::vector<std::uint64_t> payload_block_offsets) {
        if (baseline.length != payload.length) {
            throw std::runtime_error(
                "the synthetic backup block devices have different lengths");
        }
        return SyntheticBackupBlockDevice{
            std::move(baseline), std::move(payload), block_size,
            std::move(payload_block_offsets)};
    }

    template <typename... Observers>
    [[gsl::suppress("26447",
        justification: "`wil::safe_cast_failfast` cannot throw.")]]
    _Success_(return >= 0)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        const auto output = std::span{
            static_cast<std::byte *>(buffer), wanted};
        const auto request_end = offset + wanted;
        auto position = offset;

        [[gsl::suppress("6001",
            justification:
                "The `_Pre_equal_to_(0)` contract establishes that "
                "`transferred` is initialized before this read.")]]
        while (transferred != wanted) {
            const auto block_offset =
                position - (position % block_size_);
            auto next_payload = std::ranges::lower_bound(
                payload_block_offsets_, block_offset);
            const auto use_payload =
                (next_payload != payload_block_offsets_.end()) &&
                (*next_payload == block_offset);

            auto segment_end = request_end;
            if (use_payload) {
                segment_end = position + std::min(
                    block_size_ - (position % block_size_),
                    request_end - position);
                while ((segment_end != request_end) &&
                    (++next_payload != payload_block_offsets_.end()) &&
                    (*next_payload == segment_end)) {
                    segment_end += std::min(
                        block_size_, request_end - segment_end);
                }
            } else if (next_payload !=
                payload_block_offsets_.end()) {
                segment_end = std::min(request_end, *next_payload);
            }

            // A source segment is a subrange of the ULONG-sized request.
            const auto segment_wanted =
                wil::safe_cast_failfast<ULONG>(segment_end - position);
            auto segment_transferred = ULONG{};
            const auto status =
                (use_payload ? payload_ : baseline_).Read(
                    output.subspan(transferred).data(),
                    position, segment_wanted, segment_transferred,
                    observers...);
            transferred += segment_transferred;

            // The source owns short-read policy. Preserve its partial result
            // instead of retrying or reading a later selection segment.
            if ((status < 0) ||
                (segment_transferred != segment_wanted)) {
                return status;
            }
            position = segment_end;
        }
        return STATUS_SUCCESS;
    }

  private:
    SyntheticBackupBlockDevice(
        DeviceType baseline, DeviceType payload,
        const std::uint64_t block_size,
        std::vector<std::uint64_t> payload_block_offsets) noexcept
        : length{payload.length},
          baseline_{std::move(baseline)},
          payload_{std::move(payload)},
          block_size_{block_size},
          payload_block_offsets_{std::move(payload_block_offsets)} {}

    DeviceType baseline_;
    DeviceType payload_;
    std::uint64_t block_size_;
    std::vector<std::uint64_t> payload_block_offsets_;
};

} // namespace devicefs
