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

#include <sal.h>
#include <windows.h>
#include <sddl.h>
#include <winioctl.h>

// wil/stl.h uses symbols defined in <algorithm> without including it.
#include <algorithm>

#include "winfsp_compat.h"
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>

#include <cstddef>

export module devicefs.filesystem;

import std;
import devicefs.common;
import devicefs.stream_writer;

#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA || DEVICEFS_MEASURE_READ_PATH
import devicefs.filesystem_measurement;
#endif

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose `std::start_lifetime_as`.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
} // namespace std
#endif

namespace {

constexpr auto kAdvertisedSectorSize = UINT16{512};
constexpr auto kDefaultStopEvent = std::wstring_view(L"Local\\devicefs-stop");
constexpr auto kFileSystemName = std::wstring_view(L"DEVICEFS");
constexpr auto kMeasureFreeClusterData = DEVICEFS_MEASURE_FREE_CLUSTER_DATA != 0;
static_assert(kFileSystemName.size() + 1 <=
    std::size(FSP_FSCTL_VOLUME_PARAMS{}.FileSystemName),
    "The filesystem name and its terminator must fit the WinFsp volume parameters.");
constexpr auto kMaxNameLength = UINT16{255};
constexpr auto kMaxDirectoryInfoSize =
    sizeof(FSP_FSCTL_DIR_INFO) + kMaxNameLength * sizeof(wchar_t);
static_assert(std::in_range<decltype(FSP_FSCTL_DIR_INFO::Size)>(kMaxDirectoryInfoSize),
    "The largest directory record must fit in the FSP_FSCTL_DIR_INFO::Size field.");
constexpr auto kMaxMountPrefixLength = std::size(FSP_FSCTL_VOLUME_PARAMS{}.Prefix) - 1;
constexpr auto kVolumeLabel = std::wstring_view(L"DEVICEFS");
static_assert(kVolumeLabel.size() <= std::size(FSP_FSCTL_VOLUME_INFO{}.VolumeLabel),
    "The volume label must fit the WinFsp volume information buffer.");
constexpr auto kRootInfo = FSP_FSCTL_FILE_INFO{
    .FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY,
    .IndexNumber = 1,
};
constexpr auto kWriteAccess = UINT32{FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
    FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER};

struct Mapping {
    std::wstring name;
    std::wstring device;
};

struct Mount {
    bool network = false;
    std::wstring value;
};

struct Options {
    Mount mount;
    std::wstring read_user;
    std::wstring stop_event{kDefaultStopEvent};
    std::vector<Mapping> mappings;
    bool cache = false;
    bool extended_dasd = true;
    bool synthetic_free_clusters = false;
    bool help = false;
};

auto FurtherHardenProcess() {
    auto child_processes = PROCESS_MITIGATION_CHILD_PROCESS_POLICY{};
    child_processes.NoChildProcessCreation = 1;
    if (!SetProcessMitigationPolicy(
            ProcessChildProcessPolicy, &child_processes, sizeof(child_processes))) {
        WinError("could not prohibit child process creation");
    }
}

[[nodiscard]] auto Lowercase(const wil::zwstring_view value) {
    const auto output_size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.c_str(), -1, nullptr, 0, nullptr, nullptr, 0);
    if (output_size == 0) {
        WinError("could not lowercase a virtual filename");
    }

    auto result = std::wstring(output_size, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            value.c_str(), -1, result.data(), output_size, nullptr, nullptr, 0)) {
        WinError("could not lowercase a virtual filename");
    }
    result.pop_back();
    return result;
}

auto Usage(std::ostream &output) noexcept {
    devicefs::WriteToStream(output,
        L"Usage: devicefs --mount TARGET --read-user USER"
        L" --map NAME DEVICE [--map NAME DEVICE ...] [OPTIONS]\n\n"
        L"Options:\n"
        L"  --mount TARGET             Drive letter, directory, or network prefix\n"
        L"  --read-user USER           User granted read access\n"
        L"  --map NAME DEVICE          Virtual filename and block device (repeatable)\n"
        L"  --stop-event NAME          Named shutdown event (default: {})\n"
        L"  --cache                    Enable file-data caching (requires read-only volumes)\n"
        L"  --no-extended-dasd-io      Do not issue FSCTL_ALLOW_EXTENDED_DASD_IO\n"
        L"  --synthetic-free-clusters  Return zeros for free clusters on read-only NTFS volumes\n"
        L"  -h, --help                 Show this help\n\n"
        L"Example:\n"
        L"  devicefs --mount X: --read-user 'pbs-vss' `\n"
        L"    --map C.img '\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy12'\n",
        kDefaultStopEvent);
}

[[nodiscard]] auto ParseArgs(const std::span<const wchar_t *const> args) {
    auto result = Options{};
    const auto next = [&](auto &i) {
        if (++i == args.size()) {
            throw std::invalid_argument(
                std::format("missing value after argument {}", i - 1));
        }
        return args[i];
    };

    for (auto i = 1uz; i < args.size(); ++i) {
        const auto arg = std::wstring_view(args[i]);
        if ((arg == L"-h") || (arg == L"--help")) {
            result.help = true;
        } else if (arg == L"--mount") {
            result.mount.value = next(i);
        } else if (arg == L"--read-user") {
            result.read_user = next(i);
        } else if (arg == L"--stop-event") {
            result.stop_event = next(i);
        } else if (arg == L"--map") {
            const auto *const name = next(i);
            const auto *const device = next(i);
            result.mappings.push_back({.name = name, .device = device});
        } else if (arg == L"--cache") {
            result.cache = true;
        } else if (arg == L"--no-extended-dasd-io") {
            result.extended_dasd = false;
        } else if (arg == L"--synthetic-free-clusters") {
            result.synthetic_free_clusters = true;
        } else {
            throw std::invalid_argument(std::format("unknown option at argument {}", i));
        }
    }

    if (result.stop_event.empty()) {
        throw std::invalid_argument("--stop-event must not be empty");
    }
    if (!result.help &&
        ((result.mount.value.empty()) || (result.read_user.empty()) ||
            (result.mappings.empty()))) {
        throw std::invalid_argument(
            "--mount, --read-user, and at least one --map are required");
    }

    auto names = std::unordered_set<std::wstring>{};
    for (auto i = 0uz; i < result.mappings.size(); ++i) {
        const auto &name = result.mappings[i].name;
        if ((name.empty()) || (name == L".") || (name == L"..") ||
            (name.size() > kMaxNameLength) ||
            (name.find_first_of(L"/\\:") != std::wstring_view::npos)) {
            throw std::invalid_argument(std::format("invalid filename in --map #{}", i + 1));
        }
        if (!names.emplace(Lowercase(name)).second) {
            throw std::invalid_argument(
                std::format("duplicate filename in --map #{}", i + 1));
        }
    }

    if ((result.mount.value.size() == 2) && (result.mount.value[1] == L':')) {
        result.mount.value = std::format(L"\\\\.\\{}", result.mount.value);
    }
    const auto device_path = (result.mount.value.starts_with(L"\\\\?\\")) ||
        (result.mount.value.starts_with(L"\\\\.\\"));
    result.mount.network = !device_path && (result.mount.value.starts_with(L"\\"));
    if ((result.mount.network) && (result.mount.value.starts_with(L"\\\\"))) {
        result.mount.value.erase(result.mount.value.begin());
    }
    if ((result.mount.network) &&
        (result.mount.value.size() > kMaxMountPrefixLength)) {
        throw std::invalid_argument("UNC mount prefix is too long");
    }
    return result;
}

auto CheckNt(const NTSTATUS status, const wil::zstring_view operation) {
    if (!NT_SUCCESS(status)) {
        WinError("{}", operation,
            ExplicitWin32Error{FspWin32FromNtStatus(status)});
    }
}

[[nodiscard]] auto MakeSecurityDescriptor(const std::wstring &account) {
    auto sid_size = DWORD{};
    auto domain_size = DWORD{};
    auto use = SID_NAME_USE{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        WinError("could not resolve --read-user");
    }

    auto sid = std::vector<BYTE>(sid_size);
    auto domain = std::vector<wchar_t>(std::max<DWORD>(1, domain_size));
    if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_size,
            domain.data(), &domain_size, &use)) {
        WinError("could not resolve --read-user");
    }

    auto sid_text = wil::unique_hlocal_string{};
    if (!ConvertSidToStringSidW(sid.data(), sid_text.addressof())) {
        WinError("could not format --read-user SID");
    }

    // SYSTEM and Administrators get full access.
    // The user specified by `--read-user` gets read and execute.
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            std::format(
                L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;{})",
                sid_text.get()).c_str(),
            SDDL_REVISION_1, descriptor.addressof(), nullptr)) {
        WinError("could not create the filesystem ACL");
    }
    return descriptor;
}

_Success_(return == ERROR_SUCCESS)
[[nodiscard]] auto Ioctl(const HANDLE device, const DWORD code,
    _Out_writes_bytes_opt_(output_size) void *const output,
    const DWORD output_size,
    _In_reads_bytes_opt_(input_size) void *const input = nullptr,
    const DWORD input_size = 0,
    _Out_opt_ DWORD *const bytes_returned = nullptr) {
    auto event = wil::unique_event_nothrow{};
    if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
        return GetLastError();
    }

    auto operation = OVERLAPPED{.hEvent = event.get()};
    auto returned = DWORD{};
    auto *const result_size = bytes_returned == nullptr ? &returned : bytes_returned;
    if (DeviceIoControl(device, code, input, input_size,
            output, output_size, result_size, &operation)) {
        return DWORD{ERROR_SUCCESS};
    }
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        return error;
    }
    if (!GetOverlappedResult(device, &operation, result_size, TRUE)) {
        return GetLastError();
    }
    return DWORD{ERROR_SUCCESS};
}

constexpr auto kVolumeBitmapHeaderSize = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
constexpr auto kBitsPerByte = std::numeric_limits<BYTE>::digits;

struct AllocationBitmap {
    UINT32 cluster_size = 0;
    UINT64 cluster_count = 0;
    wil::unique_virtualalloc_ptr<BYTE> storage;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    std::unique_ptr<FreeClusterMeasurement> measurement;
#endif

    [[nodiscard]] auto IsAllocated(const UINT64 cluster) const noexcept {
        // Preserve any device tail not represented by NTFS clusters.
        if (cluster >= cluster_count) {
            return true;
        }
        return (storage.get()[kVolumeBitmapHeaderSize + cluster / kBitsPerByte] &
            (1u << (cluster % kBitsPerByte))) != 0;
    }

    [[nodiscard]] auto HasAllocatedClusters(
        const UINT64 offset,
        _In_range_(1, MAXUINT64 - offset) const UINT64 length) const noexcept {
        if (!storage) {
            return true;
        }

        const auto first_cluster = offset / cluster_size;
        const auto last_cluster = (offset + (length - 1)) / cluster_size;
        if (last_cluster >= cluster_count) {
            return true;
        }
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            if (IsAllocated(cluster)) {
                return true;
            }
        }
        return false;
    }

    auto SynthesizeFreeClusters(
        const std::span<BYTE> output,
        const UINT64 offset) const noexcept {
        if (!storage || output.empty()) {
            return;
        }

        const auto end = offset + output.size();
        const auto first_cluster = offset / cluster_size;
        const auto last_cluster = (end - 1) / cluster_size;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        measurement->ObserveRead(output, offset);
#endif
        auto free_begin = end;
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            const auto position = std::max(offset, cluster * cluster_size);
            if (!IsAllocated(cluster)) {
                if (free_begin == end) {
                    free_begin = position;
                }
            } else if (free_begin != end) {
                std::ranges::fill(
                    output.subspan(free_begin - offset, position - free_begin), 0);
                free_begin = end;
            }
        }
        if (free_begin != end) {
            std::ranges::fill(
                output.subspan(free_begin - offset, end - free_begin), 0);
        }
    }
};

struct DeviceFile {
    std::wstring name;
    wil::unique_hfile handle;
    UINT32 sector_size = 0;
    AllocationBitmap allocation_bitmap;
    FSP_FSCTL_FILE_INFO info{};
};

// WinFsp directory markers require a stable order and an upper-bound lookup.
using DeviceFiles = std::map<std::wstring, DeviceFile>;

[[nodiscard]] auto LoadAllocationBitmap(
    const HANDLE device, const UINT64 device_size, const UINT64 map_number) {
    auto volume = NTFS_VOLUME_DATA_BUFFER{};
    const auto volume_error =
        Ioctl(device, FSCTL_GET_NTFS_VOLUME_DATA, &volume, sizeof(volume));
    if (volume_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_NTFS_VOLUME_DATA failed for --map #{}", map_number,
            ExplicitWin32Error{volume_error});
    }
    if ((volume.TotalClusters.QuadPart <= 0) || (volume.BytesPerCluster == 0)) {
        throw std::runtime_error(std::format(
            "FSCTL_GET_NTFS_VOLUME_DATA returned invalid data for --map #{}",
            map_number));
    }

    // The nonpositive case is rejected above, so this conversion preserves
    // the cluster count.
    const auto cluster_count =
        wil::safe_cast_failfast<UINT64>(volume.TotalClusters.QuadPart);
    if (cluster_count > (device_size / volume.BytesPerCluster)) {
        throw std::runtime_error(std::format(
            "NTFS cluster span exceeds the exposed device length for --map #{}",
            map_number));
    }

    // The bitmap is applied directly to device offsets, so LCN 0 must begin at byte 0.
    auto retrieval_base = RETRIEVAL_POINTER_BASE{};
    const auto retrieval_base_error = Ioctl(device,
        FSCTL_GET_RETRIEVAL_POINTER_BASE, &retrieval_base, sizeof(retrieval_base));
    if (retrieval_base_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_RETRIEVAL_POINTER_BASE failed for --map #{}",
            map_number, ExplicitWin32Error{retrieval_base_error});
    }
    if (retrieval_base.FileAreaOffset.QuadPart != 0) {
        throw std::runtime_error(std::format(
            "NTFS LCN 0 is offset {} sectors from the start of the exposed device "
            "for --map #{}",
            retrieval_base.FileAreaOffset.QuadPart, map_number));
    }

    const auto bitmap_bytes =
        cluster_count / kBitsPerByte +
        ((cluster_count % kBitsPerByte) != 0);
    const auto bitmap_data_size = kVolumeBitmapHeaderSize + bitmap_bytes;
    const auto output_size = std::max(sizeof(VOLUME_BITMAP_BUFFER), bitmap_data_size);
    if (!std::in_range<DWORD>(output_size)) {
        throw std::runtime_error(std::format(
            "NTFS allocation bitmap is too large for --map #{}", map_number));
    }
    // std::in_range above proves output_size is representable by DWORD.
    const auto output_size_for_api =
        wil::safe_cast_failfast<DWORD>(output_size);

    auto storage = wil::unique_virtualalloc_ptr<BYTE>(static_cast<BYTE *>(
        VirtualAlloc(nullptr, output_size_for_api, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
    if (!storage) {
        WinError(
            "could not allocate NTFS allocation bitmap for --map #{}", map_number);
    }
    auto *const output = std::start_lifetime_as<VOLUME_BITMAP_BUFFER>(storage.get());
    auto input = STARTING_LCN_INPUT_BUFFER{.StartingLcn = {.QuadPart = 0}};
    auto returned = DWORD{};
    const auto bitmap_error = Ioctl(device, FSCTL_GET_VOLUME_BITMAP,
        output, output_size_for_api, &input, sizeof(input), &returned);
    if (bitmap_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_VOLUME_BITMAP failed for --map #{}", map_number,
            ExplicitWin32Error{bitmap_error});
    }
    if ((output->StartingLcn.QuadPart != 0) ||
        (output->BitmapSize.QuadPart != volume.TotalClusters.QuadPart) ||
        (returned < bitmap_data_size)) {
        throw std::runtime_error(std::format(
            "FSCTL_GET_VOLUME_BITMAP returned incomplete data for --map #{}",
            map_number));
    }

    auto result = AllocationBitmap{
        .cluster_size = volume.BytesPerCluster,
        .cluster_count = cluster_count,
        .storage = std::move(storage),
    };
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    result.measurement = std::make_unique<FreeClusterMeasurement>(
        std::span<const BYTE>{
            result.storage.get() + kVolumeBitmapHeaderSize,
            bitmap_bytes,
        },
        volume.BytesPerCluster, cluster_count);
#endif
    return result;
}

[[nodiscard]] auto OpenDevice(
    const Mapping &mapping, const bool extended_dasd,
    const bool cache, const bool synthetic_free_clusters,
    const UINT64 map_number) {
    auto handle = wil::unique_hfile(CreateFileW(mapping.device.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!handle) {
        WinError("could not open block device for --map #{}", map_number);
    }

    auto length = GET_LENGTH_INFORMATION{};
    const auto length_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length));
    if (length_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_LENGTH_INFO failed for --map #{}", map_number,
            ExplicitWin32Error{length_error});
    }
    if (length.Length.QuadPart < 0) {
        throw std::runtime_error(std::format(
            "IOCTL_DISK_GET_LENGTH_INFO returned an invalid length for --map #{}",
            map_number));
    }

    auto geometry = DISK_GEOMETRY{};
    const auto geometry_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, &geometry, sizeof(geometry));
    if (geometry_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_DRIVE_GEOMETRY failed for --map #{}", map_number,
            ExplicitWin32Error{geometry_error});
    }

    // The negative case is rejected above, so this conversion preserves the
    // device length.
    const auto size =
        wil::safe_cast_failfast<UINT64>(length.Length.QuadPart);
    if ((geometry.BytesPerSector == 0) || ((size % geometry.BytesPerSector) != 0)) {
        throw std::runtime_error(std::format(
            "block device length is not a multiple of its sector size for --map #{}",
            map_number));
    }
    if ((size % kAdvertisedSectorSize) != 0) {
        throw std::runtime_error(std::format(
            "block device length is not a multiple of the advertised "
            "allocation unit for --map #{}",
            map_number));
    }

    const auto dasd_error = extended_dasd
        ? Ioctl(handle.get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0)
        : DWORD{ERROR_SUCCESS};
    if (dasd_error != ERROR_SUCCESS) {
        const auto error = std::error_code(
            std::bit_cast<int>(dasd_error), std::system_category());
        devicefs::WriteToStream(
            std::cerr,
            L"devicefs: warning: FSCTL_ALLOW_EXTENDED_DASD_IO failed for '{}': ",
            mapping.device);
        devicefs::WriteToStream(std::cerr, "{}\n", error.message());
    }

    if (cache || synthetic_free_clusters) {
        auto file_system_flags = DWORD{};
        if (!GetVolumeInformationByHandleW(handle.get(), nullptr, 0, nullptr,
                nullptr, &file_system_flags, nullptr, 0)) {
            WinError(
                "could not query filesystem flags for --map #{}", map_number);
        }
        if ((file_system_flags & FILE_READ_ONLY_VOLUME) == 0) {
            const auto option = cache ? "--cache" : "--synthetic-free-clusters";
            throw std::runtime_error(std::format(
                "{} requires a read-only volume for --map #{}", option, map_number));
        }
    }
    auto allocation_bitmap = synthetic_free_clusters
        ? LoadAllocationBitmap(handle.get(), size, map_number)
        : AllocationBitmap{};
    return DeviceFile{
        .name = mapping.name,
        .handle = std::move(handle),
        .sector_size = geometry.BytesPerSector,
        .allocation_bitmap = std::move(allocation_bitmap),
        .info = {
            .FileAttributes = FILE_ATTRIBUTE_READONLY,
            .AllocationSize = size,
            .FileSize = size,
            .IndexNumber = map_number + 1,
        },
    };
}

template <typename Function>
[[nodiscard]] auto NtCallback(const Function &function) noexcept {
    try {
        return function();
    } catch (...) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
}

auto CloseFileSystem(FSP_FILE_SYSTEM *const fs) noexcept {
    FspFileSystemStopDispatcher(fs);
    FspFileSystemDelete(fs);
}

using UniqueFileSystem = wil::unique_any<
    FSP_FILE_SYSTEM *, decltype(&CloseFileSystem), CloseFileSystem>;

auto g_stop_event = HANDLE{};
auto g_stopped_event = HANDLE{};
auto g_dispatcher_stopped_unexpectedly = false;

class DeviceFs {
public:
    [[nodiscard]] DeviceFs(
        DeviceFiles files, wil::unique_hlocal_security_descriptor security,
        Mount mount, const bool cache)
        : files_(std::move(files)), security_(std::move(security)), mount_(std::move(mount)) {
        auto params = FSP_FSCTL_VOLUME_PARAMS{
            .Version = sizeof(FSP_FSCTL_VOLUME_PARAMS),
            .SectorSize = kAdvertisedSectorSize,
            .SectorsPerAllocationUnit = 1,
            .MaxComponentLength = kMaxNameLength,
            .FileInfoTimeout = cache ? MAXUINT32 : 0,
            .CasePreservedNames = 1,
            .UnicodeOnDisk = 1,
            .PersistentAcls = 1,
            .ReadOnlyVolume = 1,
            // Give raw-device reads page-aligned buffers.
            .AlwaysUseDoubleBuffering = 1,
        };
        wcscpy_s(params.FileSystemName, kFileSystemName.data());

        if (mount_.network) {
            wcscpy_s(params.Prefix, mount_.value.c_str());
        }
        auto device = std::wstring{mount_.network
            ? L"" FSP_FSCTL_NET_DEVICE_NAME
            : L"" FSP_FSCTL_DISK_DEVICE_NAME};
        CheckNt(FspFileSystemCreate(device.data(), &params, &interface_, fs_.put()),
            "could not create WinFsp filesystem");
        fs_.get()->UserContext = this;
    }

    auto Start() {
        if (!mount_.network) {
            [[gsl::suppress("type.3",
                justification: "WinFsp copies the mount point despite accepting a mutable pointer.")]]
            CheckNt(FspFileSystemSetMountPoint(
                fs_.get(), const_cast<PWSTR>(mount_.value.c_str())),
                "could not mount filesystem");
        }
        CheckNt(FspFileSystemStartDispatcher(fs_.get(), 0),
            "could not start WinFsp dispatcher");
    }

private:
    [[nodiscard]] static auto Self(const FSP_FILE_SYSTEM *const fs) noexcept -> decltype(auto) {
        return *static_cast<const DeviceFs *>(fs->UserContext);
    }

    [[nodiscard]] auto File(wil::zwstring_view path) const {
        path.remove_prefix(1);
        const auto found = files_.find(Lowercase(path));
        return found == files_.end() ? nullptr : &found->second;
    }

    static auto GetVolumeInfo(
        FSP_FILE_SYSTEM *const fs, FSP_FSCTL_VOLUME_INFO *const info) noexcept {
        info->TotalSize = std::ranges::fold_left(Self(fs).files_, UINT64{},
            [](const auto total, const auto &entry) {
                return total + entry.second.info.FileSize;
            });
        info->FreeSize = 0;
        [[gsl::suppress("type.4",
            justification: "Braced initialization checks this constant expression for narrowing.")]]
        info->VolumeLabelLength = UINT16{kVolumeLabel.size() * sizeof(wchar_t)};
        std::ranges::copy(kVolumeLabel, info->VolumeLabel);
        return STATUS_SUCCESS;
    }

    _Success_(return == STATUS_SUCCESS)
    static auto GetSecurity(FSP_FILE_SYSTEM *const fs, void *,
        _Out_writes_bytes_to_opt_(*size, *size) void *const output,
        _Inout_ SIZE_T *const size) noexcept {
        const auto &security = Self(fs).security_;
        const auto length = GetSecurityDescriptorLength(security.get());
        if (*size < length) {
            *size = length;
            return STATUS_BUFFER_OVERFLOW;
        }
        *size = length;
        if (output != nullptr) {
            std::memcpy(output, security.get(), length);
        }
        return STATUS_SUCCESS;
    }

    _Success_(return == STATUS_SUCCESS)
    static auto GetSecurityByName(FSP_FILE_SYSTEM *const fs,
        _In_z_ wchar_t *const name,
        _Out_opt_ UINT32 *const attributes,
        _When_(size != nullptr,
            _Out_writes_bytes_to_opt_(*size, *size)) void *const security,
        _Inout_opt_ SIZE_T *const size) noexcept {
        return NtCallback([&] {
            const auto &self = Self(fs);
            const auto root = 0 == std::wcscmp(name, L"\\");
            const auto *const file = root ? nullptr : self.File(name);
            if (!root && (file == nullptr)) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (attributes != nullptr) {
                *attributes = root ? kRootInfo.FileAttributes : file->info.FileAttributes;
            }
            return size == nullptr ? STATUS_SUCCESS : GetSecurity(fs, nullptr, security, size);
        });
    }

    _Success_(return == STATUS_SUCCESS)
    static auto Open(FSP_FILE_SYSTEM *const fs,
        _In_z_ wchar_t *const name,
        [[maybe_unused]] const UINT32 create_options, const UINT32 access,
        _Outptr_result_maybenull_ void **const context,
        _Out_ FSP_FSCTL_FILE_INFO *const info) noexcept {
        return NtCallback([&] {
            const auto &self = Self(fs);
            const auto root = 0 == std::wcscmp(name, L"\\");
            const auto *const file = root ? nullptr : self.File(name);
            if (!root && (file == nullptr)) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (access & kWriteAccess) {
                return STATUS_MEDIA_WRITE_PROTECTED;
            }
#if DEVICEFS_MEASURE_READ_PATH
            if (!root) {
                self.read_measurement_.RecordOpen(create_options);
            }
#endif
            [[gsl::suppress("type.3",
                justification: "WinFsp stores an opaque context as void *, but DeviceFile is immutable.")]]
            *context = const_cast<DeviceFile *>(file);
            *info = root ? kRootInfo : file->info;
            return STATUS_SUCCESS;
        });
    }

    _Success_(return == STATUS_SUCCESS)
    static auto Read([[maybe_unused]] FSP_FILE_SYSTEM *const fs,
        _In_opt_ void *const context,
        _Out_writes_bytes_to_(length, *transferred) void *const buffer,
        const UINT64 offset, const ULONG length,
        _Out_ ULONG *const transferred) noexcept {
        *transferred = 0;
        const auto *const file = static_cast<const DeviceFile *>(context);
        if (file == nullptr) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (length == 0) {
            return STATUS_SUCCESS;
        }
        if (offset >= file->info.FileSize) {
            return STATUS_END_OF_FILE;
        }

        [[gsl::suppress("type.1",
            justification: "The minimum cannot exceed the ULONG length argument.")]]
        const auto wanted = static_cast<std::remove_cv_t<decltype(length)>>(
            std::min(UINT64{length}, file->info.FileSize - offset));
        const auto output = std::span<BYTE>{static_cast<BYTE *>(buffer), wanted};
#if DEVICEFS_MEASURE_READ_PATH
        auto observation = Self(fs).read_measurement_.BeginRead(length, wanted);
#endif
        if constexpr (!kMeasureFreeClusterData) {
            if (!file->allocation_bitmap.HasAllocatedClusters(offset, wanted)) {
#if DEVICEFS_MEASURE_READ_PATH
                observation.RecordSynthetic();
#endif
                std::ranges::fill(output, 0);
                *transferred = wanted;
                return STATUS_SUCCESS;
            }
        }
        const auto failure = [&](const DWORD error) {
            devicefs::WriteToStream(
                std::cerr,
                L"devicefs: read failed for '{}': Windows error {}\n",
                file->name, error);
            return FspNtStatusFromWin32(error);
        };
        const auto read = [&](void *const output, const UINT64 position,
                              const auto count, auto *const done) {
            // ReadFile resets the event; each dispatcher thread uses it serially.
            thread_local auto event = wil::unique_event_nothrow{};
            if (!event && !event.try_create(wil::EventOptions::ManualReset, nullptr)) {
                return failure(GetLastError());
            }
            auto operation = OVERLAPPED{};
            const auto parts = ULARGE_INTEGER{.QuadPart = position};
            operation.Offset = parts.LowPart;
            operation.OffsetHigh = parts.HighPart;
            operation.hEvent = event.get();
#if DEVICEFS_MEASURE_READ_PATH
            observation.BeginSourceRead();
#endif
            // GetOverlappedResult supplies the byte count for either completion path.
            if (!ReadFile(file->handle.get(), output, count, nullptr, &operation)) {
                const auto error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    return failure(error);
                }
#if DEVICEFS_MEASURE_READ_PATH
                observation.RecordSourcePending();
#endif
            }
            if (!GetOverlappedResult(file->handle.get(), &operation, done, TRUE)) {
                return failure(GetLastError());
            }
            if (*done != count) {
                return failure(ERROR_READ_FAULT);
            }
#if DEVICEFS_MEASURE_READ_PATH
            observation.FinishSourceRead(*done);
#endif
            return STATUS_SUCCESS;
        };

        const auto sector_size = file->sector_size;
        const auto read_offset = offset - (offset % sector_size);
        const auto end = offset + wanted;
        const auto read_end = ((end + sector_size - 1) / sector_size) * sector_size;
        using LengthType = std::remove_cv_t<decltype(length)>;
        const auto aligned_length = read_end - read_offset;
        if (!std::in_range<LengthType>(aligned_length)) {
            return STATUS_INVALID_PARAMETER;
        }
        [[gsl::suppress("type.1",
            justification: "std::in_range above proves aligned_length is representable by LengthType.")]]
        const auto read_length = static_cast<LengthType>(aligned_length);
        if ((read_offset == offset) && (read_length == wanted)) {
            const auto status = read(output.data(), offset, wanted, transferred);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            file->allocation_bitmap.SynthesizeFreeClusters(output, offset);
            return STATUS_SUCCESS;
        }

        const auto prefix = offset - read_offset;

        auto storage = wil::unique_virtualalloc_ptr<BYTE>(static_cast<BYTE *>(
            VirtualAlloc(nullptr, read_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
        if (!storage) {
            return failure(GetLastError());
        }
        const auto bounce = std::span<BYTE>{storage.get(), read_length};
        auto device_transferred = LengthType{};
        const auto status = read(
            bounce.data(), read_offset, read_length, &device_transferred);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        *transferred = wanted;
        std::ranges::copy(bounce.subspan(prefix, wanted), output.begin());
        file->allocation_bitmap.SynthesizeFreeClusters(output, offset);
#if DEVICEFS_MEASURE_READ_PATH
        observation.RecordBounce();
#endif
        return STATUS_SUCCESS;
    }

    static auto GetFileInfo(FSP_FILE_SYSTEM *,
        _In_opt_ void *const context,
        _Out_ FSP_FSCTL_FILE_INFO *const info) noexcept {
        *info = context == nullptr
            ? kRootInfo
            : static_cast<const DeviceFile *>(context)->info;
        return STATUS_SUCCESS;
    }

    _Success_(return == STATUS_SUCCESS)
    static auto ReadDirectory(FSP_FILE_SYSTEM *const fs,
        _In_opt_ void *const context,
        wchar_t *,
        _In_opt_z_ wchar_t *const marker,
        _Out_writes_bytes_to_(length, *transferred) void *const buffer,
        const ULONG length, _Out_ ULONG *const transferred) noexcept {
        return NtCallback([&] {
            *transferred = 0;
            if (context != nullptr) {
                return STATUS_NOT_A_DIRECTORY;
            }
            const auto &self = Self(fs);
            auto current = marker == nullptr
                ? self.files_.begin()
                : self.files_.upper_bound(Lowercase(marker));
            alignas(FSP_FSCTL_DIR_INFO) auto storage = std::array<BYTE, kMaxDirectoryInfoSize>{};
            auto *const info = std::start_lifetime_as<FSP_FSCTL_DIR_INFO>(storage.data());
            for (; current != self.files_.end(); ++current) {
                const auto &file = current->second;
                const auto name_bytes = file.name.size() * sizeof(wchar_t);
                // Filename validation bounds the record by
                // kMaxDirectoryInfoSize, which is asserted to fit this field.
                info->Size = wil::safe_cast_failfast<decltype(info->Size)>(
                    sizeof(FSP_FSCTL_DIR_INFO) + name_bytes);
                info->FileInfo = file.info;
                std::memcpy(info->FileNameBuf, file.name.data(), name_bytes);
                if (!FspFileSystemAddDirInfo(info, buffer, length, transferred)) {
                    return STATUS_SUCCESS;
                }
            }
            FspFileSystemAddDirInfo(nullptr, buffer, length, transferred);
            return STATUS_SUCCESS;
        });
    }

    static auto DispatcherStopped(
        [[maybe_unused]] FSP_FILE_SYSTEM *const fs,
        const BOOLEAN normally) noexcept {
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        for (const auto &entry : Self(fs).files_) {
            const auto &file = entry.second;
            if (file.allocation_bitmap.measurement) {
                file.allocation_bitmap.measurement->Report(file.name);
            }
        }
#endif
#if DEVICEFS_MEASURE_READ_PATH
        Self(fs).read_measurement_.Report();
#endif
        if (normally) {
            return;
        }
        g_dispatcher_stopped_unexpectedly = true;
        SetEvent(g_stop_event);
    }

    static const FSP_FILE_SYSTEM_INTERFACE interface_;
    const DeviceFiles files_;
    const wil::unique_hlocal_security_descriptor security_;
    const Mount mount_;
#if DEVICEFS_MEASURE_READ_PATH
    mutable ReadPathMeasurement read_measurement_;
#endif
    // Declared last so it stops callbacks before the state they reference is destroyed.
    UniqueFileSystem fs_;
};

const FSP_FILE_SYSTEM_INTERFACE DeviceFs::interface_ = {
    .GetVolumeInfo = GetVolumeInfo,
    .GetSecurityByName = GetSecurityByName,
    // WinFsp requires Create, Open, and Overwrite callbacks as a group.
    .Create = [](auto...) noexcept { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Open = Open,
    .Overwrite = [](auto...) noexcept { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Read = Read,
    .GetFileInfo = GetFileInfo,
    .GetSecurity = GetSecurity,
    .ReadDirectory = ReadDirectory,
    .DispatcherStopped = DispatcherStopped,
};

auto WINAPI ControlHandler(const DWORD event) noexcept -> BOOL {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return SetEvent(g_stop_event);
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (!SetEvent(g_stop_event)) {
            return FALSE;
        }
        WaitForSingleObject(g_stopped_event, INFINITE);
        return TRUE;
    default:
        return FALSE;
    }
}

auto Run(const Options &options) {
    if (!NT_SUCCESS(FspLoad(nullptr))) {
        throw std::runtime_error("could not load WinFsp DLL");
    }

    auto security = MakeSecurityDescriptor(options.read_user);
    auto files = DeviceFiles{};
    for (auto i = 0uz; i < options.mappings.size(); ++i) {
        const auto &mapping = options.mappings[i];
        files.emplace(Lowercase(mapping.name),
            OpenDevice(mapping, options.extended_dasd,
                options.cache, options.synthetic_free_clusters, i + 1));
    }

    auto stop_event = wil::unique_event_nothrow{};
    auto already_exists = false;
    if (!stop_event.try_create(wil::EventOptions::ManualReset,
            options.stop_event.c_str(), nullptr, &already_exists)) {
        WinError("could not create shutdown event");
    }
    if (already_exists) {
        throw std::runtime_error("--stop-event already exists");
    }
    auto stopped_event = wil::unique_event_nothrow{};
    if (!stopped_event.try_create(wil::EventOptions::ManualReset, nullptr)) {
        WinError("could not create shutdown-complete event");
    }

    auto wait_error = DWORD{};
    {
        if constexpr (kMeasureFreeClusterData) {
            if (options.synthetic_free_clusters) {
                devicefs::WriteToStream(
                    std::cerr,
                    "devicefs: free-cluster measurement is enabled; "
                    "free-only reads will access the source device\n");
            }
        }
        auto filesystem = DeviceFs(
            std::move(files), std::move(security), options.mount, options.cache);
        g_stop_event = stop_event.get();
        g_stopped_event = stopped_event.get();
        filesystem.Start();

        if (!SetConsoleCtrlHandler(ControlHandler, TRUE)) {
            WinError("could not install console handler");
        }

        devicefs::WriteToStream(
            std::cout,
            L"devicefs: mounted {} device(s) at {}; read access: {}; "
            L"stop event: {}\n",
            options.mappings.size(), options.mount.value,
            options.read_user, options.stop_event);
        if (WaitForSingleObject(stop_event.get(), INFINITE) == WAIT_FAILED) {
            wait_error = GetLastError();
        }
    }
    SetEvent(stopped_event.get());
    // The registered handler lives until process exit, so its handles must as well.
    stop_event.release();
    stopped_event.release();
    if (wait_error != ERROR_SUCCESS) {
        WinError("shutdown wait failed", ExplicitWin32Error{wait_error});
    }
    if (g_dispatcher_stopped_unexpectedly) {
        throw std::runtime_error("WinFsp dispatcher stopped unexpectedly");
    }
    return 0;
}

} // namespace

export namespace devicefs {

auto Main(const std::span<const wchar_t *const> arguments) -> int {
    try {
        FurtherHardenProcess();
        auto options = Options{};
        try {
            options = ParseArgs(arguments);
        } catch (const std::invalid_argument &error) {
            devicefs::WriteToStream(
                std::cerr, "devicefs: {}\n\n", error.what());
            Usage(std::cerr);
            return 2;
        }
        if (options.help) {
            Usage(std::cout);
            return 0;
        }
        return Run(options);
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(
            std::cerr, "devicefs: {}\n", error.what());
        return 1;
    }
}

} // namespace devicefs
