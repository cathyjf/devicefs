#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <winioctl.h>

using PNTSTATUS = NTSTATUS *;
#pragma warning(push, 0)
#include <winfsp/winfsp.h>
#include <wil/resource.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr auto kBlockSize = UINT64{512};
constexpr auto kDefaultStopEvent = std::wstring_view(L"Local\\devicefs-stop");
constexpr auto kMaxNameLength = std::size_t{255};
constexpr auto kMaxMountPrefixLength = std::size(FSP_FSCTL_VOLUME_PARAMS{}.Prefix) - 1;
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
    bool extended_dasd = true;
    bool help = false;
};

[[noreturn]] auto WinError(
    const std::string &operation, const DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

auto Lowercase(const std::wstring_view value) {
    const auto input_size = static_cast<int>(value.size());
    const auto output_size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.data(), input_size, nullptr, 0, nullptr, nullptr, 0);
    if (output_size == 0) {
        WinError("could not lowercase a virtual filename");
    }

    auto result = std::wstring(static_cast<std::size_t>(output_size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            value.data(), input_size, result.data(), output_size, nullptr, nullptr, 0)) {
        WinError("could not lowercase a virtual filename");
    }
    return result;
}

auto Usage(std::wostream &out) {
    out << L"Usage: devicefs --mount TARGET --read-user USER"
           L" --map NAME DEVICE [--map NAME DEVICE ...] [OPTIONS]\n\n"
        << L"Options:\n"
        << L"  --stop-event NAME      Named shutdown event (default: "
        << kDefaultStopEvent << L")\n"
        << L"  --no-extended-dasd-io  Do not issue FSCTL_ALLOW_EXTENDED_DASD_IO\n"
        << L"  -h, --help             Show this help\n\n"
        << L"Example:\n"
        << L"  devicefs --mount X: --read-user '.\\pbs-vss' `\n"
        << L"    --map C.img '\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy12'\n";
}

auto ParseArgs(const int argc, const wchar_t *const *const argv) {
    auto result = Options{};
    const auto next = [&](auto &i) {
        if (++i == argc) {
            throw std::invalid_argument(
                std::format("missing value after argument {}", i - 1));
        }
        return argv[i];
    };

    for (auto i = 1; i < argc; ++i) {
        const auto arg = std::wstring_view(argv[i]);
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
        } else if (arg == L"--no-extended-dasd-io") {
            result.extended_dasd = false;
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
    for (auto i = std::size_t{}; i < result.mappings.size(); ++i) {
        const auto &name = result.mappings[i].name;
        if ((name.empty()) || (name == L".") || (name == L"..") ||
            (name.size() > kMaxNameLength) ||
            (name.find_first_of(L"/\\") != std::wstring_view::npos)) {
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

auto CheckNt(const NTSTATUS status, const char *const operation) {
    if (!NT_SUCCESS(status)) {
        WinError(operation, FspWin32FromNtStatus(status));
    }
}

auto MakeSecurityDescriptor(const std::wstring &account) {
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

    auto *sid_text = PWSTR{};
    if (!ConvertSidToStringSidW(sid.data(), &sid_text)) {
        WinError("could not format --read-user SID");
    }
    const auto sid_owner = wil::unique_hlocal_ptr<wchar_t>(sid_text);

    auto *raw = PSECURITY_DESCRIPTOR{};
    auto size = ULONG{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            std::format(
                L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;{})", sid_text).c_str(),
            SDDL_REVISION_1, &raw, &size)) {
        WinError("could not create the filesystem ACL");
    }
    const auto owner = wil::unique_hlocal_ptr<void>(raw);
    const auto *const first = static_cast<const BYTE *>(raw);
    return std::vector<BYTE>(first, first + size);
}

auto Ioctl(const HANDLE device, const DWORD code, void *const output,
    const DWORD output_size) {
    auto event = wil::unique_event_nothrow{};
    if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
        return GetLastError();
    }

    auto operation = OVERLAPPED{.hEvent = event.get()};
    auto returned = DWORD{};
    if (DeviceIoControl(device, code, nullptr, 0, output, output_size, &returned, &operation)) {
        return DWORD{ERROR_SUCCESS};
    }
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        return error;
    }
    if (!GetOverlappedResult(device, &operation, &returned, TRUE)) {
        return GetLastError();
    }
    return DWORD{ERROR_SUCCESS};
}

struct DeviceFile {
    std::wstring name;
    wil::unique_hfile handle;
    UINT32 sector_size = 0;
    FSP_FSCTL_FILE_INFO info{};
};

using DeviceFiles = std::unordered_map<std::wstring, DeviceFile>;

auto OpenDevice(const Mapping &mapping, const bool extended_dasd, const UINT64 map_number) {
    auto handle = wil::unique_hfile(CreateFileW(mapping.device.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr));
    if (!handle) {
        WinError(std::format("could not open block device for --map #{}", map_number));
    }

    auto length = GET_LENGTH_INFORMATION{};
    const auto length_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length));
    if (length_error != ERROR_SUCCESS) {
        WinError(std::format("IOCTL_DISK_GET_LENGTH_INFO failed for --map #{}", map_number),
            length_error);
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
        WinError(std::format("IOCTL_DISK_GET_DRIVE_GEOMETRY failed for --map #{}", map_number),
            geometry_error);
    }

    const auto size = static_cast<UINT64>(length.Length.QuadPart);
    if ((geometry.BytesPerSector == 0) || ((size % geometry.BytesPerSector) != 0)) {
        throw std::runtime_error(std::format(
            "block device length is not a multiple of its sector size for --map #{}",
            map_number));
    }

    const auto dasd_error = extended_dasd
        ? Ioctl(handle.get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0)
        : DWORD{ERROR_SUCCESS};
    if (dasd_error != ERROR_SUCCESS) {
        const auto error = std::error_code(
            static_cast<int>(dasd_error), std::system_category());
        std::wcerr << L"devicefs: warning: FSCTL_ALLOW_EXTENDED_DASD_IO failed for '"
                   << mapping.device << L"': " << error.message().c_str() << L"\n";
    }

    return DeviceFile{
        .name = mapping.name,
        .handle = std::move(handle),
        .sector_size = geometry.BytesPerSector,
        .info = {
            .FileAttributes = FILE_ATTRIBUTE_READONLY,
            .AllocationSize = size,
            .FileSize = size,
            .IndexNumber = map_number + 1,
        },
    };
}

template <typename Function>
auto NtCallback(const Function &function) noexcept {
    try {
        return function();
    } catch (...) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
}

class DeviceFs {
public:
    DeviceFs(DeviceFiles files, std::vector<BYTE> security, Mount mount)
        : files_(std::move(files)), security_(std::move(security)), mount_(std::move(mount)) {
        auto params = FSP_FSCTL_VOLUME_PARAMS{
            .Version = static_cast<UINT16>(sizeof(FSP_FSCTL_VOLUME_PARAMS)),
            .SectorSize = static_cast<UINT16>(kBlockSize),
            .SectorsPerAllocationUnit = 1,
            .MaxComponentLength = static_cast<UINT16>(kMaxNameLength),
            .CasePreservedNames = 1,
            .UnicodeOnDisk = 1,
            .PersistentAcls = 1,
            .ReadOnlyVolume = 1,
        };
        wcscpy_s(params.FileSystemName, std::size(params.FileSystemName), L"DEVICEFS");

        if (mount_.network) {
            wcscpy_s(params.Prefix, std::size(params.Prefix), mount_.value.c_str());
        }
        const auto *const device = mount_.network
            ? L"" FSP_FSCTL_NET_DEVICE_NAME
            : L"" FSP_FSCTL_DISK_DEVICE_NAME;
        CheckNt(FspFileSystemCreate(const_cast<PWSTR>(device), &params, &interface_, &fs_),
            "could not create WinFsp filesystem");
        fs_->UserContext = this;
    }

    ~DeviceFs() {
        FspFileSystemStopDispatcher(fs_);
        FspFileSystemDelete(fs_);
    }

    auto Start() {
        if (!mount_.network) {
            CheckNt(FspFileSystemSetMountPoint(fs_, mount_.value.data()),
                "could not mount filesystem");
        }
        CheckNt(FspFileSystemStartDispatcher(fs_, 0), "could not start WinFsp dispatcher");
    }

private:
    static auto Self(const FSP_FILE_SYSTEM *const fs) noexcept -> decltype(auto) {
        return *static_cast<const DeviceFs *>(fs->UserContext);
    }

    auto File(const std::wstring_view path) const {
        const auto found = files_.find(Lowercase(path.substr(1)));
        return found == files_.end() ? nullptr : &found->second;
    }

    static auto GetVolumeInfo(
        FSP_FILE_SYSTEM *const fs, FSP_FSCTL_VOLUME_INFO *const info) noexcept {
        info->TotalSize = std::ranges::fold_left(Self(fs).files_, UINT64{},
            [](const auto total, const auto &entry) {
                return total + entry.second.info.FileSize;
            });
        info->FreeSize = 0;
        constexpr auto label = std::wstring_view(L"DEVICEFS");
        info->VolumeLabelLength = static_cast<UINT16>(label.size() * sizeof(wchar_t));
        std::memcpy(info->VolumeLabel, label.data(), info->VolumeLabelLength);
        return STATUS_SUCCESS;
    }

    static auto GetSecurity(FSP_FILE_SYSTEM *const fs, void *,
        void *const output, SIZE_T *const size) noexcept {
        const auto &security = Self(fs).security_;
        if (*size < security.size()) {
            *size = security.size();
            return STATUS_BUFFER_OVERFLOW;
        }
        *size = security.size();
        if (output != nullptr) {
            std::memcpy(output, security.data(), security.size());
        }
        return STATUS_SUCCESS;
    }

    static auto GetSecurityByName(FSP_FILE_SYSTEM *const fs, wchar_t *const name,
        UINT32 *const attributes, void *const security,
        SIZE_T *const size) noexcept {
        return NtCallback([&] {
            const auto &self = Self(fs);
            const auto root = 0 == wcscmp(name, L"\\");
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

    static auto Open(FSP_FILE_SYSTEM *const fs, wchar_t *const name,
        UINT32, const UINT32 access, void **const context,
        FSP_FSCTL_FILE_INFO *const info) noexcept {
        return NtCallback([&] {
            const auto &self = Self(fs);
            const auto root = 0 == wcscmp(name, L"\\");
            const auto *const file = root ? nullptr : self.File(name);
            if (!root && (file == nullptr)) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (access & kWriteAccess) {
                return STATUS_MEDIA_WRITE_PROTECTED;
            }
            *context = const_cast<DeviceFile *>(file);
            *info = root ? kRootInfo : file->info;
            return STATUS_SUCCESS;
        });
    }

    static auto Read(FSP_FILE_SYSTEM *, void *const context, void *const buffer,
        const UINT64 offset, const ULONG length,
        ULONG *const transferred) noexcept {
        *transferred = 0;
        const auto *const file = static_cast<const DeviceFile *>(context);
        if (file == nullptr) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (offset >= file->info.FileSize) {
            return STATUS_END_OF_FILE;
        }

        const auto wanted = static_cast<std::remove_cv_t<decltype(length)>>(
            std::min(UINT64{length}, file->info.FileSize - offset));
        const auto failure = [&](const auto error) {
            std::fwprintf(stderr, L"devicefs: read failed for '%ls': Windows error %lu\n",
                file->name.c_str(), static_cast<unsigned long>(error));
            return FspNtStatusFromWin32(error);
        };
        const auto read = [&](void *const output, const UINT64 position,
                              const auto count, auto *const done) {
            auto event = wil::unique_event_nothrow{};
            if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
                return failure(GetLastError());
            }
            auto operation = OVERLAPPED{};
            operation.Offset = static_cast<decltype(operation.Offset)>(position);
            operation.OffsetHigh = static_cast<decltype(operation.OffsetHigh)>(position >> 32);
            operation.hEvent = event.get();
            if (ReadFile(file->handle.get(), output, count, done, &operation)) {
                return STATUS_SUCCESS;
            }
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return failure(error);
            }
            if (!GetOverlappedResult(file->handle.get(), &operation, done, TRUE)) {
                return failure(GetLastError());
            }
            return STATUS_SUCCESS;
        };

        const auto sector_size = UINT64{file->sector_size};
        const auto read_offset = offset - (offset % sector_size);
        const auto end = offset + wanted;
        const auto read_end = ((end + sector_size - 1) / sector_size) * sector_size;
        const auto read_length = static_cast<std::remove_cv_t<decltype(length)>>(
            read_end - read_offset);
        if ((read_offset == offset) && (read_length == wanted)) {
            return read(buffer, offset, wanted, transferred);
        }

        auto storage = wil::unique_virtualalloc_ptr<BYTE>(static_cast<BYTE *>(
            VirtualAlloc(nullptr, read_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
        if (!storage) {
            return failure(GetLastError());
        }
        auto device_transferred = std::remove_cv_t<decltype(length)>{};
        const auto status = read(
            storage.get(), read_offset, read_length, &device_transferred);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        const auto prefix = static_cast<std::remove_cv_t<decltype(length)>>(
            offset - read_offset);
        if (device_transferred <= prefix) {
            return STATUS_END_OF_FILE;
        }
        *transferred = std::min(wanted, device_transferred - prefix);
        std::memcpy(buffer, storage.get() + prefix, *transferred);
        return STATUS_SUCCESS;
    }

    static auto GetFileInfo(FSP_FILE_SYSTEM *, void *const context,
        FSP_FSCTL_FILE_INFO *const info) noexcept {
        *info = context == nullptr
            ? kRootInfo
            : static_cast<const DeviceFile *>(context)->info;
        return STATUS_SUCCESS;
    }

    static auto ReadDirectory(FSP_FILE_SYSTEM *const fs, void *const context, wchar_t *,
        wchar_t *const marker, void *const buffer, const ULONG length,
        ULONG *const transferred) noexcept {
        return NtCallback([&] {
            *transferred = 0;
            if (context != nullptr) {
                return STATUS_NOT_A_DIRECTORY;
            }
            const auto &self = Self(fs);
            auto current = self.files_.begin();
            if (marker != nullptr) {
                current = self.files_.find(Lowercase(marker));
                if (current != self.files_.end()) {
                    ++current;
                }
            }
            alignas(FSP_FSCTL_DIR_INFO) auto storage = std::array<BYTE,
                sizeof(FSP_FSCTL_DIR_INFO) + kMaxNameLength * sizeof(wchar_t)>{};
            auto *const info = reinterpret_cast<FSP_FSCTL_DIR_INFO *>(storage.data());
            for (; current != self.files_.end(); ++current) {
                const auto &file = current->second;
                const auto name_bytes = file.name.size() * sizeof(wchar_t);
                info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + name_bytes);
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

    static const FSP_FILE_SYSTEM_INTERFACE interface_;
    DeviceFiles files_;
    const std::vector<BYTE> security_;
    Mount mount_;
    FSP_FILE_SYSTEM *fs_ = nullptr;
};

const auto DeviceFs::interface_ = FSP_FILE_SYSTEM_INTERFACE{
    .GetVolumeInfo = GetVolumeInfo,
    .GetSecurityByName = GetSecurityByName,
    // WinFsp requires Create, Open, and Overwrite callbacks as a group.
    .Create = [](auto...) { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Open = Open,
    .Overwrite = [](auto...) { return STATUS_MEDIA_WRITE_PROTECTED; },
    .Read = Read,
    .GetFileInfo = GetFileInfo,
    .GetSecurity = GetSecurity,
    .ReadDirectory = ReadDirectory,
};

auto *g_stop_event = HANDLE{};
auto *g_stopped_event = HANDLE{};

auto WINAPI ControlHandler(const DWORD event) -> BOOL {
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
    auto security = MakeSecurityDescriptor(options.read_user);
    auto files = DeviceFiles{};
    for (auto i = std::size_t{}; i < options.mappings.size(); ++i) {
        const auto &mapping = options.mappings[i];
        files.emplace(Lowercase(mapping.name),
            OpenDevice(mapping, options.extended_dasd, i + 1));
    }

    auto stop_event = wil::unique_event_nothrow{};
    auto already_exists = bool{};
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

    auto wait = WAIT_FAILED;
    auto wait_error = DWORD{ERROR_GEN_FAILURE};
    {
        auto filesystem = DeviceFs(
            std::move(files), std::move(security), options.mount);
        filesystem.Start();

        g_stop_event = stop_event.get();
        g_stopped_event = stopped_event.get();
        if (!SetConsoleCtrlHandler(ControlHandler, TRUE)) {
            WinError("could not install console handler");
        }

        std::wcout << L"devicefs: mounted " << options.mappings.size() << L" device(s) at "
                   << options.mount.value << L"; read access: " << options.read_user
                   << L"; stop event: " << options.stop_event << L"\n";
        wait = WaitForSingleObject(stop_event.get(), INFINITE);
        if (wait == WAIT_FAILED) {
            wait_error = GetLastError();
        }
    }
    SetEvent(stopped_event.get());
    // The registered handler lives until process exit, so its handles must as well.
    stop_event.release();
    stopped_event.release();
    if (wait != WAIT_OBJECT_0) {
        WinError("shutdown wait failed", wait_error);
    }
    return 0;
}

} // namespace

auto wmain(const int argc, wchar_t **const argv) -> int {
    try {
        auto options = Options{};
        try {
            options = ParseArgs(argc, argv);
        } catch (const std::invalid_argument &error) {
            std::wcerr << L"devicefs: " << error.what() << L"\n\n";
            Usage(std::wcerr);
            return 2;
        }
        if (options.help) {
            Usage(std::wcout);
            return 0;
        }
        CheckNt(FspLoad(nullptr), "could not load WinFsp DLL");
        return Run(options);
    } catch (const std::runtime_error &error) {
        std::wcerr << L"devicefs: " << error.what() << L"\n";
        return 1;
    }
}
