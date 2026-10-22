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

export module devicefs.filesystem;

import std;
import devicefs.terminal.safecast;
import :internal;
import :rpc_client;
import :winfsp_loader;
export import devicefs.windows_block_device;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import devicefs.rpc_constants;
import devicefs.stream_writer;
import devicefs.vhdx_viewer;

#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA || DEVICEFS_MEASURE_READ_PATH
import devicefs.filesystem_measurement;
#endif

import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

namespace devicefs {

export template <typename DeviceType>
concept BlockDevice = requires(
    const DeviceType &device,
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
concept WindowsDevice =
    std::same_as<std::remove_cvref_t<DeviceType>, WindowsBlockDevice<false>> ||
    std::same_as<std::remove_cvref_t<DeviceType>, WindowsBlockDevice<true>>;

template <typename... DeviceTypes>
using SelectedWindowsBlockDevice = WindowsBlockDevice<
    (std::same_as<DeviceTypes, WindowsBlockDevice<true>> || ...)>;

// `std::conditional_t` needs both type arguments to be valid. An incomplete
// primary template lets the alias name `VariantMultiBlockDevice<T>` without
// defining a one-type wrapper.
template <typename... DeviceTypes>
struct VariantMultiBlockDevice;

template <BlockDevice... DeviceTypes>
    requires (sizeof...(DeviceTypes) > 1)
struct VariantMultiBlockDevice<DeviceTypes...> {
    const std::uint64_t length;
    std::variant<DeviceTypes...> contents;

    // Precondition: `contents` holds a device, rather than being valueless.
    // `std::visit` can then throw only if `visitor` throws.
    auto Visit(this auto &self, auto &&visitor)
        noexcept((std::is_nothrow_invocable_v<decltype(visitor),
            decltype(std::get<DeviceTypes>(self.contents))> && ...))
        -> decltype(auto) {
        using WindowsType = SelectedWindowsBlockDevice<DeviceTypes...>;
        if constexpr (
            (std::same_as<DeviceTypes, WindowsType> || ...)) {
            if (auto *const device = std::get_if<WindowsType>(
                    &self.contents)) [[likely]] {
                return std::invoke(
                    std::forward<decltype(visitor)>(visitor), *device);
            }
        }
        return std::visit(
            [&visitor](auto &device)
                noexcept(std::is_nothrow_invocable_v<
                    decltype(visitor), decltype(device)>)
                -> std::invoke_result_t<decltype(visitor), decltype(device)> {
                if constexpr (std::same_as<
                    std::remove_cvref_t<decltype(device)>,
                    WindowsType>) {
                    std::unreachable();
                } else {
                    return std::invoke(
                        std::forward<decltype(visitor)>(visitor), device);
                }
            }, self.contents);
    }

    template <typename... Observers>
    _Success_((return >= 0) && (return != STATUS_PENDING))
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        return Visit(
            [buffer, offset, wanted, &transferred, &observers...](
                const auto &device) noexcept {
                [[msvc::forceinline_calls]]
                return device.Read(
                    buffer, offset, wanted, transferred, observers...);
            });
    }
};

template <BlockDevice... DeviceTypes>
    requires (sizeof...(DeviceTypes) > 0)
using VariantBlockDevice = std::conditional_t<
    sizeof...(DeviceTypes) == 1,
    std::tuple_element_t<0, std::tuple<DeviceTypes...>>,
    VariantMultiBlockDevice<DeviceTypes...>>;

template <BlockDevice Destination, BlockDevice Source>
[[nodiscard]] auto VariantFromBlockDevice(Source source) noexcept
    -> Destination {
    if constexpr (std::same_as<Destination, Source>) {
        return source;
    } else {
        return Destination{
            .length = source.length,
            .contents = std::move(source),
        };
    }
}

auto VisitBlockDevice(BlockDevice auto &device, auto &&visitor)
    noexcept(std::is_nothrow_invocable_v<
        decltype(visitor), decltype(device)>)
    -> decltype(auto) {
    return std::invoke(std::forward<decltype(visitor)>(visitor), device);
}

auto VisitBlockDevice(BlockDevice auto &device, auto &&visitor)
    noexcept(noexcept(device.Visit(
        std::forward<decltype(visitor)>(visitor))))
    -> decltype(auto)
    requires requires {
        device.Visit(std::forward<decltype(visitor)>(visitor));
    } {
    return device.Visit(std::forward<decltype(visitor)>(visitor));
}

} // namespace devicefs

auto internal::CheckNt(
    const NTSTATUS status, const wil::zstring_view operation) -> void {
    if (!NT_SUCCESS(status)) {
        WinError("{}", operation,
            ExplicitWin32Error{FspWin32FromNtStatus(status)});
    }
}

namespace {

using namespace std::string_view_literals;

constexpr auto kDefaultStopEvent = "Local\\devicefs-stop"sv;
constexpr auto kFileSystemName = L"DEVICEFS"sv;
constexpr auto kKnownDataMapSuffix = L".known-data.bitmap";
static_assert(kFileSystemName.size() + 1 <=
    std::size(FSP_FSCTL_VOLUME_PARAMS{}.FileSystemName),
    "The filesystem name and its terminator must fit the WinFsp volume parameters.");
constexpr auto kMaxNameLength = UINT16{255};
constexpr auto kMaxDirectoryInfoSize =
    sizeof(FSP_FSCTL_DIR_INFO) + kMaxNameLength * sizeof(wchar_t);
static_assert(std::in_range<decltype(FSP_FSCTL_DIR_INFO::Size)>(kMaxDirectoryInfoSize),
    "The largest directory record must fit in the FSP_FSCTL_DIR_INFO::Size field.");
constexpr auto kMaxMountPrefixLength = std::size(FSP_FSCTL_VOLUME_PARAMS{}.Prefix) - 1;
constexpr auto kVolumeLabel = L"DEVICEFS"sv;
static_assert(kVolumeLabel.size() <= std::size(FSP_FSCTL_VOLUME_INFO{}.VolumeLabel),
    "The volume label must fit the WinFsp volume information buffer.");
constexpr auto kRootInfo = FSP_FSCTL_FILE_INFO{
    .FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY,
    .IndexNumber = 1,
};
constexpr auto kWriteAccess = UINT32{FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
    FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER};

struct Mount {
    bool network = false;
    std::wstring value;
};

struct Options {
    Mount mount;
    std::string read_user;
    std::string stop_event{kDefaultStopEvent};
    std::vector<internal::Mapping> mappings;
    bool cache = false;
    bool extended_dasd = true;
    bool synthetic_free_clusters = false;
    [[msvc::no_unique_address]]
    std::optional<std::uint64_t> known_data_map_cluster_size;
    bool vhdx = false;
    bool help = false;
};

auto FurtherHardenProcess() {
    auto child_processes = PROCESS_MITIGATION_CHILD_PROCESS_POLICY{};
    child_processes.NoChildProcessCreation = 1;
    if (!SetProcessMitigationPolicy(
            ProcessChildProcessPolicy, &child_processes, sizeof(child_processes))) {
        WinError("failed to prohibit child process creation");
    }
}

struct FilenameLess {
    using is_transparent = std::true_type;

    [[nodiscard]] auto operator()(auto &&left, auto &&right) const {
        return _wcsicmp_l(wil::str_raw_ptr(left), wil::str_raw_ptr(right),
            locale_.get()) < 0;
    }

private:
    // This locale is chosen for compatibility with the directory listings
    // produced by cmd.exe's `dir /L` command.
    wil::shared_locale locale_{_create_locale(LC_ALL, ".OCP")};
};

[[nodiscard]] auto CurrentUserName() {
    auto buffer = std::array<char, UNLEN + 1>{};
    auto length = CompileTimeCast<DWORD, buffer.size()>();
    if (!GetUserNameA(buffer.data(), &length)) {
        WinError("failed to obtain the current user name");
    }
    return std::string{buffer.data(), length - 1};
}

auto Usage(const auto output) noexcept {
    devicefs::WriteToStream(output,
        "Usage: devicefs --mount TARGET --map NAME DEVICE"
        " [--map NAME DEVICE ...] [OPTIONS]\n\n"
        "Options:\n"
        "  --mount TARGET             Drive letter, directory, or network prefix\n"
        "  --read-user USER           User granted read access (default: current user)\n"
        "  --map NAME DEVICE          Virtual filename and block device (repeatable)\n"
        "  --stop-event NAME          Named shutdown event (default: {})\n"
        "  --cache                    Enable file-data caching (requires read-only volumes)\n"
        "  --no-extended-dasd-io      Do not issue FSCTL_ALLOW_EXTENDED_DASD_IO\n"
        "  --synthetic-free-clusters  Return zeros for free clusters on read-only NTFS and ReFS volumes\n"
        "  --expose-known-data-map CLUSTER-SIZE\n"
        "                             Add FILENAME.known-data.bitmap for each image\n"
        "                             CLUSTER-SIZE is bytes per bitmap bit; requires\n"
        "                             --synthetic-free-clusters; incompatible with --vhdx\n"
        "  --vhdx                     Expose each mapped volume as a VHDX disk\n"
        "  -h, --help                 Show this help\n\n"
        "Example:\n"
        "  devicefs --mount X: `\n"
        "    --map C.img '\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy12'\n",
        kDefaultStopEvent);
}

[[nodiscard]] auto ParseArgs(
    const std::span<const std::string_view> args) {
    auto result = Options{};
    const auto next = [&](auto &i) {
        if (++i == args.size()) {
            throw std::invalid_argument(std::format(
                "{} requires a value", args[i - 1]));
        }
        return args[i];
    };

    for (auto i = 1uz; i < args.size(); ++i) {
        const auto arg = args[i];
        if ((arg == "-h") || (arg == "--help")) {
            result.help = true;
        } else if (arg == "--mount") {
            result.mount.value =
                Transcode<std::wstring>(next(i));
        } else if (arg == "--read-user") {
            result.read_user = next(i);
        } else if (arg == "--stop-event") {
            result.stop_event = next(i);
        } else if (arg == "--map") {
            result.mappings.push_back({
                .name = Transcode<std::wstring>(next(i)),
                .device = std::string{next(i)},
            });
        } else if (arg == "--cache") {
            result.cache = true;
        } else if (arg == "--no-extended-dasd-io") {
            result.extended_dasd = false;
        } else if (arg == "--synthetic-free-clusters") {
            result.synthetic_free_clusters = true;
        } else if (arg == "--expose-known-data-map") {
            const auto value = next(i);
            auto size = std::uint64_t{};
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), size);
            if ((error != std::errc{}) || (end != (value.data() + value.size())) || (size == 0)) {
                throw std::invalid_argument(std::format(
                    "--expose-known-data-map requires a positive byte count; received '{}'", value));
            }
            result.known_data_map_cluster_size = size;
        } else if (arg == "--vhdx") {
            result.vhdx = true;
        } else {
            throw std::invalid_argument(std::format(
                "unknown option '{}' at argument {}", arg, i));
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

    if (result.known_data_map_cluster_size && !result.synthetic_free_clusters) {
        throw std::invalid_argument(
            "--expose-known-data-map requires --synthetic-free-clusters");
    }
    if (result.known_data_map_cluster_size && result.vhdx) {
        throw std::invalid_argument(
            "--expose-known-data-map cannot be combined with --vhdx");
    }

    auto names = std::set<std::wstring, FilenameLess>{};
    const auto register_name = [&names](const std::wstring &name, const std::size_t map_number) {
        if ((name.empty()) || (name == L".") || (name == L"..") ||
            (name.size() > kMaxNameLength) ||
            (name.find_first_of(L"/\\:") != std::wstring_view::npos)) {
            throw std::invalid_argument(std::format(
                "invalid filename '{}' in --map #{}",
                Transcode<std::string>(name), map_number));
        }
        if (!names.emplace(name).second) {
            throw std::invalid_argument(std::format(
                "duplicate filename '{}' in --map #{}",
                Transcode<std::string>(name), map_number));
        }
    };
    for (auto i = 0uz; i < result.mappings.size(); ++i) {
        const auto &mapping = result.mappings[i];
        register_name(mapping.name, i + 1);
        if (result.known_data_map_cluster_size && !rpc_client::IsRpcDevice(mapping)) {
            register_name(mapping.name + kKnownDataMapSuffix, i + 1);
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
        throw std::invalid_argument(std::format(
            "UNC mount prefix has {} characters; the maximum is {}",
            result.mount.value.size(), kMaxMountPrefixLength));
    }
    return result;
}

[[nodiscard]] auto MakeSecurityDescriptor(const std::string &account) {
    auto sid_size = DWORD{};
    auto domain_size = DWORD{};
    auto use = SID_NAME_USE{};
    LookupAccountNameA(nullptr, account.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        WinError("failed to resolve --read-user account '{}'", account);
    }

    const auto sid = NEW_MALLOCA_BUFFER(BYTE, sid_size);
    const auto domain = NEW_MALLOCA_BUFFER(char, std::max<DWORD>(1, domain_size));
    if (!sid || !domain) {
        throw std::bad_alloc{};
    }
    if (!LookupAccountNameA(nullptr, account.c_str(), sid.get(), &sid_size,
            domain.get(), &domain_size, &use)) {
        WinError("failed to resolve --read-user account '{}'", account);
    }

    auto sid_text = wil::unique_hlocal_ansistring{};
    if (!ConvertSidToStringSidA(sid.get(), sid_text.addressof())) {
        WinError("failed to format the SID for --read-user account '{}'", account);
    }

    // Administrators own the filesystem and mount directory, allowing an
    // elevated administrator to create the directory without assigning
    // ownership to another account through SeRestorePrivilege.
    // SYSTEM and Administrators get full access.
    // The user specified by `--read-user` gets read and execute.
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            std::format(
                "O:BAG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;{})",
                sid_text.get()).c_str(),
            SDDL_REVISION_1, descriptor.addressof(), nullptr)) {
        WinError("failed to create the filesystem ACL for --read-user account '{}'",
            account);
    }
    return descriptor;
}

using devicefs::WindowsBlockDevice;
using devicefs::WindowsDevice;
using devicefs::SelectedWindowsBlockDevice;
using devicefs::VariantBlockDevice;
using devicefs::VisitBlockDevice;
using devicefs::VariantFromBlockDevice;
using devicefs::VhdxViewer;
using devicefs::BlockDevice;
using rpc_client::RPCBlockDevice;

// Preconditions for reads: `contents.size() == length`, and the destination
// buffer does not overlap this object, the storage owned by `contents`, or
// `transferred`.
// `__restrict` expresses the non-overlap condition to the optimizer.
struct MemoryBlockDevice {
    const std::uint64_t length;
    std::vector<BYTE> contents;

    template <typename... Observers>
    _Success_(return == STATUS_SUCCESS)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred)
        void *__restrict const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...) const noexcept -> NTSTATUS {
        std::memcpy(buffer, contents.data() + offset, wanted);
        transferred = wanted;
        return STATUS_SUCCESS;
    }
};

template <typename DeviceType>
struct DeviceFile {
    std::wstring name;
    FSP_FSCTL_FILE_INFO info{};
    DeviceType device;
};

// WinFsp directory markers require a stable order and an upper-bound lookup.
template <typename DeviceType>
using DeviceFiles = std::map<std::wstring, DeviceFile<DeviceType>, FilenameLess>;

[[nodiscard]] auto ParseInheritedHandle(
    const std::string_view value, const std::string_view description) {
    auto handle = std::uintptr_t{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), handle);
    if ((error != std::errc{}) || (end != std::to_address(value.end()))) {
        throw std::invalid_argument(std::format(
            "invalid inherited block-device handle '{}' for {}; "
            "expected an unsigned decimal integer", value, description));
    }
    return wil::unique_hfile{std::bit_cast<HANDLE>(handle)};
}

template <BlockDevice... DeviceTypes>
[[nodiscard]] auto OpenSource(
    const internal::Mapping &mapping, const bool extended_dasd,
    const bool cache,
    const UINT64 map_number,
    const std::string_view rpc_password)
    -> VariantBlockDevice<DeviceTypes...> {
    using SourceType = VariantBlockDevice<DeviceTypes...>;
    if constexpr ((std::same_as<DeviceTypes, RPCBlockDevice> || ...)) {
        if (rpc_client::IsRpcDevice(mapping)) {
            const auto tcp = rpc_client::IsTcpDevice(mapping);
            return VariantFromBlockDevice<SourceType>(
                RPCBlockDevice::FromSymbol(
                    tcp
                        ? rpc_client::MakeTcpRpcBinding(mapping, rpc_password)
                        : rpc_client::LocalRpcBinding(),
                    tcp
                        ? std::string_view{}
                        : std::string_view{mapping.device}.substr(
                            std::string_view{
                                devicefs::rpc::kDeviceSourcePrefix}.size())));
        }
    }
    if constexpr ((WindowsDevice<DeviceTypes> || ...)) {
        using WindowsType = SelectedWindowsBlockDevice<DeviceTypes...>;
        const auto source = std::string_view{mapping.device};
        const auto description = std::format("--map #{}", map_number);
        return VariantFromBlockDevice<SourceType>(
            [source, &description](auto &&...arguments) {
                if (source.starts_with(devicefs::kHandleDevicePrefix)) {
                    return WindowsType::FromHandle(
                        ParseInheritedHandle(
                            source.substr(devicefs::kHandleDevicePrefix.size()),
                            description),
                        std::forward<decltype(arguments)>(arguments)...);
                }
                return WindowsType::FromFilename(
                    std::forward<decltype(arguments)>(arguments)...);
            }(std::filesystem::path{mapping.device}, extended_dasd,
                cache, description));
    } else {
        std::unreachable();
    }
}

template <typename Function>
[[nodiscard]] auto NtCallback(const Function &function) noexcept {
    try {
        return function();
    } catch (const std::exception &error) {
        devicefs::WriteToStream(devicefs::stderr,
            "devicefs: filesystem callback failed: {}\n", error.what());
        return STATUS_UNEXPECTED_IO_ERROR;
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
            .SectorSize = WindowsBlockDevice<>::kAdvertisedSectorSize,
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
        internal::CheckNt(FspFileSystemCreate(device.data(), &params, &interface_, fs_.put()),
            "failed to create WinFsp filesystem");
        fs_.get()->UserContext = this;
        if (!mount_.network) {
            // The Windows mount directory has its own security descriptor.
            // Passing ours here gives `--read-user` access to that directory
            // as well as the root and files served by DeviceFs.
            [[gsl::suppress("type.3",
                justification: "WinFsp copies the mount point despite accepting a mutable pointer.")]]
            internal::CheckNt(FspFileSystemSetMountPointEx(
                fs_.get(), const_cast<PWSTR>(mount_.value.c_str()), security_.get()),
                "failed to mount filesystem");
        }
        internal::CheckNt(FspFileSystemStartDispatcher(fs_.get(), 0),
            "failed to start WinFsp dispatcher");
    }

    DeviceFs(const DeviceFs &) = delete;
    auto operator=(const DeviceFs &) -> DeviceFs & = delete;
    DeviceFs(DeviceFs &&) = delete;
    auto operator=(DeviceFs &&) -> DeviceFs & = delete;

    ~DeviceFs() noexcept {
        auto open_files = open_file_count_.load();
        if (open_files != 0) {
            devicefs::WriteToStream(devicefs::stderr,
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
        const auto found = files_.find(path);
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
        info->VolumeLabelLength =
            CompileTimeCast<UINT16, kVolumeLabel.size() * sizeof(wchar_t)>();
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
    _Success_((return >= 0) && (return != STATUS_PENDING))
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
        // another location for its core read logic. The [[msvc::flatten]]
        // attribute expresses that policy.
        [[msvc::flatten]]
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
                : self.files_.upper_bound(marker);
            alignas(FSP_FSCTL_DIR_INFO) auto storage = std::array<BYTE, kMaxDirectoryInfoSize>{};
            auto *const info = std::start_lifetime_as<FSP_FSCTL_DIR_INFO>(storage.data());
            for (; current != self.files_.end(); ++current) {
                const auto &file = current->second;
                const auto name_bytes = file.name.size() * sizeof(wchar_t);
                // Filename validation bounds the record by
                // kMaxDirectoryInfoSize, which is asserted to fit this field.
                info->Size = devicefs::terminal::FailFastCast<
                    decltype(info->Size)>(
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
        for (const auto &entry : Self(fs).files_) {
            const auto &file = entry.second;
            VisitBlockDevice(file.device, [&file](const auto &device) {
                if constexpr (std::same_as<
                    std::remove_cvref_t<decltype(device)>,
                    WindowsBlockDevice<true>>) {
                    if (device.allocation_bitmap.measurement) {
                        device.allocation_bitmap.measurement->Report(
                            file.name);
                    }
                }
            });
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
        FAIL_FAST_IF_WIN32_BOOL_FALSE(SetEvent(g_stop_event));
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

template <BlockDevice DeviceType>
auto RunWithDevices(const Options &options, DeviceFiles<DeviceType> files) {
    std::tie(g_stop_event, g_stopped_event) = [&options] {
        auto stop_event = wil::unique_event_nothrow{CreateEventExA(
            nullptr, options.stop_event.c_str(),
            CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
        if (!stop_event) {
            WinError("failed to create stop event '{}'", options.stop_event);
        }
        auto stopped_event = wil::unique_event_nothrow{};
        if (!stopped_event.try_create(wil::EventOptions::ManualReset, nullptr)) {
            WinError("failed to create completion event");
        }
        // The console application signal handler that is installed below
        // references these two events. That signal handler is never
        // unregistered. Therefore, these two events must not be freed while
        // the process remains running. Releasing the events from their smart
        // owners prevents them from being freed.
        return std::pair{stop_event.release(), stopped_event.release()};
    }();
    const auto signal_completion_event_on_exit = wil::scope_exit([] noexcept {
        // This completion event (`g_stopped_event`) must be signalled on all
        // paths out of this function.
        FAIL_FAST_IF_WIN32_BOOL_FALSE(SetEvent(g_stopped_event));
    });
    const auto signal_handler = [](DWORD) noexcept -> BOOL {
        FAIL_FAST_IF_WIN32_BOOL_FALSE(SetEvent(g_stop_event));
        wil::handle_wait(g_stopped_event);
        return TRUE;
    };
    if (!SetConsoleCtrlHandler(signal_handler, TRUE)) {
        WinError("failed to install console application signal handler");
    }
    if constexpr (WindowsBlockDevice<>::kMeasureFreeClusterData) {
        if (options.synthetic_free_clusters) {
            devicefs::WriteToStream(
                devicefs::stderr,
                "devicefs: free-cluster measurement is enabled; "
                "free-only reads will access the source device\n");
        }
    }
    {
        const auto filesystem = DeviceFs{
            std::move(files), MakeSecurityDescriptor(options.read_user),
            options.mount, options.cache};
        devicefs::WriteToStream(
            devicefs::stdout,
            "devicefs: mounted {} device(s) at {}; read access: {}; "
            "stop event: {}\n",
            options.mappings.size(),
            Transcode<std::string>(options.mount.value),
            options.read_user, options.stop_event);
        wil::handle_wait(g_stop_event);
    }
    if (g_dispatcher_stopped_unexpectedly) {
        throw std::runtime_error("WinFsp dispatcher stopped unexpectedly");
    }
    return EXIT_SUCCESS;
}

template <bool AsVhdx, bool ExposeKnownDataMap, BlockDevice... SourceTypes>
[[nodiscard]] auto OpenDevices(
    const Options &options,
    wil::secure_string rpc_password = {}) {
    using SourceType = VariantBlockDevice<SourceTypes...>;
    using DeviceType = std::conditional_t<AsVhdx,
        VhdxViewer<SourceType>,
        std::conditional_t<ExposeKnownDataMap,
            VariantBlockDevice<SourceTypes..., MemoryBlockDevice>,
            SourceType>>;
    auto sources = std::views::iota(0uz, options.mappings.size())
        | std::views::transform([&](const auto i) {
            return OpenSource<SourceTypes...>(
                options.mappings[i], options.extended_dasd,
                options.cache,
                i + 1, std::string_view{rpc_password});
        }) | std::ranges::to<std::vector>();
    if constexpr ((WindowsDevice<SourceTypes> || ...)) {
        // `VhdxViewer::FromBlockDevice` reads NTFS metadata from its source and
        // can therefore allocate a thread's retained read buffer. The maximum
        // alignment must already be known before that first read, so all sources
        // are opened before any VHDX views are constructed.
        WindowsBlockDevice<>::read_buffer_alignment = std::ranges::max(
            sources | std::views::transform([](const auto &source) static {
                return VisitBlockDevice(source, [](const auto &device) static {
                    if constexpr (WindowsDevice<decltype(device)>) {
                        return device.buffer_alignment;
                    } else {
                        return std::align_val_t{1};
                    }
                });
            }));
    }
    auto next_index = kRootInfo.IndexNumber + 1;
    auto files = DeviceFiles<DeviceType>{};
    for (const auto i : std::views::iota(0uz, options.mappings.size())) {
        const auto &mapping = options.mappings[i];
        auto &source = sources[i];
        auto device = [&source] {
            if constexpr (AsVhdx) {
                return VhdxViewer<SourceType>::FromBlockDevice(
                    std::move(source));
            } else if constexpr (std::same_as<DeviceType, SourceType>) {
                return std::move(source);
            } else {
                return VisitBlockDevice(source, [](auto &device) static {
                    return VariantFromBlockDevice<DeviceType>(
                        std::move(device));
                });
            }
        }();
        files.emplace(
            mapping.name,
            DeviceFile<DeviceType>{
                .name = mapping.name,
                .info = {
                    .FileAttributes = FILE_ATTRIBUTE_READONLY,
                    .AllocationSize = device.length,
                    .FileSize = device.length,
                    .IndexNumber = next_index++,
                },
                .device = std::move(device),
            });
    }
    if constexpr (ExposeKnownDataMap) {
        for (const auto &mapping : options.mappings) {
            const auto *const device = std::get_if<WindowsBlockDevice<true>>(
                &files.at(mapping.name).device.contents);
            if (!device) {
                continue;
            }
            auto bitmap = device->MakeKnownDataMap(
                *options.known_data_map_cluster_size);
            const auto name = mapping.name + kKnownDataMapSuffix;
            files.emplace(name, DeviceFile<DeviceType>{
                .name = name,
                .info = {
                    .FileAttributes = FILE_ATTRIBUTE_READONLY,
                    .AllocationSize = bitmap.size(),
                    .FileSize = bitmap.size(),
                    .IndexNumber = next_index++,
                },
                .device = VariantFromBlockDevice<DeviceType>(
                    MemoryBlockDevice{
                        .length = bitmap.size(),
                        .contents = std::move(bitmap),
                    }),
            });
        }
    }
    return files;
}

// Precondition: `needed` selects at least one candidate type.
template <BlockDevice... Candidates>
auto DispatchDeviceTypes(
    const std::array<bool, sizeof...(Candidates)> &needed,
    const auto &run) -> int {
    return [&]<std::size_t Index = 0, class... Selected>(
               this auto &&self) -> int {
        if constexpr (Index == sizeof...(Candidates)) {
            if constexpr (sizeof...(Selected) == 0) {
                std::unreachable();
            } else {
                return run.template operator()<Selected...>();
            }
        } else {
            using Next = std::tuple_element_t<Index,
                std::tuple<Candidates...>>;
            if (needed[Index]) {
                return self.template operator()<
                    Index + 1, Selected..., Next>();
            }
            return self.template operator()<Index + 1, Selected...>();
        }
    }();
}

auto Run(const Options &options) {
    devicefs::winfsp_loader::LoadWinFspLibrary();
    auto password = [&options] {
        auto password = wil::secure_string{};
        if (std::ranges::any_of(options.mappings, rpc_client::IsTcpDevice)) {
            ClearCrtIoError();
            std::getline(std::cin, password);
            if (std::ferror(stdin) || std::cin.bad()) {
                WinError("failed to read the RPC password from standard input",
                    ExplicitCrtIoError{});
            }
            if (!std::cin) {
                WinError("failed to read the RPC password from standard input",
                    ExplicitWin32Error{std::cin.eof()
                        ? CompileTimeCast<DWORD, ERROR_HANDLE_EOF>()
                        : CompileTimeCast<DWORD, ERROR_INSUFFICIENT_BUFFER>()});
            }
        }
        return password;
    }();
    const auto run = [&options, &password]<BlockDevice... SourceTypes> {
        if (options.vhdx) {
            return RunWithDevices(options,
                OpenDevices<true, false, SourceTypes...>(
                    options, std::move(password)));
        }
        if constexpr ((std::same_as<
                SourceTypes, WindowsBlockDevice<true>> || ...)) {
            if (options.known_data_map_cluster_size) {
                return RunWithDevices(options,
                    OpenDevices<false, true, SourceTypes...>(
                        options, std::move(password)));
            }
        }
        return RunWithDevices(options,
            OpenDevices<false, false, SourceTypes...>(
                options, std::move(password)));
    };
    const auto rpc_devices =
        std::ranges::count_if(options.mappings, rpc_client::IsRpcDevice);
    const auto dispatch = [&options, rpc_devices, &run]<bool HasBitmap> {
        return DispatchDeviceTypes<WindowsBlockDevice<HasBitmap>,
            RPCBlockDevice>(
                {std::cmp_less(rpc_devices, options.mappings.size()),
                    rpc_devices != 0}, run);
    };
    return options.synthetic_free_clusters
        ? dispatch.template operator()<true>()
        : dispatch.template operator()<false>();
}

} // namespace

export namespace devicefs {

auto Main(const std::span<const std::string_view> arguments) -> int {
    FurtherHardenProcess();
    auto options = Options{};
    try {
        options = ParseArgs(arguments);
    } catch (const std::invalid_argument &error) {
        devicefs::WriteToStream(
            devicefs::stderr, "devicefs: {}\n\n", error.what());
        Usage(devicefs::stderr);
        return ERROR_INVALID_COMMAND_LINE;
    }
    if (options.help) {
        Usage(devicefs::stdout);
        return EXIT_SUCCESS;
    }
    if (options.read_user.empty()) {
        options.read_user = CurrentUserName();
    }
    return Run(options);
}

} // namespace devicefs
