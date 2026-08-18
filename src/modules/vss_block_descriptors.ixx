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
//
// This particular file contains code derived from the outstanding "libvshadow"
// product. The derived code is based on commit f5a73627 (July 14, 2026) from
// the following repository: <https://github.com/libyal/libvshadow>. The
// following copyright and licensing statement applies to the original
// unmodified code obtained from the libvshadow repository, from which this
// specific file is derived:
//
//     Copyright (C) 2011-2026, Joachim Metz <joachim.metz@gmail.com>
//
//     Refer to AUTHORS for acknowledgements.
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU Lesser General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU Lesser General Public License
//     along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// The above libvshadow licensing notice is reproduced verbatim, including its
// anomalous reference to the "GNU General Public License" in one place,
// even though the remainder of the notice refers to the "GNU Lesser General
// Public License".
//
// The full text of the upstream AUTHORS file referred to in the above
// libvshadow copyright notice is as follows:
//
//     Acknowledgements: libvshadow
//
//     Copyright (C) 2011-2026, Joachim Metz <joachim.metz@gmail.com>
//
// Although the original unmodified libvshadow code is licensed under
// LGPL-3.0-or-later, that license does not apply to this derivative work;
// instead, this derivative work is licensed solely under GPL-3.0-or-later.
// This choice of license is authorized by section 2(b) of the LGPL-3.0.

module;

#include <windows.h>
#include <winioctl.h>

// wil/resource.h uses these facilities without including their standard headers.
#include <algorithm>
#include <cstdint>

#include <wil/resource.h>
#include <wil/safecast.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module devicefs.vss_block_descriptors;

import devicefs.common;

export namespace devicefs::vss {

// libvshadow uses one 16-KiB unit for VSS metadata-block traversal and its
// descriptor trees. The caller also needs that unit when projecting raw byte
// offsets into conservative dirty blocks.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L51-L108
inline constexpr auto kBlockSize = std::uint64_t{0x4000};

// These are raw descriptor flag bits, not VSS snapshot-attribute flags from
// the Windows SDK. They are exported because the CLI and eventual supervisor
// must classify the records returned by ReadBlockDescriptors.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_definitions.h.in#L66-L73
inline constexpr auto kForwarderFlag = std::uint32_t{0x00000001};
inline constexpr auto kOverlayFlag = std::uint32_t{0x00000002};
inline constexpr auto kNotUsedFlag = std::uint32_t{0x00000004};

struct BlockDescriptor {
    std::uint64_t original_offset;
    std::uint64_t relative_offset;
    std::uint64_t store_offset;
    std::uint32_t flags;
    std::uint32_t bitmap;
};

struct StoreBlockDescriptors {
    GUID store_identifier;
    GUID snapshot_identifier;
    std::uint64_t volume_size;
    std::uint64_t list_block_count;
    std::vector<BlockDescriptor> descriptors;
};

// source exposes NTFS volume byte zero at offset zero. It may be a live volume
// handle path or a flat byte-for-byte image of that volume. The result preserves
// every nonempty raw descriptor's traversal order and multiplicity.
[[nodiscard]] auto ReadBlockDescriptors(
    std::wstring_view source, const GUID &snapshot_identifier)
    -> StoreBlockDescriptors;

} // namespace devicefs::vss

namespace {

// This module reproduces the VSS-descriptor behavior of libvshadow release
// 20260714, commit f5a7362713a04491ee78d36ebdcc1781950f3a75. Each source
// function-level cross-reference explains the behavior being preserved or the
// identified libvshadow defect being repaired; it is not merely provenance.

// Both libvshadow's signature preflight and volume-open path address the VSS
// volume header at raw NTFS byte offset 0x1e00.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_volume.c#L1033-L1046
constexpr auto kVssVolumeHeaderOffset = std::uint64_t{0x1e00};

// Keep these equal but independently named: they are the serialized widths of
// two different on-disk structures, not one reusable C++ object size.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/vshadow_volume.h#L32-L213
constexpr auto kNtfsVolumeHeaderSize = std::size_t{512};
constexpr auto kVssVolumeHeaderSize = std::size_t{512};

// The catalog header is a defined structure; catalog entries are separate
// fixed 128-byte records advanced by libvshadow_io_handle_read_catalog.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/vshadow_catalog.h#L32-L71
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L759-L875
constexpr auto kCatalogHeaderSize = std::size_t{128};
constexpr auto kCatalogEntrySize = std::size_t{128};

// These constants describe distinct store structures. In particular, the
// type-4 header and type-3 block header happen to have the same width but have
// different fields at +40, so neither may stand in for the other by name.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/vshadow_store.h#L32-L192
constexpr auto kStoreHeaderSize = std::size_t{128};
constexpr auto kStoreBlockHeaderSize = std::size_t{128};
constexpr auto kDescriptorSize = std::size_t{32};

// These are the private on-disk VSS record-type values consumed below.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_definitions.h.in#L77-L87
constexpr auto kVolumeHeaderRecordType = std::uint32_t{1};
constexpr auto kCatalogRecordType = std::uint32_t{2};
constexpr auto kStoreIndexRecordType = std::uint32_t{3};
constexpr auto kStoreHeaderRecordType = std::uint32_t{4};

// This byte sequence identifies the private VSS records parsed by this module;
// it is not one of the provider or snapshot GUID constants in the Windows SDK.
// https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L41-L45
constexpr auto kVssIdentifier = std::array<std::byte, 16>{
    std::byte{0x6b}, std::byte{0x87}, std::byte{0x08}, std::byte{0x38},
    std::byte{0x76}, std::byte{0xc1}, std::byte{0x48}, std::byte{0x4e},
    std::byte{0xb7}, std::byte{0xae}, std::byte{0x04}, std::byte{0x04},
    std::byte{0x6e}, std::byte{0x6c}, std::byte{0xc7}, std::byte{0x52},
};

[[noreturn]] auto ThrowOffsetError(
    const std::string_view description,
    const std::uint64_t offset,
    const std::uint64_t size,
    const std::uint64_t source_size) {
    throw std::runtime_error(std::format(
        "{} at offset 0x{:x} requested {} byte(s) from a {}-byte source",
        description, offset, size, source_size));
}

[[nodiscard]] auto StartsWithOrdinalIgnoreCase(
    const std::wstring_view value, const std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    const auto length = wil::safe_cast<int>(prefix.size());
    return CompareStringOrdinal(
        value.data(), length, prefix.data(), length, TRUE) == CSTR_EQUAL;
}

class RawSource {
  public:
    explicit RawSource(const std::wstring_view path) {
        auto normalized_path = std::wstring{path};
        constexpr auto volume_guid_prefix =
            std::wstring_view{LR"(\\?\Volume{)"};
        const auto volume_guid_end =
            normalized_path.find(L'}', volume_guid_prefix.size());
        const auto volume_guid_path = StartsWithOrdinalIgnoreCase(
                normalized_path, volume_guid_prefix) &&
            (volume_guid_end != std::wstring::npos) &&
            ((normalized_path.size() == volume_guid_end + 1) ||
                ((normalized_path.size() == volume_guid_end + 2) &&
                normalized_path.ends_with(L'\\')));
        // CreateFileW opens a volume itself, rather than its root directory,
        // only when the canonical volume-GUID path has no trailing backslash.
        if (volume_guid_path && normalized_path.ends_with(L'\\')) {
            normalized_path.pop_back();
        }
        constexpr auto global_root_device_prefix =
            std::wstring_view{LR"(\\?\GLOBALROOT\Device\)"};
        constexpr auto win32_device_prefix = std::wstring_view{LR"(\\.\)"};
        // A child below one of these namespaces can be an ordinary flat image.
        // Treat only the namespace object itself as a raw device so an
        // unsupported disk-length control code can still fall back to the
        // ordinary-file length path for a child.
        const auto raw_device_path = volume_guid_path ||
            (StartsWithOrdinalIgnoreCase(
                normalized_path, global_root_device_prefix) &&
                (normalized_path.find(L'\\',
                    global_root_device_prefix.size()) == std::wstring::npos)) ||
            (normalized_path.starts_with(win32_device_prefix) &&
                (normalized_path.find(L'\\', win32_device_prefix.size()) ==
                    std::wstring::npos));
        handle_.reset(CreateFileW(
              normalized_path.c_str(), GENERIC_READ,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
              nullptr, OPEN_EXISTING,
              FILE_FLAG_RANDOM_ACCESS | SECURITY_SQOS_PRESENT |
                  SECURITY_IDENTIFICATION,
              nullptr));
        if (!handle_) {
            WinError("could not open the VSS descriptor source");
        }

        // A successful disk-length query identifies a live raw device, whose
        // reads require sector handling. The ordinary-file fallback is for the
        // trusted acceptance fixture's exact flat copy of the same raw address
        // space; it is not an SVI-file transport.
        auto device_length = GET_LENGTH_INFORMATION{};
        auto returned = DWORD{};
        if (DeviceIoControl(handle_.get(), IOCTL_DISK_GET_LENGTH_INFO,
                nullptr, 0, &device_length, sizeof(device_length),
                &returned, nullptr)) {
            if (returned < sizeof(device_length)) {
                throw std::runtime_error(std::format(
                    "IOCTL_DISK_GET_LENGTH_INFO returned {} byte(s); "
                    "{} were required",
                    returned, sizeof(device_length)));
            }
            if (device_length.Length.QuadPart < 0) {
                throw std::runtime_error(
                    "the VSS descriptor source reported a negative length");
            }
            size_ = wil::safe_cast<std::uint64_t>(
                device_length.Length.QuadPart);

            // Windows treats volume handles as noncached. Query the sector size
            // so ReadRawExact can satisfy the device's offset, length, and buffer
            // alignment requirements while still exposing byte-exact reads to
            // the format parser.
            auto geometry = DISK_GEOMETRY{};
            returned = 0;
            if (!DeviceIoControl(handle_.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY,
                    nullptr, 0, &geometry, sizeof(geometry),
                    &returned, nullptr)) {
                WinError("could not obtain the VSS descriptor source geometry");
            }
            if (returned < sizeof(geometry)) {
                throw std::runtime_error(std::format(
                    "IOCTL_DISK_GET_DRIVE_GEOMETRY returned {} byte(s); "
                    "{} were required",
                    returned, sizeof(geometry)));
            }
            sector_size_ = geometry.BytesPerSector;
            if ((sector_size_ == 0) || ((size_ % sector_size_) != 0)) {
                throw std::runtime_error(std::format(
                    "the VSS descriptor source had invalid sector geometry "
                    "(length {}, bytes per sector {})",
                    size_, sector_size_));
            }

            // ValidateNtfsVolume reads the backup NTFS header in the final
            // sector, so request permission to read beyond the filesystem's
            // conventional volume boundary. Its bundled libcfile Windows layer
            // treats failure of this request as nonfatal; the subsequent exact
            // final-sector read is the authoritative test of whether the source
            // is usable and will report any actual access failure.
            // https://github.com/libyal/libcfile/blob/e2274ec/libcfile/libcfile_file.c#L983-L1017
            static_cast<void>(DeviceIoControl(handle_.get(),
                FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0,
                &returned, nullptr));
        } else {
            const auto device_error = GetLastError();
            if (raw_device_path ||
                ((device_error != ERROR_INVALID_FUNCTION) &&
                (device_error != ERROR_NOT_SUPPORTED) &&
                (device_error != ERROR_INVALID_PARAMETER))) {
                WinError("could not obtain the VSS descriptor source length",
                    ExplicitWin32Error{device_error});
            }

            auto file_size = LARGE_INTEGER{};
            if (!GetFileSizeEx(handle_.get(), &file_size)) {
                WinError("could not obtain the VSS descriptor image length");
            }
            if (file_size.QuadPart < 0) {
                throw std::runtime_error(
                    "the VSS descriptor image reported a negative length");
            }
            size_ = wil::safe_cast<std::uint64_t>(file_size.QuadPart);
        }

        if (size_ == 0) {
            throw std::runtime_error("the VSS descriptor source is empty");
        }
    }

    [[nodiscard]] auto Read(
        const std::uint64_t offset, const std::size_t size) {
        auto result = std::vector<std::byte>(size);
        ReadExact(offset, result);
        return result;
    }

  private:
    void ReadExact(
        const std::uint64_t offset, std::span<std::byte> destination) {
        const auto requested =
            wil::safe_cast<std::uint64_t>(destination.size());
        if ((offset > size_) || (requested > (size_ - offset))) {
            ThrowOffsetError("a VSS metadata read was out of bounds",
                offset, requested, size_);
        }
        if (sector_size_ != 0) {
            ReadRawExact(offset, destination);
            return;
        }

        Seek(offset);
        ReadCurrentExact(offset, destination);
    }

    void Seek(const std::uint64_t offset) {
        auto position = LARGE_INTEGER{
            .QuadPart = wil::safe_cast<LONGLONG>(offset),
        };
        if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN)) {
            WinError("could not seek to VSS metadata at offset 0x{:x}", offset);
        }
    }

    void ReadCurrentExact(
        const std::uint64_t offset, std::span<std::byte> destination) {
        const auto requested = wil::safe_cast<DWORD>(destination.size());
        auto completed = DWORD{};
        if (!ReadFile(handle_.get(), destination.data(), requested,
                &completed, nullptr)) {
            WinError("could not read VSS metadata at offset 0x{:x}", offset);
        }
        if (completed != requested) {
            ThrowOffsetError("a VSS metadata read completed short",
                offset, requested, size_);
        }
    }

    void ReadRawExact(
        const std::uint64_t offset, const std::span<std::byte> destination) {
        if (destination.empty()) {
            return;
        }
        const auto end = offset + destination.size();
        const auto raw_offset = offset - (offset % sector_size_);
        auto raw_end = end;
        if (const auto remainder = end % sector_size_; remainder != 0) {
            raw_end += sector_size_ - remainder;
        }
        const auto raw_size = raw_end - raw_offset;
        if (!std::in_range<DWORD>(raw_size)) {
            ThrowOffsetError("an aligned VSS metadata read was too large",
                raw_offset, raw_size, size_);
        }

        auto storage = wil::unique_virtualalloc_ptr<std::byte>{
            static_cast<std::byte *>(VirtualAlloc(nullptr,
                wil::safe_cast<SIZE_T>(raw_size),
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE))};
        if (!storage) {
            WinError("could not allocate an aligned VSS metadata buffer");
        }
        if ((std::bit_cast<std::uintptr_t>(storage.get()) %
                sector_size_) != 0) {
            throw std::runtime_error(
                "the VSS metadata buffer did not meet volume alignment");
        }

        Seek(raw_offset);
        auto completed = DWORD{};
        if (!ReadFile(handle_.get(), storage.get(),
                wil::safe_cast<DWORD>(raw_size), &completed, nullptr)) {
            WinError("could not read aligned VSS metadata at offset 0x{:x}",
                raw_offset);
        }
        if (completed != raw_size) {
            ThrowOffsetError("an aligned VSS metadata read completed short",
                raw_offset, raw_size, size_);
        }
        std::memcpy(destination.data(),
            storage.get() +
                wil::safe_cast<std::size_t>(offset - raw_offset),
            destination.size());
    }

    wil::unique_hfile handle_;
    std::uint64_t size_ = 0;
    DWORD sector_size_ = 0;
};

// ReadField decodes integer fields and serialized Windows GUIDs directly into
// their native object representations, so every supported field requires the
// little-endian object representation used by Windows.
static_assert(std::endian::native == std::endian::little);

template <class Field>
    requires (std::unsigned_integral<Field> || std::same_as<Field, GUID>)
[[nodiscard]] auto ReadField(
    const std::span<const std::byte> data, const std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<Field>);
    if ((offset > data.size()) ||
        (sizeof(Field) > (data.size() - offset))) {
        throw std::runtime_error(
            "an internal VSS field read was out of bounds");
    }
    auto encoded = std::array<std::byte, sizeof(Field)>{};
    std::ranges::copy(data.subspan(offset, encoded.size()), encoded.begin());
    return std::bit_cast<Field>(encoded);
}

static_assert(sizeof(GUID) == 16);

[[nodiscard]] auto HasIdentifier(
    const std::span<const std::byte> data, const std::size_t length) {
    return (length <= data.size()) &&
        (length <= kVssIdentifier.size()) &&
        (std::memcmp(data.data(), kVssIdentifier.data(), length) == 0);
}

struct NtfsHeader {
    std::uint16_t bytes_per_sector;
    std::uint64_t volume_size;
};

[[nodiscard]] auto ParseNtfsHeader(
    const std::span<const std::byte> data) {
    // libvshadow_ntfs_volume_header_read_data establishes that the source is an
    // NTFS raw address space and supplies the sector count and sector size used
    // to locate its backup header. Those are the only NTFS fields consumed by
    // this descriptor parser; MFT, index, and cluster-layout fields protect no
    // subsequent read and are intentionally not copied from the generic NTFS
    // decoder.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_ntfs_volume_header.c#L140-L620
    constexpr auto signature = std::array{
        std::byte{'N'}, std::byte{'T'}, std::byte{'F'}, std::byte{'S'},
        std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '},
    };
    if (std::memcmp(data.data() + 3,
            signature.data(), signature.size()) != 0) {
        throw std::runtime_error(
            "the VSS descriptor source is not an NTFS volume");
    }

    const auto bytes_per_sector =
        ReadField<std::uint16_t>(data, 11);
    if ((bytes_per_sector != 256) && (bytes_per_sector != 512) &&
        (bytes_per_sector != 1024) && (bytes_per_sector != 2048) &&
        (bytes_per_sector != 4096)) {
        throw std::runtime_error(std::format(
            "the NTFS volume header contained unsupported sector size {}",
            bytes_per_sector));
    }

    const auto sectors = ReadField<std::uint64_t>(data, 40);
    // libvshadow_ntfs_volume_header_read_data computes
    // (sectors + 1) * bytes_per_sector, but its published upper-bound
    // expression permits that calculation to overflow. This subtraction-form
    // bound is the deliberate arithmetic-safety repair: it accepts every input
    // for which the mathematical result is representable by std::uint64_t,
    // without allowing unsigned wraparound.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_ntfs_volume_header.c#L579-L591
    if (sectors > ((std::numeric_limits<std::uint64_t>::max() /
            bytes_per_sector) - 1)) {
        throw std::runtime_error(std::format(
            "the NTFS volume header was invalid: sector-count value {} "
            "with {} bytes per sector cannot form a 64-bit volume size",
            sectors, bytes_per_sector));
    }
    return NtfsHeader{
        .bytes_per_sector = bytes_per_sector,
        .volume_size = (sectors + 1) * bytes_per_sector,
    };
}

auto ValidateNtfsVolume(RawSource &source) {
    // libvshadow_volume_open_read_ntfs_volume_headers requires the primary and
    // backup NTFS headers to describe the same volume size before opening the
    // VSS catalog. The equality protects the physical-offset coordinate system
    // consumed by ReadCatalog, ReadStoreHeaders, and ReadDescriptorList.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_volume.c#L1199-L1290
    const auto primary = ParseNtfsHeader(
        source.Read(0, kNtfsVolumeHeaderSize));
    const auto backup = ParseNtfsHeader(source.Read(
        primary.volume_size - primary.bytes_per_sector,
        kNtfsVolumeHeaderSize));
    if (backup.volume_size != primary.volume_size) {
        throw std::runtime_error(std::format(
            "the primary NTFS header reported volume size {} and {}-byte "
            "sectors, but the backup header reported volume size {} and "
            "{}-byte sectors",
            primary.volume_size, primary.bytes_per_sector,
            backup.volume_size, backup.bytes_per_sector));
    }
}

auto CheckSignedMetadataOffset(
    const std::uint64_t offset, const std::string_view description) {
    if (!std::in_range<std::int64_t>(offset)) {
        throw std::runtime_error(std::format(
            "{} 0x{:x} was invalid because it does not fit in the signed "
            "64-bit VSS metadata-offset domain",
            description, offset));
    }
}

[[nodiscard]] auto ReadCatalogOffset(
    const std::span<const std::byte> volume_header) {
    // libvshadow_io_handle_read_volume_header_data checks the first eight
    // identifier bytes before trusting version, record type, or the physical
    // catalog pointer at +48. Keep that local decoder boundary even though the
    // caller first applies the stronger complete-format signature check.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L358-L607
    if (!HasIdentifier(volume_header, 8)) {
        throw std::runtime_error(
            "the VSS volume header identifier was invalid");
    }
    const auto version =
        ReadField<std::uint32_t>(volume_header, 16);
    const auto record_type =
        ReadField<std::uint32_t>(volume_header, 20);
    if ((version != 1) && (version != 2)) {
        throw std::runtime_error(std::format(
            "the VSS volume header version {} is unsupported", version));
    }
    if (record_type != kVolumeHeaderRecordType) {
        throw std::runtime_error(std::format(
            "the VSS volume header record type {} is unsupported", record_type));
    }
    const auto catalog_offset =
        ReadField<std::uint64_t>(volume_header, 48);
    // libvshadow_io_handle_read_volume_header_data casts this unsigned field to
    // off64_t without checking it. Requiring std::int64_t representability is a
    // deliberate repair: it prevents a high-bit physical pointer from being
    // mistaken for a negative "catalog absent" result.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L446-L607
    CheckSignedMetadataOffset(
        catalog_offset, "the VSS catalog root offset");
    return catalog_offset;
}

struct CatalogType3 {
    GUID store_identifier;
    std::uint64_t list_offset;
    std::uint64_t header_offset;
};

struct CatalogStore {
    GUID store_identifier;
    std::uint64_t volume_size;
    std::uint64_t list_offset;
    std::uint64_t header_offset;
    bool has_in_volume_data;
    GUID snapshot_identifier{};
};

auto VisitMetadataBlock(
    std::unordered_set<std::uint64_t> &visited,
    const std::uint64_t offset,
    const std::string_view description) {
    // libvshadow_io_handle_check_if_block_first_read records the containing
    // 16-KiB block-tree leaf, rather than the literal pointer. Use the same key
    // so a chain that aliases an already visited block through a different
    // interior offset is rejected instead of looping; no alignment rule is
    // inferred from that behavior.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_block_tree_node.c#L442-L610
    if (!visited.insert(offset / devicefs::vss::kBlockSize).second) {
        throw std::runtime_error(std::format(
            "{} repeated the 16-KiB metadata block containing offset 0x{:x}",
            description, offset));
    }
}

[[nodiscard]] auto ReadCatalog(RawSource &source, std::uint64_t offset) {
    auto stores = std::vector<CatalogStore>{};
    auto visited = std::unordered_set<std::uint64_t>{};

    // libvshadow_io_handle_read_catalog follows the root and +40 links,
    // libvshadow_io_handle_read_catalog_header_data validates each block, and
    // libvshadow_store_descriptor_read_catalog_entry accepts only entry types
    // 0 through 3. Those are the rules needed to discover stores. The +24 and
    // +32 header fields are deliberately excluded from control flow because
    // libvshadow_io_handle_read_catalog_header_data only prints them in
    // diagnostic builds and does not make them acceptance conditions.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_io_handle.c#L613-L879
    while (offset != 0) {
        VisitMetadataBlock(visited, offset, "the VSS catalog chain");
        const auto block = source.Read(offset, devicefs::vss::kBlockSize);
        const auto data = std::span<const std::byte>{block};
        if (!HasIdentifier(data, 8)) {
            throw std::runtime_error(std::format(
                "the VSS catalog block at 0x{:x} had an invalid identifier",
                offset));
        }
        const auto version = ReadField<std::uint32_t>(data, 16);
        const auto record_type = ReadField<std::uint32_t>(data, 20);
        if (version != 1) {
            throw std::runtime_error(std::format(
                "the VSS catalog block at 0x{:x} had unsupported version {}",
                offset, version));
        }
        if (record_type != kCatalogRecordType) {
            throw std::runtime_error(std::format(
                "the VSS catalog block at 0x{:x} had unsupported record type {}",
                offset, record_type));
        }

        const auto next = ReadField<std::uint64_t>(data, 40);
        CheckSignedMetadataOffset(next, "the VSS catalog next offset");

        for (auto entry_offset = kCatalogHeaderSize;
             entry_offset < data.size(); entry_offset += kCatalogEntrySize) {
            const auto entry = data.subspan(entry_offset, kCatalogEntrySize);
            const auto type = ReadField<std::uint64_t>(entry, 0);
            if ((type == 0) || (type == 1)) {
                continue;
            }
            if (type == 2) {
                stores.push_back(CatalogStore{
                    .store_identifier = ReadField<GUID>(entry, 16),
                    .volume_size = ReadField<std::uint64_t>(entry, 8),
                    .list_offset = 0,
                    .header_offset = 0,
                    .has_in_volume_data = false,
                });
                continue;
            }
            if (type == 3) {
                const auto type3 = CatalogType3{
                    .store_identifier = ReadField<GUID>(entry, 16),
                    .list_offset = ReadField<std::uint64_t>(entry, 8),
                    .header_offset = ReadField<std::uint64_t>(entry, 32),
                };
                CheckSignedMetadataOffset(
                    type3.list_offset, "a VSS descriptor-list offset");
                CheckSignedMetadataOffset(
                    type3.header_offset, "a VSS store-header offset");

                // The format pairs type-2 and type-3 entries by the store GUID
                // and assumes the type-2 entry appears first; the records need
                // not be adjacent or ordered by age. Search all type-2 entries
                // seen so far and let a later type-3 overwrite the same store's
                // physical offsets. libvshadow's creation-time sorting establishes
                // reconstruction chronology, which this module does not need; its
                // last-entry pointer is only an association shortcut, which a
                // direct GUID search makes unnecessary.
                // https://github.com/libyal/libvshadow/blob/f5a7362/documentation/Volume%20Shadow%20Snapshot%20(VSS)%20format.asciidoc#L185-L269
                //
                // The pinned catalog-entry decoder fails to copy the type-3
                // GUID before trying to compare it. Reading bytes +16 above is
                // the direct repair of that defect.
                // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_store_descriptor.c#L581-L837
                const auto matching = std::ranges::find_if(stores,
                    [&](const auto &store) {
                        return InlineIsEqualGUID(store.store_identifier,
                            type3.store_identifier) != FALSE;
                    });
                if (matching != stores.end()) {
                    matching->list_offset = type3.list_offset;
                    matching->header_offset = type3.header_offset;
                    matching->has_in_volume_data = true;
                }
                continue;
            }
            throw std::runtime_error(std::format(
                "the VSS catalog contained unsupported entry type {}", type));
        }
        offset = next;
    }
    return stores;
}

struct StoreBlockHeader {
    std::uint32_t record_type;
    std::uint64_t next_offset;
};

[[nodiscard]] auto ParseStoreBlockHeader(
    const std::span<const std::byte> data,
    const std::uint64_t physical_offset) {
    // libvshadow_store_block_read_header_data checks all 16 identifier bytes
    // and version 1, then decodes the record type and bytes +40. The descriptor-
    // list caller treats +40 as its next link; the type-4 store-header caller
    // decodes but does not use it. Neither caller turns the diagnostic
    // relative/current fields into acceptance conditions.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_store_block.c#L261-L437
    if (!HasIdentifier(data, kVssIdentifier.size())) {
        throw std::runtime_error(std::format(
            "the VSS store block at 0x{:x} had an invalid identifier",
            physical_offset));
    }
    const auto version = ReadField<std::uint32_t>(data, 16);
    if (version != 1) {
        throw std::runtime_error(std::format(
            "the VSS store block at 0x{:x} had unsupported version {}",
            physical_offset, version));
    }
    return StoreBlockHeader{
        .record_type = ReadField<std::uint32_t>(data, 20),
        .next_offset = ReadField<std::uint64_t>(data, 40),
    };
}

auto ReadStoreHeaders(RawSource &source, std::vector<CatalogStore> &stores) {
    // libvshadow_volume_open_read invokes
    // libvshadow_store_descriptor_read_store_header for every in-volume store
    // before vshadowinfo can enumerate any of them. Preserve the eager common
    // block-header and record-type validation, then retain the fixed copy ID
    // needed for caller selection. Its following machine-description strings
    // are unrelated to locating or interpreting the descriptor list and are
    // deliberately omitted.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_volume.c#L1092-L1160
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_store_descriptor.c#L868-L995
    for (auto &store : stores) {
        if (!store.has_in_volume_data) {
            continue;
        }
        const auto block = source.Read(
            store.header_offset, devicefs::vss::kBlockSize);
        const auto data = std::span<const std::byte>{block};
        const auto header = ParseStoreBlockHeader(data, store.header_offset);
        if (header.record_type != kStoreHeaderRecordType) {
            throw std::runtime_error(std::format(
                "the VSS store header at 0x{:x} had unsupported record type {}",
                store.header_offset, header.record_type));
        }

        store.snapshot_identifier =
            ReadField<GUID>(data, kStoreHeaderSize + 16);
    }
}

struct NormalizedDescriptor {
    devicefs::vss::BlockDescriptor value;
    std::shared_ptr<NormalizedDescriptor> overlay;
};

class DescriptorState {
  public:
    explicit DescriptorState(const std::uint64_t volume_size)
        : volume_size_{volume_size} {
        // libvshadow_store_descriptor_read_block_descriptors passes the
        // selected type-2 catalog entry's volume size to
        // libvshadow_block_tree_initialize, whose root node rejects zero and
        // values not representable by std::int64_t. Express that domain
        // directly before using any descriptor offset as a tree key.
        // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_block_tree_node.c#L34-L88
        if ((volume_size_ == 0) ||
            !std::in_range<std::int64_t>(volume_size_)) {
            throw std::runtime_error(std::format(
                "the selected VSS catalog entry reported invalid volume "
                "size {}; the descriptor-tree domain must be nonzero and "
                "fit in a signed 64-bit value",
                volume_size_));
        }
    }

    void Insert(const devicefs::vss::BlockDescriptor &raw) {
        // libvshadow_store_descriptor_read_store_block_list calls
        // libvshadow_block_tree_insert before appending a raw descriptor to the
        // list printed by vshadowinfo. Reproduce that forward/reverse state
        // machine so malformed descriptor sequences fail at the same point,
        // while two keyed maps replace libyal's generic multi-level tree.
        // std::shared_ptr preserves the source algorithm's dual-tree object
        // lifetime without copying its manual index and conditional-free
        // bookkeeping.
        // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_block_tree.c#L567-L1089
        if ((raw.flags & devicefs::vss::kNotUsedFlag) != 0) {
            return;
        }

        auto resolved_original = raw.original_offset;
        if ((raw.flags & devicefs::vss::kOverlayFlag) == 0) {
            const auto reverse = reverse_.find(Key(raw.original_offset,
                "a VSS reverse descriptor offset"));
            if (reverse != reverse_.end()) {
                const auto reverse_descriptor = reverse->second;
                resolved_original =
                    reverse_descriptor->value.original_offset;
                const auto erased = reverse_.erase(
                    Key(reverse_descriptor->value.relative_offset,
                        "a VSS reverse descriptor target"));
                if (erased != 1) {
                    throw std::runtime_error(
                        "the VSS reverse descriptor state was inconsistent");
                }
            }
        }

        if (((raw.flags & devicefs::vss::kForwarderFlag) != 0) &&
            (resolved_original == raw.relative_offset)) {
            return;
        }

        auto normalized = std::make_shared<NormalizedDescriptor>(
            NormalizedDescriptor{
                .value = raw,
                .overlay = nullptr,
            });
        normalized->value.original_offset = resolved_original;

        const auto forward_key = Key(
            normalized->value.original_offset,
            "a VSS forward descriptor offset");
        const auto existing = forward_.find(forward_key);
        if (existing == forward_.end()) {
            forward_.emplace(forward_key, normalized);
        } else if ((normalized->value.flags &
                devicefs::vss::kOverlayFlag) != 0) {
            auto overlay = ((existing->second->value.flags &
                    devicefs::vss::kOverlayFlag) != 0)
                ? existing->second
                : existing->second->overlay;
            if (overlay) {
                overlay->value.bitmap |= normalized->value.bitmap;
            } else {
                existing->second->overlay = normalized;
            }
            return;
        } else {
            const auto replaced = existing->second;
            existing->second = normalized;
            if ((replaced->value.flags &
                    devicefs::vss::kOverlayFlag) != 0) {
                if (replaced->overlay) {
                    throw std::runtime_error(
                        "the VSS overlay descriptor state was inconsistent");
                }
                normalized->overlay = replaced;
            } else {
                normalized->overlay = std::move(replaced->overlay);
            }
        }

        if ((normalized->value.flags & devicefs::vss::kForwarderFlag) != 0) {
            reverse_[Key(normalized->value.relative_offset,
                "a VSS forwarder target")] = normalized;
        }
    }

  private:
    using Map = std::unordered_map<
        std::uint64_t, std::shared_ptr<NormalizedDescriptor>>;

    [[nodiscard]] auto Key(
        const std::uint64_t offset,
        const std::string_view description) const -> std::uint64_t {
        if (offset >= volume_size_) {
            throw std::runtime_error(std::format(
                "{} 0x{:x} was outside the {}-byte VSS volume",
                description, offset, volume_size_));
        }
        return offset / devicefs::vss::kBlockSize;
    }

    std::uint64_t volume_size_;
    Map forward_;
    Map reverse_;
};

[[nodiscard]] auto ReadDescriptorList(
    RawSource &source, const CatalogStore &store) {
    auto result = devicefs::vss::StoreBlockDescriptors{
        .store_identifier = store.store_identifier,
        .snapshot_identifier = store.snapshot_identifier,
        .volume_size = store.volume_size,
        .list_block_count = 0,
        .descriptors = {},
    };
    auto state = DescriptorState{store.volume_size};
    auto visited = std::unordered_set<std::uint64_t>{};
    auto offset = store.list_offset;

    // libvshadow_store_descriptor_read_block_descriptors also reads VSS store
    // bitmaps and block-range lists for reconstructed snapshot I/O. This module
    // intentionally omits those unrelated chains: the change-map consumer needs
    // the raw descriptor list, not libvshadow's snapshot reader. Consequently,
    // this list-local visited set detects descriptor-list cycles and aliases,
    // but does not reject a list block merely because an omitted bitmap or range
    // chain refers to the same physical 16-KiB block.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_store_descriptor.c#L1948-L2215
    //
    // libvshadow_store_descriptor_read_store_block_list scans all 508 slots in
    // every linked block, and libvshadow_block_descriptor_read_data treats only
    // an entirely zero 32-byte record as empty and rejects a forwarder with a
    // nonzero store offset. Preserve that scan and append each accepted raw
    // five-field record in traversal order, because that is the list exposed by
    // vshadowinfo even when normalization ignores NOT_USED or self-forwarders.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_store_descriptor.c#L1569-L1752
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_block_descriptor.c#L313-L450
    while (offset != 0) {
        VisitMetadataBlock(visited, offset, "the VSS descriptor-list chain");
        const auto block = source.Read(offset, devicefs::vss::kBlockSize);
        const auto data = std::span<const std::byte>{block};
        const auto header = ParseStoreBlockHeader(data, offset);
        if (header.record_type != kStoreIndexRecordType) {
            throw std::runtime_error(std::format(
                "the VSS descriptor-list block at 0x{:x} had unsupported record type {}",
                offset, header.record_type));
        }
        ++result.list_block_count;

        for (auto descriptor_offset = kStoreBlockHeaderSize;
             descriptor_offset <= (data.size() - kDescriptorSize);
             descriptor_offset += kDescriptorSize) {
            const auto bytes = data.subspan(
                descriptor_offset, kDescriptorSize);
            if (std::ranges::all_of(bytes, [](const auto value) {
                    return value == std::byte{};
                })) {
                continue;
            }
            const auto descriptor = devicefs::vss::BlockDescriptor{
                .original_offset =
                    ReadField<std::uint64_t>(bytes, 0),
                .relative_offset =
                    ReadField<std::uint64_t>(bytes, 8),
                .store_offset =
                    ReadField<std::uint64_t>(bytes, 16),
                .flags = ReadField<std::uint32_t>(bytes, 24),
                .bitmap = ReadField<std::uint32_t>(bytes, 28),
            };
            if (((descriptor.flags & devicefs::vss::kForwarderFlag) != 0) &&
                (descriptor.store_offset != 0)) {
                throw std::runtime_error(std::format(
                    "the VSS forwarder descriptor at 0x{:x} had nonzero "
                    "store offset 0x{:x} (original 0x{:x}, relative 0x{:x}, "
                    "flags 0x{:08x})",
                    offset + descriptor_offset, descriptor.store_offset,
                    descriptor.original_offset, descriptor.relative_offset,
                    descriptor.flags));
            }
            state.Insert(descriptor);
            result.descriptors.push_back(descriptor);
        }
        offset = header.next_offset;
    }
    return result;
}

} // namespace

namespace devicefs::vss {

auto ReadBlockDescriptors(
    const std::wstring_view source_path,
    const GUID &snapshot_identifier) -> StoreBlockDescriptors {
    auto source = RawSource{source_path};
    const auto signature = source.Read(
        kVssVolumeHeaderOffset, kVssIdentifier.size());

    // info_handle_open_input performs the 16-byte
    // libvshadow_check_volume_signature_file_io_handle preflight before calling
    // libvshadow_volume_open_file_io_handle. The complete identifier establishes
    // that this is the known VSS format before any embedded physical pointer is
    // trusted. ReadCatalogOffset separately mirrors the eight-byte header check
    // in libvshadow_io_handle_read_volume_header_data; that weaker local check
    // is not a substitute for ReadBlockDescriptors recognizing the complete
    // format identifier before calling it.
    // https://github.com/libyal/libvshadow/blob/f5a7362/vshadowtools/info_handle.c#L485-L506
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_support.c#L324-L418
    if (!HasIdentifier(signature, kVssIdentifier.size())) {
        throw std::runtime_error("the VSS volume signature was not present");
    }
    ValidateNtfsVolume(source);

    // libvshadow_volume_open_read reads the complete header only after the
    // signature and both NTFS headers have passed. The preflight establishes
    // only the format identity; this fresh, fully decoded header is the source
    // of the catalog pointer used by ReadCatalogOffset.
    // https://github.com/libyal/libvshadow/blob/f5a7362/libvshadow/libvshadow_volume.c#L994-L1067
    const auto volume_header = source.Read(
        kVssVolumeHeaderOffset, kVssVolumeHeaderSize);
    const auto catalog_offset = ReadCatalogOffset(volume_header);
    auto stores = ReadCatalog(source, catalog_offset);
    ReadStoreHeaders(source, stores);

    const CatalogStore *selected = nullptr;
    for (const auto &store : stores) {
        if (!store.has_in_volume_data ||
            (InlineIsEqualGUID(store.snapshot_identifier,
                snapshot_identifier) == FALSE)) {
            continue;
        }
        if (selected != nullptr) {
            // This is the caller's fail-closed selection policy, not a VSS
            // format uniqueness assertion made by libvshadow.
            throw std::runtime_error(
                "the requested VSS snapshot ID matched multiple stores");
        }
        selected = &store;
    }
    if (selected == nullptr) {
        throw std::runtime_error(
            "the requested VSS snapshot ID was not found in the catalog");
    }
    return ReadDescriptorList(source, *selected);
}

} // namespace devicefs::vss
