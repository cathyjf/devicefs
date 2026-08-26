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

#define RPC_USE_NATIVE_WCHAR

#include <sal.h>
#include <windows.h>
#include <lmcons.h>
#include <sddl.h>

// wil/stl.h uses symbols defined in <algorithm> without including it.
#include <algorithm>

#include <devicefs/rpc_block_device.h>
#include <devicefs/midl_compat.h>
#include <devicefs/winfsp_compat.h>
#include <wil/resource.h>
#include <wil/rpc_helpers.h>
#include <wil/safecast.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

export module devicefs.filesystem;

import std;
export import devicefs.windows_block_device;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.vhdx_viewer;

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

constexpr auto kDefaultStopEvent = std::wstring_view(L"Local\\devicefs-stop");
constexpr auto kFileSystemName = std::wstring_view(L"DEVICEFS");
constexpr auto kRpcDevicePrefix = std::wstring_view{LR"(\\\)"};
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

[[nodiscard]] auto IsRpcDevice(const Mapping &mapping) noexcept {
    return mapping.device.starts_with(kRpcDevicePrefix);
}

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
    bool vhdx = false;
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

[[nodiscard]] auto CurrentUserName() {
    auto length = DWORD{UNLEN + 1};
    auto result = std::wstring(length, L'\0');
    if (!GetUserNameW(result.data(), &length)) {
        WinError("could not obtain the current user name");
    }
    result.resize(length - 1);
    return result;
}

auto Usage(std::ostream &output) noexcept {
    devicefs::WriteToStream(output,
        L"Usage: devicefs --mount TARGET --map NAME DEVICE"
        L" [--map NAME DEVICE ...] [OPTIONS]\n\n"
        L"Options:\n"
        L"  --mount TARGET             Drive letter, directory, or network prefix\n"
        L"  --read-user USER           User granted read access (default: current user)\n"
        L"  --map NAME DEVICE          Virtual filename and block device (repeatable)\n"
        L"  --stop-event NAME          Named shutdown event (default: {})\n"
        L"  --cache                    Enable file-data caching (requires read-only volumes)\n"
        L"  --no-extended-dasd-io      Do not issue FSCTL_ALLOW_EXTENDED_DASD_IO\n"
        L"  --synthetic-free-clusters  Return zeros for free clusters on read-only NTFS volumes\n"
        L"  --vhdx                     Expose each mapped volume as a VHDX disk\n"
        L"  -h, --help                 Show this help\n\n"
        L"Example:\n"
        L"  devicefs --mount X: `\n"
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
        } else if (arg == L"--vhdx") {
            result.vhdx = true;
        } else {
            throw std::invalid_argument(std::format("unknown option at argument {}", i));
        }
    }

    if (result.stop_event.empty()) {
        throw std::invalid_argument("--stop-event must not be empty");
    }
    if (!result.help &&
        ((result.mount.value.empty()) || (result.mappings.empty()))) {
        throw std::invalid_argument(
            "--mount and at least one --map are required");
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
    if (!result.mappings.empty()) {
        const auto rpc = IsRpcDevice(result.mappings.front());
        if (!std::ranges::all_of(result.mappings,
                [rpc](const auto &mapping) {
                    return IsRpcDevice(mapping) == rpc;
                })) {
            throw std::invalid_argument(
                "all --map devices must use RPC when any one does");
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

} // namespace

namespace {

using devicefs::WindowsBlockDevice;
using devicefs::VhdxViewer;

[[nodiscard]] auto MakeRpcBinding(const wil::zwstring_view endpoint) {
    auto endpoint_text = std::wstring{endpoint};
    auto protocol_sequence = std::to_array(
        DEVICEFS_RPC_PROTOCOL_SEQUENCE);
    auto string_binding = wil::unique_rpc_wstr{};
    const auto compose_error = RpcStringBindingComposeW(
        nullptr, protocol_sequence.data(), nullptr,
        endpoint_text.data(), nullptr, string_binding.put());
    if (compose_error != RPC_S_OK) {
        WinError("could not compose the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(compose_error)});
    }

    auto result = wil::unique_rpc_binding{};
    const auto error = RpcBindingFromStringBindingW(
        string_binding.get(), result.put());
    if (error != RPC_S_OK) {
        WinError("could not create the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(error)});
    }
    return wil::shared_rpc_binding{std::move(result)};
}

struct RPCBlockDevice {
    const std::uint64_t length;

    [[nodiscard]] static auto FromSymbol(
        const wil::shared_rpc_binding &binding,
        const std::wstring_view symbol) {
        auto stored_symbol = std::wstring{symbol};
        const auto length = [&] {
            auto result = std::uint64_t{};
            auto status = NTSTATUS{};
            const auto error = wil::invoke_rpc_result_nothrow(
                status, DeviceFsRpcClient_GetLength,
                binding.get(), stored_symbol.c_str(), &result);
            if (FAILED(error)) {
                WinError("could not query the RPC block-device length",
                    ExplicitWin32Error::FromHresult(error));
            }
            CheckNt(status, "could not query the RPC block-device length");
            return result;
        }();
        return RPCBlockDevice{
            length, std::move(stored_symbol), binding};
    }

    template <typename... Observers>
    _Success_(return >= 0)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        auto status = NTSTATUS{};
        auto rpc_transferred = ULONG{};
        (observers.BeginSourceRead(), ...);
        const auto error = wil::invoke_rpc_result_nothrow(
            status, DeviceFsRpcClient_Read,
            binding_.get(), symbol_.c_str(), offset, wanted,
            &rpc_transferred, static_cast<BYTE *>(buffer));
        if (FAILED(error)) {
            const auto win32_error =
                ExplicitWin32Error::FromHresult(error).value;
            devicefs::WriteToStream(std::cerr,
                L"devicefs: RPC read failed for '{}': Windows error {}\n",
                symbol_, win32_error);
            return FspNtStatusFromWin32(win32_error);
        }
        transferred = rpc_transferred;
        if (status >= 0) {
            (observers.FinishSourceRead(rpc_transferred), ...);
        }
        return status;
    }

  private:
    RPCBlockDevice(
        const std::uint64_t length,
        std::wstring symbol,
        wil::shared_rpc_binding binding) noexcept
        : length(length), symbol_(std::move(symbol)),
          binding_(std::move(binding)) {}

    std::wstring symbol_;
    wil::shared_rpc_binding binding_;
};

template <typename DeviceType>
concept BlockDevice = requires(
    DeviceType &device,
    _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
    _In_range_(0, device.length - 1)
        const std::remove_const_t<decltype(device.length)> offset,
    _In_range_(1, device.length - offset) const ULONG wanted,
    _Pre_equal_to_(0) ULONG &transferred) {
    { device.length } -> std::same_as<const std::uint64_t &>;
    { device.Read(buffer, offset, wanted, transferred) }
        noexcept -> std::same_as<NTSTATUS>;
};

template <typename DeviceType>
struct DeviceFile {
    std::wstring name;
    FSP_FSCTL_FILE_INFO info{};
    DeviceType device;
};

// WinFsp directory markers require a stable order and an upper-bound lookup.
template <typename DeviceType>
using DeviceFiles = std::map<std::wstring, DeviceFile<DeviceType>>;

} // namespace

namespace {

template <BlockDevice DeviceType>
[[nodiscard]] auto OpenDevice(
    const Mapping &mapping, const bool extended_dasd,
    const bool cache, const bool synthetic_free_clusters,
    const UINT64 map_number,
    const wil::shared_rpc_binding &rpc_binding) {
    auto source = [&] {
        if constexpr ((std::same_as<DeviceType, RPCBlockDevice>) ||
            (std::same_as<DeviceType, VhdxViewer<RPCBlockDevice>>)) {
            return RPCBlockDevice::FromSymbol(rpc_binding,
                std::wstring_view{mapping.device}.substr(
                    kRpcDevicePrefix.size()));
        } else {
            return WindowsBlockDevice::FromFilename(
                std::filesystem::path{mapping.device}, extended_dasd,
                cache, synthetic_free_clusters,
                std::format("--map #{}", map_number));
        }
    }();
    auto device = [&]() -> DeviceType {
        if constexpr (std::same_as<DeviceType, decltype(source)>) {
            return std::move(source);
        } else {
            return DeviceType::FromBlockDevice(std::move(source));
        }
    }();
    return DeviceFile<DeviceType>{
        .name = mapping.name,
        .info = {
            .FileAttributes = FILE_ATTRIBUTE_READONLY,
            .AllocationSize = device.length,
            .FileSize = device.length,
            .IndexNumber = map_number + 1,
        },
        .device = std::move(device),
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

template <BlockDevice DeviceType>
class DeviceFs {
public:
    [[nodiscard]] DeviceFs(
        DeviceFiles<DeviceType> files,
        wil::unique_hlocal_security_descriptor security,
        Mount mount, const bool cache)
        : files_(std::move(files)), security_(std::move(security)), mount_(std::move(mount)) {
        auto params = FSP_FSCTL_VOLUME_PARAMS{
            .Version = sizeof(FSP_FSCTL_VOLUME_PARAMS),
            .SectorSize = WindowsBlockDevice::kAdvertisedSectorSize,
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

    auto WaitForOpenFilesToClose() const noexcept {
        auto open_files = open_file_count_.load();
        if (open_files != 0) {
            devicefs::WriteToStream(std::cerr,
                "devicefs: waiting for open files to close before shutdown\n");
        }
        while (open_files != 0) {
            open_file_count_.wait(open_files);
            open_files = open_file_count_.load();
        }
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

    auto EndFileOpen() const noexcept {
        if (open_file_count_.fetch_sub(1) == 1) {
            open_file_count_.notify_all();
        }
    }

    [[gsl::suppress("26461",
        justification:
            "The function signature must match the requirement of the "
            "corresponding WinFsp callback.")]]
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

    [[gsl::suppress("26461", "26429",
        justification:
            "The function signature must match the requirement of the "
            "corresponding WinFsp callback. The `_Inout_` annotation reflects "
            "that `size` cannot be null.")]]
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
            if (!root) {
                self.open_file_count_.fetch_add(1);
            }
#if DEVICEFS_MEASURE_READ_PATH
            if (!root) {
                self.read_measurement_.RecordOpen(create_options);
            }
#endif
            [[gsl::suppress("type.3",
                justification: "WinFsp stores an opaque context as void *, but DeviceFile is immutable.")]]
            *context = const_cast<DeviceFile<DeviceType> *>(file);
            *info = root ? kRootInfo : file->info;
            return STATUS_SUCCESS;
        });
    }

    [[gsl::suppress("26461",
        justification:
            "The function signature must match the requirement of the "
            "corresponding WinFsp callback.")]]
    static auto Cleanup(FSP_FILE_SYSTEM *const fs,
        _In_opt_ void *const context, wchar_t *, ULONG) noexcept {
        if (context != nullptr) {
            Self(fs).EndFileOpen();
        }
    }

    [[gsl::suppress("26429",
        justification:
            "The function signature must match the requirement of the "
            "corresponding WinFsp callback. The `_Out_` annotation reflects "
            "that `transferred` cannot be null.")]]
    _Success_(return == STATUS_SUCCESS)
    static auto Read([[maybe_unused]] FSP_FILE_SYSTEM *const fs,
        _In_opt_ void *const context,
        _Out_writes_bytes_to_(length, *transferred) void *const buffer,
        const UINT64 offset, const ULONG length,
        _Out_ ULONG *const transferred) noexcept {
        *transferred = 0;
        const auto *const file = static_cast<const DeviceFile<DeviceType> *>(context);
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
#if DEVICEFS_MEASURE_READ_PATH
        auto observation = Self(fs).read_measurement_.BeginRead(length, wanted);
#endif

        // For performance reasons, this WinFsp callback should not jump to
        // another location for its core read logic. The [[msvc::forceinline_calls]]
        // attribute expresses that policy.
        [[msvc::forceinline_calls]]
        return file->device.Read(
            buffer, offset, wanted, *transferred
#if DEVICEFS_MEASURE_READ_PATH
            , observation
#endif
        );
    }

    static auto GetFileInfo(FSP_FILE_SYSTEM *,
        _In_opt_ void *const context,
        _Out_ FSP_FSCTL_FILE_INFO *const info) noexcept {
        *info = context == nullptr
            ? kRootInfo
            : static_cast<const DeviceFile<DeviceType> *>(context)->info;
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

    [[gsl::suppress("26461",
        justification:
            "The function signature must match the requirement of the "
            "corresponding WinFsp callback.")]]
    static auto DispatcherStopped(
        [[maybe_unused]] FSP_FILE_SYSTEM *const fs,
        const BOOLEAN normally) noexcept {
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        if constexpr (std::same_as<DeviceType, WindowsBlockDevice>) {
            for (const auto &entry : Self(fs).files_) {
                const auto &file = entry.second;
                if (file.device.allocation_bitmap.measurement) {
                    file.device.allocation_bitmap.measurement->Report(file.name);
                }
            }
        }
#endif
#if DEVICEFS_MEASURE_READ_PATH
        Self(fs).read_measurement_.Report();
#endif
        if (normally) {
            return;
        }
        // If the WinFsp dispatcher fails, Windows can no longer tell us when
        // open handles are closed. Stop waiting so the process can exit.
        Self(fs).open_file_count_.store(0);
        Self(fs).open_file_count_.notify_all();
        g_dispatcher_stopped_unexpectedly = true;
        SetEvent(g_stop_event);
    }

    static const FSP_FILE_SYSTEM_INTERFACE interface_;
    const DeviceFiles<DeviceType> files_;
    const wil::unique_hlocal_security_descriptor security_;
    const Mount mount_;
    mutable std::atomic_size_t open_file_count_{};
#if DEVICEFS_MEASURE_READ_PATH
    mutable ReadPathMeasurement read_measurement_;
#endif
    // Declared last so it stops callbacks before the state they reference is destroyed.
    UniqueFileSystem fs_;
};

template <BlockDevice DeviceType>
const FSP_FILE_SYSTEM_INTERFACE DeviceFs<DeviceType>::interface_ = {
    .GetVolumeInfo = GetVolumeInfo,
    .GetSecurityByName = GetSecurityByName,
    // WinFsp requires Create, Open, and Overwrite callbacks as a group.
    .Create = [](auto...) noexcept { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Open = Open,
    .Overwrite = [](auto...) noexcept { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Cleanup = Cleanup,
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

template <BlockDevice DeviceType>
auto RunWithDevice(
    const Options &options,
    const wil::shared_rpc_binding rpc_binding = nullptr) {
    auto security = MakeSecurityDescriptor(options.read_user);
    auto files = DeviceFiles<DeviceType>{};
    for (auto i = 0uz; i < options.mappings.size(); ++i) {
        const auto &mapping = options.mappings[i];
        files.emplace(Lowercase(mapping.name),
            OpenDevice<DeviceType>(mapping, options.extended_dasd,
                options.cache, options.synthetic_free_clusters,
                i + 1, rpc_binding));
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
        if constexpr (WindowsBlockDevice::kMeasureFreeClusterData) {
            if (options.synthetic_free_clusters) {
                devicefs::WriteToStream(
                    std::cerr,
                    "devicefs: free-cluster measurement is enabled; "
                    "free-only reads will access the source device\n");
            }
        }
        auto filesystem = DeviceFs<DeviceType>(
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
        filesystem.WaitForOpenFilesToClose();
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

auto Run(const Options &options) {
    if (!NT_SUCCESS(FspLoad(nullptr))) {
        throw std::runtime_error("could not load WinFsp DLL");
    }
    const auto rpc = IsRpcDevice(options.mappings.front());
    if (rpc) {
        const auto binding = [] {
            auto result = std::wstring{};
            const auto error = wil::GetEnvironmentVariableW(
                DEVICEFS_RPC_ENDPOINT_ENVIRONMENT_VARIABLE, result);
            if (FAILED(error)) {
                WinError("could not obtain the RPC block-device endpoint",
                    ExplicitWin32Error::FromHresult(error));
            }
            if (result.empty()) {
                throw std::runtime_error(
                    "the RPC block-device endpoint is empty");
            }
            return MakeRpcBinding(result);
        }();
        return options.vhdx
            ? RunWithDevice<VhdxViewer<RPCBlockDevice>>(options, binding)
            : RunWithDevice<RPCBlockDevice>(options, binding);
    }
    if (options.vhdx) {
        return RunWithDevice<VhdxViewer<WindowsBlockDevice>>(options);
    }
    return RunWithDevice<WindowsBlockDevice>(options);
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
        if (options.read_user.empty()) {
            options.read_user = CurrentUserName();
        }
        return Run(options);
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(
            std::cerr, "devicefs: {}\n", error.what());
        return 1;
    }
}

} // namespace devicefs
