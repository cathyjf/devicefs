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

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <virtdisk.h>

#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.native_backup:vhdx_attachment;

import std;
import <devicefs/windows_imports.h>;
import <wil/filesystem.h>;
import :internal;
import devicefs.common;
import devicefs.stream_writer;

#undef stderr
#undef stdout

namespace {

using namespace std::chrono_literals;

constexpr auto kShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

[[nodiscard]] auto QueryPhysicalDiskPath(const HANDLE disk) {
    auto bytes = ULONG{};
    const auto query = GetVirtualDiskPhysicalPath(
        disk, &bytes, nullptr);
    if (query != ERROR_INSUFFICIENT_BUFFER) {
        WinError("could not size the VHDX physical path",
            ExplicitWin32Error{query});
    }
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    auto path = std::vector<wchar_t>(
        (std::size_t{bytes} + sizeof(wchar_t) - 1) /
            sizeof(wchar_t));
    const auto status = GetVirtualDiskPhysicalPath(
        disk, &bytes, path.data());
    if (status != ERROR_SUCCESS) {
        WinError("could not obtain the VHDX physical path",
            ExplicitWin32Error{status});
    }
    return std::wstring{path.data()};
}

[[nodiscard]] auto OpenPhysicalDisk(const std::wstring &path) {
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!result) {
        WinError("could not open the attached VHDX physical disk");
    }
    return result;
}

[[nodiscard]] auto QueryDiskNumber(const HANDLE disk) {
    auto number = STORAGE_DEVICE_NUMBER{};
    auto returned = DWORD{};
    if (!DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &number, sizeof(number), &returned, nullptr)) {
        WinError("could not identify the attached VHDX physical disk");
    }
    return number.DeviceNumber;
}

[[nodiscard]] auto QueryVolumeRoot(
    const DWORD disk_number,
    const HANDLE cancellation_event) {
    // VhdxViewer presents exactly one GPT partition. Open that known root and
    // ask Windows for the volume-GUID path of the filesystem mounted there.
    const auto partition_root = std::format(
        LR"(\\?\GLOBALROOT\Device\Harddisk{}\Partition1\)",
        disk_number);
    const auto partition_root_name = wil::zwstring_view{partition_root};
    devicefs::WriteToStream(
        devicefs::stdout,
        L"  Partition root: {}\n"
        L"  Opening its filesystem root.\n",
        partition_root);
    auto partition = [&] {
        constexpr auto retry_interval = 100ms;
        constexpr auto retry_period = 10s;
        const auto retry_deadline =
            std::chrono::steady_clock::now() + retry_period;
        while (true) {
            auto result = wil::unique_hfile{CreateFileW(
                partition_root_name.c_str(), 0,
                kShareMode, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (result) {
                return result;
            }
            if (std::chrono::steady_clock::now() >= retry_deadline) {
                WinError("could not open attached VHDX Partition1 root {}",
                    std::filesystem::path{partition_root}.string());
            }
            const auto wait = WaitForSingleObject(
                cancellation_event,
                wil::safe_cast_failfast<DWORD>(
                    retry_interval.count()));
            if (wait == WAIT_FAILED) {
                WinError("could not wait for the attached filesystem");
            }
            if (wait == WAIT_OBJECT_0) {
                WinError("waiting for the attached filesystem was cancelled",
                    ExplicitWin32Error{ERROR_CANCELLED});
            }
        }
    }();

    devicefs::WriteToStream(devicefs::stdout,
        "  Filesystem root opened.\n"
        "  Querying its volume-GUID name.\n");
    auto volume_root = wil::unique_cotaskmem_string{};
    const auto error = wil::GetFinalPathNameByHandleW(
        partition.get(), volume_root, wil::VolumePrefix::VolumeGuid);
    if (FAILED(error)) {
        WinError("could not obtain the attached VHDX volume name",
            ExplicitWin32Error::FromHresult(error));
    }
    return std::wstring{volume_root.get()};
}

} // namespace

namespace internal {

[[nodiscard]] auto OpenVhdx(
    const std::filesystem::path &path) noexcept
    -> std::expected<wil::unique_handle, DWORD> {
    auto storage_type = VIRTUAL_STORAGE_TYPE{
        .DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
        .VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT,
    };
    auto parameters = OPEN_VIRTUAL_DISK_PARAMETERS{
        .Version = OPEN_VIRTUAL_DISK_VERSION_2,
        .Version2 = {
            .ReadOnly = TRUE,
        },
    };
    auto disk = wil::unique_handle{};
    const auto status = OpenVirtualDisk(
        &storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE,
        OPEN_VIRTUAL_DISK_FLAG{
            OPEN_VIRTUAL_DISK_FLAG_CACHED_IO |
            OPEN_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES |
            OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS |
            OPEN_VIRTUAL_DISK_FLAG_SUPPORT_ENCRYPTED_FILES},
        &parameters, disk.addressof());
    if (status != ERROR_SUCCESS) {
        return std::unexpected{status};
    }
    return std::move(disk);
}

[[nodiscard]] auto AttachVhdx(
    const HANDLE disk,
    const HANDLE cancellation_event) {
    // Use the virtual-disk API's overlapped form so Ctrl+C can cancel a
    // pending attachment without waiting for its ordinary completion. The
    // OVERLAPPED and disk handle must remain alive until cancellation itself
    // completes, so the completion event is still awaited after CancelIoEx.
    auto completion_event = wil::unique_event_nothrow{};
    if (!completion_event.try_create(
            wil::EventOptions::ManualReset, nullptr)) {
        WinError("could not create the VHDX attachment event");
    }
    auto operation = OVERLAPPED{
        .hEvent = completion_event.get(),
    };
    auto parameters = ATTACH_VIRTUAL_DISK_PARAMETERS{
        .Version = ATTACH_VIRTUAL_DISK_VERSION_1,
    };
    const auto status = AttachVirtualDisk(
        disk, nullptr,
        ATTACH_VIRTUAL_DISK_FLAG{
            ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
            ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER},
        0, &parameters, &operation);
    if (status != ERROR_IO_PENDING) {
        return status;
    }

    const auto events = std::array{
        completion_event.get(), cancellation_event,
    };
    const auto wait = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(events.size()),
        events.data(), FALSE, INFINITE);
    if (wait == WAIT_FAILED) {
        WinError("could not wait for VHDX attachment");
    }
    const auto cancelled = wait == (WAIT_OBJECT_0 + 1);
    if (cancelled) {
        devicefs::WriteToStream(
            devicefs::stdout,
            "Cancellation requested while VHDX attachment was pending; "
            "requesting cancellation of that attachment.\n");
        if (!CancelIoEx(disk, &operation)) {
            const auto error = GetLastError();
            if (error != ERROR_NOT_FOUND) {
                devicefs::WriteToStream(devicefs::stdout,
                    "Could not cancel the pending VHDX attachment "
                    "(Windows error {}); waiting for it to finish.\n",
                    error);
            }
        }
        if (WaitForSingleObject(completion_event.get(), INFINITE) ==
            WAIT_FAILED) {
            WinError("could not wait for VHDX attachment cancellation");
        }
    }

    auto progress = VIRTUAL_DISK_PROGRESS{};
    const auto progress_status = GetVirtualDiskOperationProgress(
        disk, &operation, &progress);
    if (progress_status != ERROR_SUCCESS) {
        return progress_status;
    }
    if (cancelled &&
        (progress.OperationStatus != ERROR_SUCCESS)) {
        return DWORD{ERROR_CANCELLED};
    }
    return progress.OperationStatus;
}

[[nodiscard]] auto QueryAttachedVhdxRoot(
    const HANDLE disk,
    const HANDLE cancellation_event) {
    devicefs::WriteToStream(devicefs::stdout,
        "  VHDX attached.\n"
        "  Querying its physical-disk path.\n");
    const auto physical_path = QueryPhysicalDiskPath(disk);
    devicefs::WriteToStream(devicefs::stdout,
        L"  Attached physical disk: {}\n"
        L"  Opening that physical disk.\n",
        physical_path);
    auto physical_disk = OpenPhysicalDisk(physical_path);
    devicefs::WriteToStream(devicefs::stdout,
        "  Physical disk opened.\n"
        "  Querying its disk number.\n");
    const auto disk_number = QueryDiskNumber(physical_disk.get());
    devicefs::WriteToStream(
        devicefs::stdout,
        "  Disk number: {}\n",
        disk_number);
    auto root = QueryVolumeRoot(
        disk_number, cancellation_event);
    devicefs::WriteToStream(
        devicefs::stdout, L"  Attached volume: {}\n", root);
    return root;
}

[[nodiscard]] auto DetachVhdx(const HANDLE disk) noexcept -> DWORD {
    return DetachVirtualDisk(
        disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
}

class AttachedVhdx {
  public:
    template <typename BeforeFailedAttachmentCleanup>
    [[nodiscard]] static auto Attach(
        const std::filesystem::path &path,
        const HANDLE cancellation_event,
        BeforeFailedAttachmentCleanup &&before_failed_attachment_cleanup) {
        static_assert(std::is_nothrow_invocable_v<
            BeforeFailedAttachmentCleanup>);
        if (CancellationRequested(cancellation_event)) {
            WinError("VHDX attachment was cancelled",
                ExplicitWin32Error{ERROR_CANCELLED});
        }
        devicefs::WriteToStream(
            devicefs::stdout,
            L"  File: {}\n"
            L"  Opening the VHDX.\n",
            path.native());
        auto disk = OpenVhdx(path);
        if (!disk) {
            WinError("could not open VHDX {}",
                path.string(), ExplicitWin32Error{disk.error()});
        }
        devicefs::WriteToStream(
            devicefs::stdout, "  VHDX opened.\n  Attaching the VHDX.\n");
        if (CancellationRequested(cancellation_event)) {
            WinError("VHDX attachment was cancelled",
                ExplicitWin32Error{ERROR_CANCELLED});
        }
        const auto attach_status = AttachVhdx(
            disk->get(), cancellation_event);
        if (attach_status != ERROR_SUCCESS) {
            WinError("could not attach VHDX",
                ExplicitWin32Error{attach_status});
        }
        auto cleanup_failed_attachment = wil::scope_exit([&] noexcept {
            std::invoke(before_failed_attachment_cleanup);
            const auto detach_status = DetachVhdx(disk->get());
            if (detach_status != ERROR_SUCCESS) {
                try {
                    devicefs::WriteToStream(devicefs::stdout,
                        "  VHDX preparation failed after attachment, and "
                        "detaching it failed with Windows error {}. Closing "
                        "its nonpermanent attachment handle.\n",
                        detach_status);
                } catch (const std::exception &) {
                    // Diagnostic output cannot replace attachment cleanup.
                }
            }
            disk->reset();
        });
        if (CancellationRequested(cancellation_event)) {
            WinError("VHDX attachment was cancelled",
                ExplicitWin32Error{ERROR_CANCELLED});
        }
        auto root = QueryAttachedVhdxRoot(
            disk->get(), cancellation_event);
        cleanup_failed_attachment.release();
        return AttachedVhdx{
            std::move(*disk), std::move(root)};
    }

    [[nodiscard]] static auto Attach(
        const std::filesystem::path &path,
        const HANDLE cancellation_event) {
        return Attach(path, cancellation_event, [] noexcept {});
    }

    AttachedVhdx(const AttachedVhdx &) = delete;
    auto operator=(const AttachedVhdx &) -> AttachedVhdx & = delete;
    AttachedVhdx(AttachedVhdx &&) noexcept = default;
    auto operator=(AttachedVhdx &&) -> AttachedVhdx & = delete;

    ~AttachedVhdx() noexcept {
        const auto status = Detach();
        if (status != ERROR_SUCCESS) {
            try {
                WinError("could not detach VHDX",
                    ExplicitWin32Error{status});
            } catch (const std::exception &error) {
                TryWriteError("VHDX cleanup failed", error);
            }
        }
    }

    [[nodiscard]] auto Root() const noexcept
        -> std::wstring_view {
        return root_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return disk_.is_valid();
    }

    [[nodiscard]] auto Detach() noexcept -> DWORD {
        if (!disk_) {
            return ERROR_SUCCESS;
        }
        const auto status = DetachVhdx(disk_.get());
        // The attachment does not use PERMANENT_LIFETIME. Closing the handle
        // is the fallback detach if the explicit request failed.
        disk_.reset();
        return status;
    }

  private:
    AttachedVhdx(
        wil::unique_handle disk,
        std::wstring root) noexcept
        : disk_{std::move(disk)}, root_{std::move(root)} {}

    wil::unique_handle disk_;
    std::wstring root_;
};

class MountedVolume {
  public:
    MountedVolume(
        const std::filesystem::path &directory,
        const std::wstring_view volume_root)
        : mount_path_{directory.native()} {
        if (!mount_path_.ends_with(L'\\')) {
            mount_path_.push_back(L'\\');
        }
        const auto volume_name = std::wstring{volume_root};
        if (!SetVolumeMountPointW(
                mount_path_.c_str(), volume_name.c_str())) {
            WinError("could not mount the attached volume at {}",
                directory.string());
        }
    }

    MountedVolume(const MountedVolume &) = delete;
    auto operator=(const MountedVolume &)
        -> MountedVolume & = delete;
    MountedVolume(MountedVolume &&) = delete;
    auto operator=(MountedVolume &&)
        -> MountedVolume & = delete;

    ~MountedVolume() noexcept {
        try {
            if (!DeleteVolumeMountPointW(mount_path_.c_str())) {
                WinError("could not remove the attached-volume mount point {}",
                    std::filesystem::path{mount_path_}.string());
            }
        } catch (const std::exception &error) {
            TryWriteError("attached-volume mount cleanup failed", error);
        }
    }

  private:
    std::wstring mount_path_;
};

} // namespace internal
