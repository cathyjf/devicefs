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

#include <devicefs/strsafe_compat.h>

export module devicefs.vhdx_viewer;

import std;
import <devicefs/windows_imports.h>;
import <winternl.h>;
import <sal.h>;
import devicefs.common;

namespace devicefs::vhdx_detail {

constexpr auto kOneMiB = std::uint64_t{1024 * 1024};

// The header section is exactly one MiB. See VHDX Format Specification
// v1.00, §2.3, p. 12.
constexpr auto kHeaderSectionLength = kOneMiB;

// Log fields and region-table offsets and lengths use one-MiB units. This
// view places a one-MiB zero log and a one-MiB metadata region consecutively
// at the first available aligned offsets, then starts the BAT at the next one.
// See VHDX Format Specification v1.00, §§3.1.2–3.1.3, pp. 15–18.
constexpr auto kLogOffset = kHeaderSectionLength;
constexpr auto kLogLength = kOneMiB;
constexpr auto kMetadataOffset = kLogOffset + kLogLength;
constexpr auto kMetadataLength = kOneMiB;
constexpr auto kBatOffset = kMetadataOffset + kMetadataLength;
constexpr auto kLogicalSectorSize = std::uint64_t{512};

// The largest block size permitted by the format minimizes the generated BAT.
// It does not change the size of reads delegated to the source block device.
// See VHDX Format Specification v1.00, §3.5.2.1, p. 32.
constexpr auto kPayloadBlockSize = std::uint64_t{256} * kOneMiB;

// VirtualDiskSize is limited to 64 TB by VHDX Format Specification v1.00,
// §3.5.2.2, p. 32.
constexpr auto kMaximumVirtualDiskSize = std::uint64_t{64} * 1024 * 1024 * kOneMiB;

// UEFI does not require the partition to begin at LBA 2048. The 1 MiB offset
// is a deliberate alignment choice matching the default used by modern
// Windows partitioning tools.
constexpr auto kGptPartitionOffset = kOneMiB;
constexpr auto kGptPartitionLba = kGptPartitionOffset / kLogicalSectorSize;

// Microsoft Windows 2000 Server Operations Guide, Tables 1.12–1.13, places
// the NTFS boot sector's two-byte end marker at 0x1fe, its eight-byte volume
// serial number at 0x48, and its four-byte checksum at 0x50.
constexpr auto kNtfsBootRecordLength = std::size_t{512};
constexpr auto kNtfsSerialOffset = std::size_t{0x48};
constexpr auto kNtfsChecksumOffset = std::size_t{0x50};
constexpr auto kNtfsPatchLength =
    kNtfsChecksumOffset + sizeof(std::uint32_t) - kNtfsSerialOffset;

// The VHDX structures below are serialized exactly as specified. Their source
// is VHDX Format Specification v1.00, §§3.1.2–3.1.3, pp. 14–18;
// §§3.5.1–3.5.2.5, pp. 30–33; and §4.1, p. 36.
#pragma pack(push, 1)
struct VhdxHeader {
    std::uint32_t signature;
    std::uint32_t checksum;
    std::uint64_t sequence_number;
    GUID file_write_id;
    GUID data_write_id;
    GUID log_id;
    std::uint16_t log_version;
    std::uint16_t version;
    std::uint32_t log_length;
    std::uint64_t log_offset;
};

struct RegionTableHeader {
    std::uint32_t signature;
    std::uint32_t checksum;
    std::uint32_t entry_count;
    std::uint32_t reserved;
};

struct RegionTableEntry {
    GUID id;
    std::uint64_t file_offset;
    std::uint32_t length;
    std::uint32_t flags;
};

struct MetadataTableHeader {
    std::uint64_t signature;
    std::uint16_t reserved;
    std::uint16_t entry_count;
    std::array<std::uint32_t, 5> reserved2;
};

struct MetadataTableEntry {
    GUID id;
    std::uint32_t offset;
    std::uint32_t length;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct FileParameters {
    std::uint32_t block_size;
    std::uint32_t flags;
};

struct MbrPartitionEntry {
    std::uint8_t boot_indicator;
    std::array<std::uint8_t, 3> starting_chs;
    std::uint8_t type;
    std::array<std::uint8_t, 3> ending_chs;
    std::uint32_t starting_lba;
    std::uint32_t sector_count;
};

struct GptHeader {
    std::uint64_t signature;
    std::uint32_t revision;
    std::uint32_t header_size;
    std::uint32_t checksum;
    std::uint32_t reserved;
    std::uint64_t current_lba;
    std::uint64_t backup_lba;
    std::uint64_t first_usable_lba;
    std::uint64_t last_usable_lba;
    GUID disk_id;
    std::uint64_t partition_entries_lba;
    std::uint32_t partition_entry_count;
    std::uint32_t partition_entry_size;
    std::uint32_t partition_entries_checksum;
};

struct GptPartitionEntry {
    GUID type_id;
    GUID partition_id;
    std::uint64_t first_lba;
    std::uint64_t last_lba;
    std::uint64_t attributes;
    std::array<char16_t, 36> name;
};
#pragma pack(pop)

static_assert(sizeof(VhdxHeader) == 80);
static_assert(sizeof(RegionTableHeader) == 16);
static_assert(sizeof(RegionTableEntry) == 32);
static_assert(sizeof(MetadataTableHeader) == 32);
static_assert(sizeof(MetadataTableEntry) == 32);
static_assert(sizeof(FileParameters) == 8);
static_assert(sizeof(MbrPartitionEntry) == 16);
static_assert(sizeof(GptHeader) == 92);
static_assert(sizeof(GptPartitionEntry) == 128);

// UEFI Specification 2.11, §§5.2.3 and 5.3.1–5.3.3, pp. 113–118,
// defines the protective MBR, the primary and backup GPT headers, and their
// partition-entry arrays. It reserves at least 16 KiB for each array; 128
// entries of the specified 128-byte form provide that space.
constexpr auto kGptEntryCount = std::uint32_t{128};
constexpr auto kGptEntryArrayLength =
    std::uint64_t{kGptEntryCount} * sizeof(GptPartitionEntry);
constexpr auto kGptEntryArraySectors =
    kGptEntryArrayLength / kLogicalSectorSize;
constexpr auto kGptSuffixLength =
    kGptEntryArrayLength + kLogicalSectorSize;
constexpr auto kGptEnvelopeLength =
    kGptPartitionOffset + kGptSuffixLength;

constexpr auto kBatRegionId = GUID{
    0x2dc27766, 0xf623, 0x4200,
    {0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08}};
constexpr auto kMetadataRegionId = GUID{
    0x8b7ca206, 0x4790, 0x4b9a,
    {0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e}};
constexpr auto kFileParametersId = GUID{
    0xcaa16737, 0xfa36, 0x4d43,
    {0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b}};
constexpr auto kVirtualDiskSizeId = GUID{
    0x2fa54224, 0xcd1b, 0x4876,
    {0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8}};
constexpr auto kPage83DataId = GUID{
    0xbeca12ab, 0xb2e6, 0x4523,
    {0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46}};
constexpr auto kLogicalSectorSizeId = GUID{
    0x8141bf1d, 0xa96f, 0x4709,
    {0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f}};
constexpr auto kPhysicalSectorSizeId = GUID{
    0xcda348c7, 0x445d, 0x4471,
    {0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56}};

// Windows identifies an ordinary data volume with PARTITION_BASIC_DATA_GUID.
// See Microsoft's "Windows and GPT FAQ", "Basic data partition".
constexpr auto kBasicDataPartitionId = GUID{
    0xebd0a0a2, 0xb9e5, 0x4433,
    {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};

template <typename Value>
auto SerializeObject(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const Value &value) noexcept -> void {
    static_assert(std::endian::native == std::endian::little);
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memcpy(
        destination.subspan(offset, sizeof(value)).data(),
        std::addressof(value), sizeof(value));
}

auto CopyBytes(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::span<const std::byte> source) noexcept -> void {
    std::memcpy(destination.subspan(offset, source.size()).data(),
        source.data(), source.size());
}

template <typename Value>
[[nodiscard]] auto DeserializeObject(
    const std::span<const std::byte> source,
    const std::size_t offset) noexcept -> Value {
    static_assert(std::endian::native == std::endian::little);
    static_assert(std::is_trivially_copyable_v<Value>);
    auto result = Value{};
    std::memcpy(
        std::addressof(result),
        source.subspan(offset, sizeof(result)).data(), sizeof(result));
    return result;
}

template <std::uint32_t Polynomial>
[[nodiscard]] constexpr auto Crc32(
    const std::span<const std::byte> input) noexcept -> std::uint32_t {
    auto result = std::numeric_limits<std::uint32_t>::max();
    for (const auto byte : input) {
        result ^= std::to_integer<std::uint32_t>(byte);
        for (auto bit = 0; bit < 8; ++bit) {
            const auto mask = std::uint32_t{} - (result & 1u);
            result = (result >> 1) ^ (Polynomial & mask);
        }
    }
    return ~result;
}

[[nodiscard]] constexpr auto VhdxCrc32(
    const std::span<const std::byte> input) noexcept {
    // The specification selects CRC-32C with the Castagnoli polynomial.
    // See VHDX Format Specification v1.00, §4.2, p. 36.
    return Crc32<0x82f63b78>(input);
}

[[nodiscard]] constexpr auto GptCrc32(
    const std::span<const std::byte> input) noexcept {
    // UEFI Specification 2.11, §5.3.2, pp. 115–117, uses the ordinary
    // EFI CRC32 algorithm for the GPT header and partition-entry array.
    return Crc32<0xedb88320>(input);
}

[[nodiscard]] constexpr auto AlignUp(
    const std::uint64_t value,
    const std::uint64_t alignment) noexcept {
    return value + ((alignment - (value % alignment)) % alignment);
}

[[gsl::suppress("26447",
    justification: "`wil::safe_cast_failfast` cannot throw.")]]
[[nodiscard]] auto EncodeChs(const std::uint64_t lba) noexcept {
    // UEFI Specification 2.11, §5.2.3, p. 113 requires a protective-MBR CHS
    // address when it is representable and FF-FF-FF otherwise. Use the
    // conventional 255-head, 63-sector translation for that legacy field.
    constexpr auto head_count = std::uint64_t{255};
    constexpr auto sectors_per_track = std::uint64_t{63};
    constexpr auto cylinder_count = std::uint64_t{1024};
    if (lba >= cylinder_count * head_count * sectors_per_track) {
        return std::array{std::uint8_t{0xff}, std::uint8_t{0xff},
            std::uint8_t{0xff}};
    }

    const auto cylinder = lba / (head_count * sectors_per_track);
    const auto head = (lba / sectors_per_track) % head_count;
    const auto sector = (lba % sectors_per_track) + 1;
    return std::array{
        wil::safe_cast_failfast<std::uint8_t>(head),
        wil::safe_cast_failfast<std::uint8_t>(
            sector | ((cylinder >> 2) & 0xc0)),
        wil::safe_cast_failfast<std::uint8_t>(cylinder & 0xff)};
}

[[nodiscard]] auto NewGuid() {
    auto result = GUID{};
    const auto error = CoCreateGuid(&result);
    if (FAILED(error)) {
        WinError("could not create a VHDX identifier",
            ExplicitWin32Error::FromHresult(error));
    }
    return result;
}

[[nodiscard]] auto MakeVhdxHeader(
    const std::uint64_t sequence_number,
    const GUID &file_write_id,
    const GUID &data_write_id) noexcept {
    auto result = std::array<std::byte, 4096>{};
    auto header = VhdxHeader{
        .signature = 0x64616568,
        .sequence_number = sequence_number,
        .file_write_id = file_write_id,
        .data_write_id = data_write_id,
        .version = 1,
        .log_length = std::uint32_t{kLogLength},
        .log_offset = kLogOffset,
    };
    SerializeObject(result, 0, header);
    header.checksum = VhdxCrc32(result);
    SerializeObject(result, 0, header);
    return result;
}

[[nodiscard]] auto MakeRegionTable(
    const std::uint64_t bat_length) {
    auto result = std::vector<std::byte>(64 * 1024);
    auto header = RegionTableHeader{
        .signature = 0x69676572,
        .entry_count = 2,
    };
    SerializeObject(result, 0, header);
    SerializeObject(result, sizeof(header), RegionTableEntry{
        .id = kBatRegionId,
        .file_offset = kBatOffset,
        .length = wil::safe_cast_failfast<std::uint32_t>(bat_length),
        .flags = 1,
    });
    SerializeObject(result, sizeof(header) + sizeof(RegionTableEntry),
        RegionTableEntry{
            .id = kMetadataRegionId,
            .file_offset = kMetadataOffset,
            .length = std::uint32_t{kMetadataLength},
            .flags = 1,
        });
    header.checksum = VhdxCrc32(result);
    SerializeObject(result, 0, header);
    return result;
}

[[nodiscard]] auto MakeMetadata(
    const std::uint64_t virtual_disk_length,
    const GUID &page83_data) {
    constexpr auto item_offset = std::uint32_t{64 * 1024};
    constexpr auto file_parameters_length =
        std::uint32_t{sizeof(FileParameters)};
    constexpr auto virtual_disk_size_length =
        std::uint32_t{sizeof(virtual_disk_length)};
    constexpr auto page83_data_length = std::uint32_t{sizeof(page83_data)};
    constexpr auto sector_size_length = std::uint32_t{sizeof(std::uint32_t)};
    constexpr auto file_parameters_offset = item_offset;
    constexpr auto virtual_disk_size_offset =
        file_parameters_offset + file_parameters_length;
    constexpr auto page83_data_offset =
        virtual_disk_size_offset + virtual_disk_size_length;
    constexpr auto logical_sector_size_offset =
        page83_data_offset + page83_data_length;
    constexpr auto physical_sector_size_offset =
        logical_sector_size_offset + sector_size_length;
    constexpr auto required = std::uint32_t{1u << 2};
    constexpr auto virtual_disk = std::uint32_t{1u << 1};

    auto result = std::vector<std::byte>(kMetadataLength);
    const auto header = MetadataTableHeader{
        .signature = 0x617461646174656d,
        .entry_count = 5,
    };
    SerializeObject(result, 0, header);
    auto entry_offset = sizeof(header);
    const auto add_entry = [&](const GUID &id, const std::uint32_t offset,
                               const std::uint32_t length,
                               const std::uint32_t flags) {
        SerializeObject(result, entry_offset, MetadataTableEntry{
            .id = id,
            .offset = offset,
            .length = length,
            .flags = flags,
        });
        entry_offset += sizeof(MetadataTableEntry);
    };
    add_entry(kFileParametersId, file_parameters_offset,
        file_parameters_length, required);
    add_entry(kVirtualDiskSizeId, virtual_disk_size_offset,
        virtual_disk_size_length,
        required | virtual_disk);
    add_entry(kPage83DataId, page83_data_offset, page83_data_length,
        required | virtual_disk);
    add_entry(kLogicalSectorSizeId, logical_sector_size_offset,
        sector_size_length,
        required | virtual_disk);
    add_entry(kPhysicalSectorSizeId, physical_sector_size_offset,
        sector_size_length,
        required | virtual_disk);

    SerializeObject(result, file_parameters_offset, FileParameters{
        .block_size = std::uint32_t{kPayloadBlockSize},
        .flags = 1,
    });
    SerializeObject(result, virtual_disk_size_offset, virtual_disk_length);
    SerializeObject(result, page83_data_offset, page83_data);
    SerializeObject(result, logical_sector_size_offset,
        std::uint32_t{kLogicalSectorSize});
    SerializeObject(result, physical_sector_size_offset,
        std::uint32_t{kLogicalSectorSize});
    return result;
}

[[nodiscard]] auto MakeBat(
    const std::uint64_t virtual_disk_length) {
    const auto payload_block_count =
        AlignUp(virtual_disk_length, kPayloadBlockSize) / kPayloadBlockSize;
    constexpr auto chunk_ratio =
        ((std::uint64_t{1} << 23) * kLogicalSectorSize) /
        kPayloadBlockSize;
    const auto entry_count = payload_block_count +
        ((payload_block_count - 1) / chunk_ratio);
    const auto bat_length = AlignUp(
        entry_count * sizeof(std::uint64_t), kOneMiB);
    const auto payload_offset = kBatOffset + bat_length;
    auto result = std::vector<std::byte>(
        wil::safe_cast_failfast<std::size_t>(bat_length));

    // Fixed VHDX payload blocks are fully present. Sector-bitmap entries are
    // interleaved but remain zero. See VHDX Format Specification v1.00,
    // §§3.3–3.4.1.2, pp. 24–29.
    for (auto payload = std::uint64_t{};
        payload < payload_block_count; ++payload) {
        const auto bat_index = payload + (payload / chunk_ratio);
        const auto file_offset = payload_offset + payload * kPayloadBlockSize;
        const auto entry = ((file_offset / kOneMiB) << 20) | 6u;
        SerializeObject(result,
            wil::safe_cast_failfast<std::size_t>(
                bat_index * sizeof(entry)),
            entry);
    }
    return result;
}

struct GptLayout {
    std::vector<std::byte> prefix;
    std::vector<std::byte> suffix;
};

[[nodiscard]] auto MakeGpt(const std::uint64_t source_length) {
    const auto source_sectors = source_length / kLogicalSectorSize;
    const auto partition_last_lba =
        kGptPartitionLba + source_sectors - 1;
    const auto backup_entries_lba = partition_last_lba + 1;
    const auto backup_header_lba =
        backup_entries_lba + kGptEntryArraySectors;
    const auto disk_id = NewGuid();

    auto partition_entries =
        std::vector<std::byte>(kGptEntryArrayLength);
    SerializeObject(partition_entries, 0, GptPartitionEntry{
        .type_id = kBasicDataPartitionId,
        .partition_id = NewGuid(),
        .first_lba = kGptPartitionLba,
        .last_lba = partition_last_lba,
    });
    const auto partition_entries_checksum = GptCrc32(partition_entries);

    const auto make_header = [&](const std::uint64_t current_lba,
                                 const std::uint64_t other_lba,
                                 const std::uint64_t entries_lba) {
        auto result = std::array<std::byte, 512>{};
        auto header = GptHeader{
            .signature = 0x5452415020494645,
            .revision = 0x00010000,
            .header_size = sizeof(GptHeader),
            .current_lba = current_lba,
            .backup_lba = other_lba,
            .first_usable_lba = 2 + kGptEntryArraySectors,
            .last_usable_lba = partition_last_lba,
            .disk_id = disk_id,
            .partition_entries_lba = entries_lba,
            .partition_entry_count = kGptEntryCount,
            .partition_entry_size = sizeof(GptPartitionEntry),
            .partition_entries_checksum = partition_entries_checksum,
        };
        SerializeObject(result, 0, header);
        header.checksum = GptCrc32(
            std::span{result}.first(sizeof(header)));
        SerializeObject(result, 0, header);
        return result;
    };

    auto prefix = std::vector<std::byte>(kGptPartitionOffset);
    const auto protective_sector_count = std::min(
        backup_header_lba,
        std::uint64_t{std::numeric_limits<std::uint32_t>::max()});
    SerializeObject(prefix, 446, MbrPartitionEntry{
        .starting_chs = EncodeChs(1),
        .type = 0xee,
        .ending_chs = EncodeChs(backup_header_lba),
        .starting_lba = 1,
        .sector_count =
            wil::safe_cast_failfast<std::uint32_t>(protective_sector_count),
    });
    SerializeObject(prefix, 510, std::uint16_t{0xaa55});
    const auto primary_header = make_header(1, backup_header_lba, 2);
    CopyBytes(prefix, kLogicalSectorSize, primary_header);
    CopyBytes(prefix, 2 * kLogicalSectorSize, partition_entries);

    auto suffix = std::vector<std::byte>(kGptSuffixLength);
    CopyBytes(suffix, 0, partition_entries);
    const auto backup_header = make_header(
        backup_header_lba, 1, backup_entries_lba);
    CopyBytes(suffix, kGptEntryArrayLength, backup_header);
    return GptLayout{
        .prefix = std::move(prefix),
        .suffix = std::move(suffix),
    };
}

struct VhdxLayout {
    std::uint64_t file_length;
    std::vector<std::byte> prefix;
    std::vector<std::byte> gpt_prefix;
    std::vector<std::byte> gpt_suffix;
};

[[nodiscard]] auto MakeLayout(const std::uint64_t source_length) {
    if ((source_length < kLogicalSectorSize) ||
        ((source_length % kLogicalSectorSize) != 0)) {
        throw std::runtime_error(std::format(
            "a VHDX source must contain at least one complete {}-byte sector; "
            "the source contains {} bytes",
            kLogicalSectorSize, source_length));
    }
    if (source_length > kMaximumVirtualDiskSize - kGptEnvelopeLength) {
        throw std::runtime_error(std::format(
            "the {}-byte source volume exceeds the {}-byte maximum for a VHDX view",
            source_length, kMaximumVirtualDiskSize - kGptEnvelopeLength));
    }

    auto gpt = MakeGpt(source_length);
    const auto virtual_disk_length =
        gpt.prefix.size() + source_length + gpt.suffix.size();
    auto bat = MakeBat(virtual_disk_length);
    const auto payload_offset = kBatOffset + bat.size();
    auto prefix = std::vector<std::byte>(payload_offset);

    // The file identifier, duplicate headers, and duplicate region tables use
    // the fixed offsets defined by VHDX Format Specification v1.00,
    // §§3.1.1–3.1.3, pp. 14–18.
    SerializeObject(prefix, 0, std::uint64_t{0x656c696678646876});
    const auto file_write_id = NewGuid();
    const auto data_write_id = NewGuid();
    const auto first_header =
        MakeVhdxHeader(1, file_write_id, data_write_id);
    const auto second_header =
        MakeVhdxHeader(2, file_write_id, data_write_id);
    CopyBytes(prefix, 64 * 1024, first_header);
    CopyBytes(prefix, 128 * 1024, second_header);
    const auto region_table = MakeRegionTable(bat.size());
    CopyBytes(prefix, 192 * 1024, region_table);
    CopyBytes(prefix, 256 * 1024, region_table);

    // A zero LogGuid means that the one-MiB zero-filled log has nothing to
    // replay. The required metadata items are defined in VHDX Format
    // Specification v1.00, §§3.1.2 and 3.5.1–3.5.2.5, pp. 15–16 and
    // 30–33.
    const auto metadata = MakeMetadata(virtual_disk_length, NewGuid());
    CopyBytes(prefix, kMetadataOffset, metadata);
    CopyBytes(prefix, kBatOffset, bat);

    return VhdxLayout{
        .file_length = payload_offset +
            AlignUp(virtual_disk_length, kPayloadBlockSize),
        .prefix = std::move(prefix),
        .gpt_prefix = std::move(gpt.prefix),
        .gpt_suffix = std::move(gpt.suffix),
    };
}

template <typename DeviceType>
auto ReadExact(
    DeviceType &source,
    std::span<std::byte> output,
    const std::uint64_t offset,
    const std::string_view description) {
    auto transferred = ULONG{};
    const auto status = source.Read(
        output.data(), offset,
        wil::safe_cast_failfast<ULONG>(output.size()), transferred);
    // NTSTATUS < 0 indicates failure.
    // See <https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values>.
    if (status < 0) {
        throw std::runtime_error(std::format(
            "could not read {} (NTSTATUS 0x{:08x})",
            description, std::bit_cast<std::uint32_t>(status)));
    }
    if (transferred != output.size()) {
        throw std::runtime_error(std::format(
            "could not read {} completely: requested {} bytes at offset 0x{:x}, "
            "received {} bytes",
            description, output.size(), offset, transferred));
    }
}

struct NtfsIdentityPatch {
    std::uint64_t offset;
    std::array<std::byte, kNtfsPatchLength> bytes;
};

struct NtfsIdentityOverlay {
    std::array<NtfsIdentityPatch, 2> patches;
};

[[nodiscard]] auto MakeNtfsPatch(
    const std::uint64_t sector_offset,
    const std::span<const std::byte> sector_prefix,
    const std::span<const std::byte, 8> serial) noexcept {
    auto checksum_input = std::array<std::byte, kNtfsChecksumOffset>{};
    CopyBytes(checksum_input, 0,
        sector_prefix.first(checksum_input.size()));
    CopyBytes(checksum_input, kNtfsSerialOffset, serial);

    auto checksum = std::uint32_t{};
    for (auto offset = std::size_t{};
        offset < checksum_input.size(); offset += sizeof(checksum)) {
        checksum += DeserializeObject<std::uint32_t>(checksum_input, offset);
    }

    auto result = NtfsIdentityPatch{
        .offset = sector_offset + kNtfsSerialOffset,
    };
    CopyBytes(result.bytes, 0, serial);
    SerializeObject(result.bytes, serial.size(), checksum);
    return result;
}

template <typename DeviceType>
[[nodiscard]] auto MakeNtfsIdentityOverlay(DeviceType &source)
    -> std::optional<NtfsIdentityOverlay> {
    auto primary = std::array<std::byte, kNtfsChecksumOffset>{};
    ReadExact(source, primary, 0, "the source volume boot record");
    if (std::memcmp(primary.data() + 3, "NTFS    ", 8) != 0) {
        return std::nullopt;
    }

    // NTFS identifies the volume with the 64-bit serial at offset 0x48. Its
    // checksum at 0x50 is the sum of all preceding 32-bit words. NTFS keeps
    // boot records in the first and last 512 bytes of the volume. These fields
    // and locations are documented by the Linux ntfs3 NTFS_BOOT definition
    // and ntfs_init_from_boot implementation.
    const auto backup_offset = source.length - kNtfsBootRecordLength;
    auto backup = std::array<std::byte, kNtfsChecksumOffset>{};
    ReadExact(source, backup, backup_offset,
        "the backup source volume boot record");
    const auto identity = NewGuid();
    auto serial = std::array<std::byte, 8>{};
    std::memcpy(serial.data(), std::addressof(identity), serial.size());
    return NtfsIdentityOverlay{
        .patches = {
            MakeNtfsPatch(0, primary, serial),
            MakeNtfsPatch(backup_offset, backup, serial),
        },
    };
}

[[gsl::suppress("26447",
    justification: "`wil::safe_cast_failfast` cannot throw.")]]
auto ApplyNtfsIdentityOverlay(
    const std::span<std::byte> output,
    const std::uint64_t source_offset,
    const std::optional<NtfsIdentityOverlay> &overlay) noexcept {
    if (!overlay) {
        return;
    }
    const auto source_end = source_offset + output.size();
    for (const auto &patch : overlay->patches) {
        const auto patch_end = patch.offset + patch.bytes.size();
        const auto overlap_begin = std::max(source_offset, patch.offset);
        const auto overlap_end = std::min(source_end, patch_end);
        if (overlap_begin >= overlap_end) {
            continue;
        }
        const auto count = overlap_end - overlap_begin;
        CopyBytes(output,
            wil::safe_cast_failfast<std::size_t>(
                overlap_begin - source_offset),
            std::span{patch.bytes}.subspan(
                wil::safe_cast_failfast<std::size_t>(
                    overlap_begin - patch.offset),
                wil::safe_cast_failfast<std::size_t>(count)));
    }
}

} // namespace devicefs::vhdx_detail

export namespace devicefs {

template <typename DeviceType>
class VhdxViewer {
  public:
    const std::uint64_t length;

    [[nodiscard]] static auto FromBlockDevice(DeviceType source) {
        auto layout = vhdx_detail::MakeLayout(source.length);
        auto ntfs_identity =
            vhdx_detail::MakeNtfsIdentityOverlay(source);
        return VhdxViewer{
            std::move(source), std::move(layout), std::move(ntfs_identity)};
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
        auto output = std::span{
            static_cast<std::byte *>(buffer), wanted};
        const auto copy_generated = [&](const std::span<const std::byte> source,
                                        const std::uint64_t position) {
            const auto source_position =
                wil::safe_cast_failfast<std::size_t>(position);
            const auto count = std::min<std::size_t>(
                output.size(), source.size() - source_position);
            vhdx_detail::CopyBytes(
                output, 0, source.subspan(source_position, count));
            // count is bounded by output.size(), which was constructed from
            // the ULONG wanted parameter.
            transferred = wil::safe_cast_failfast<ULONG>(count);
            return STATUS_SUCCESS;
        };

        if (offset < prefix_.size()) {
            return copy_generated(prefix_, offset);
        }

        const auto virtual_position = offset - prefix_.size();
        if (virtual_position < gpt_prefix_.size()) {
            return copy_generated(gpt_prefix_, virtual_position);
        }

        const auto source_begin = gpt_prefix_.size();
        const auto source_end = source_begin + source_.length;
        if (virtual_position < source_end) {
            const auto source_offset = virtual_position - source_begin;
            const auto source_wanted = wil::safe_cast_failfast<ULONG>(
                std::min(std::uint64_t{wanted},
                    source_.length - source_offset));
            const auto status = source_.Read(
                buffer, source_offset, source_wanted, transferred,
                observers...);
            // NTSTATUS >= 0 indicates success.
            // See <https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values>.
            if (status >= 0) {
                vhdx_detail::ApplyNtfsIdentityOverlay(
                    output.first(transferred), source_offset, ntfs_identity_);
            }
            return status;
        }

        const auto suffix_position = virtual_position - source_end;
        if (suffix_position < gpt_suffix_.size()) {
            return copy_generated(gpt_suffix_, suffix_position);
        }

        std::ranges::fill(output, std::byte{});
        transferred = wanted;
        return STATUS_SUCCESS;
    }

  private:
    VhdxViewer(
        DeviceType source,
        vhdx_detail::VhdxLayout layout,
        std::optional<vhdx_detail::NtfsIdentityOverlay> ntfs_identity) noexcept
        : length{layout.file_length},
          source_{std::move(source)},
          prefix_{std::move(layout.prefix)},
          gpt_prefix_{std::move(layout.gpt_prefix)},
          gpt_suffix_{std::move(layout.gpt_suffix)},
          ntfs_identity_{std::move(ntfs_identity)} {}

    DeviceType source_;
    std::vector<std::byte> prefix_;
    std::vector<std::byte> gpt_prefix_;
    std::vector<std::byte> gpt_suffix_;
    std::optional<vhdx_detail::NtfsIdentityOverlay> ntfs_identity_;
};

} // namespace devicefs
