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
#include <aclapi.h>
#include <bcrypt.h>

#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>

#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.native_backup:backup_viewer;

import std;
import :devicefs_process;
import :internal;
import :pbs;
import :port_selection;
import :privileges;
import :vhdx_attachment;
import devicefs.common;
import devicefs.stream_writer;

namespace internal {

auto GrantUserFullControl(
    const std::filesystem::path &path,
    const PSID user) {
    auto path_text = path.wstring();
    auto current_dacl = PACL{};
    auto raw_descriptor = PSECURITY_DESCRIPTOR{};
    const auto query = GetNamedSecurityInfoW(
        path_text.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &current_dacl, nullptr, &raw_descriptor);
    auto descriptor = wil::unique_hlocal_security_descriptor{raw_descriptor};
    if (query != ERROR_SUCCESS) {
        WinError("could not read the ACL for view directory '{}'",
            std::wstring_view{path.native()},
            ExplicitWin32Error{query});
    }
    if (current_dacl == nullptr) {
        // A null DACL already grants full access. Replacing it would narrow
        // existing access rather than preserving the inherited policy.
        return;
    }

    auto access = EXPLICIT_ACCESSW{
        .grfAccessPermissions = FILE_ALL_ACCESS,
        .grfAccessMode = GRANT_ACCESS,
        .grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT,
    };
    BuildTrusteeWithSidW(&access.Trustee, user);
    auto raw_acl = PACL{};
    const auto merge = SetEntriesInAclW(
        1, &access, current_dacl, &raw_acl);
    auto merged_acl = wil::unique_hlocal_ptr<ACL>{raw_acl};
    if (merge != ERROR_SUCCESS) {
        WinError("could not add the view user to the ACL for directory '{}'",
            std::wstring_view{path.native()},
            ExplicitWin32Error{merge});
    }

    // Keep the inherited ACL intact and add one explicit, inheritable grant.
    // In particular, do not protect the DACL or replace its inherited ACEs.
    const auto update = SetNamedSecurityInfoW(
        path_text.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, merged_acl.get(), nullptr);
    if (update != ERROR_SUCCESS) {
        WinError("could not grant the view user access to directory '{}'",
            std::wstring_view{path.native()},
            ExplicitWin32Error{update});
    }
}

auto TryRemoveDirectory(
    const std::filesystem::path &path,
    const char *const description) noexcept {
    try {
        static_cast<void>(std::filesystem::remove(path));
    } catch (const std::exception &error) {
        TryWriteError(description, error);
    }
}

class ViewDirectory {
  public:
    ViewDirectory(
        const std::string_view prefix,
        const PSID user)
        : path_{TemporarySystemDirectoryPath(prefix)} {
        if (!CreateDirectoryW(path_.c_str(), nullptr)) {
            WinError("could not create view directory '{}'",
                std::wstring_view{path_.native()});
        }
        try {
            GrantUserFullControl(path_, user);
        } catch (...) {
            TryRemoveDirectory(path_, "view-directory cleanup failed");
            throw;
        }
    }

    ViewDirectory(const ViewDirectory &) = delete;
    auto operator=(const ViewDirectory &)
        -> ViewDirectory & = delete;
    ViewDirectory(ViewDirectory &&) = delete;
    auto operator=(ViewDirectory &&)
        -> ViewDirectory & = delete;

    ~ViewDirectory() {
        TryRemoveDirectory(path_, "view-directory cleanup failed");
    }

    [[nodiscard]] auto Path() const noexcept
        -> const std::filesystem::path & {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] auto GenerateRpcPassword() -> wil::secure_string {
    auto random = std::array<unsigned char, 32>{};
    const auto erase_random =
        wil::SecureZeroMemory_scope_exit(random.data(), random.size());
    const auto status = BCryptGenRandom(
        nullptr, random.data(),
        wil::safe_cast_failfast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        static_assert(sizeof(status) == sizeof(std::uint32_t));
        throw std::runtime_error(std::format(
            "could not generate the view RPC password "
            "(NTSTATUS 0x{:08x})",
            std::bit_cast<std::uint32_t>(status)));
    }

    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto result = wil::secure_string(random.size() * 2, '\0');
    for (auto index = 0uz; index < random.size(); ++index) {
        result[index * 2] = digits.at(random.at(index) >> 4);
        result[index * 2 + 1] = digits.at(random.at(index) & 0x0f);
    }
    return result;
}

auto WaitForViewSession(
    const DeviceFsChild &devicefs,
    const PbsFishOperation &fish,
    const HANDLE cancellation_event) -> void {
    const auto handles = std::array{
        devicefs.Process().process.hProcess,
        fish.Process(),
        cancellation_event,
    };
    const auto result = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(handles.size()),
        handles.data(), FALSE, INFINITE);
    if (result == WAIT_FAILED) {
        WinError("could not wait for the selective-view session");
    }
    if (result == WAIT_OBJECT_0) {
        throw std::runtime_error(std::format(
            "devicefs exited while the backup view was mounted with code {}",
            ProcessExitCode(handles[0])));
    }
    if (result == (WAIT_OBJECT_0 + 1)) {
        throw std::runtime_error(std::format(
            "the Linux view operation exited while the view was mounted "
            "with code {}",
            ProcessExitCode(handles[1])));
    }
    if (result == (WAIT_OBJECT_0 + 2)) {
        return;
    }
    std::unreachable();
}

} // namespace internal

[[nodiscard]] auto RunSelectiveView(
    const HANDLE cancellation_event,
    const std::string_view archive,
    const std::optional<std::string_view> snapshot_override,
    const std::optional<std::string_view> timestamp,
    const std::string_view address,
    const std::optional<std::u8string> &namespace_override) -> int {
    if (internal::CancellationRequested(cancellation_event)) {
        return internal::kCancelledExitCode;
    }

    auto invoking_user = wil::unique_tokeninfo_ptr<TOKEN_USER>{};
    const auto invoking_user_query = wil::get_token_information_nothrow(
        invoking_user, GetCurrentProcessToken());
    if (FAILED(invoking_user_query)) {
        WinError("could not identify the invoking user",
            ExplicitWin32Error::FromHresult(invoking_user_query));
    }
    const auto port = std::to_string(
        internal::SelectTcpPortCandidate());
    auto devicefs_directory = internal::ViewDirectory{
        "devicefs-view", invoking_user->User.Sid};
    const auto devicefs_mount = devicefs_directory.Path() / "view";
    const auto projected_vhdx = devicefs_mount / "view.vhdx";
    auto volume_directory = internal::ViewDirectory{
        "devicefs-view-volume", invoking_user->User.Sid};

    devicefs::WriteToStream(
        devicefs::stdout,
        "Starting selective view.\n"
        "  Image archive: {}\n"
        "  RPC endpoint: {}:{}\n",
        archive, address, port);
    devicefs::WriteToStream(
        devicefs::stdout,
        L"  DeviceFs mount: {}\n"
        L"  Projected VHDX: {}\n"
        L"  View-volume mount: {}\n",
        devicefs_mount.native(), projected_vhdx.native(),
        volume_directory.Path().native());

    auto rpc_password = std::optional<wil::secure_string>{
        internal::GenerateRpcPassword()};
    auto fish = internal::StartViewFish(
        namespace_override, snapshot_override, timestamp, archive, address,
        port, std::string_view{rpc_password->data(), rpc_password->size()});
    auto stop_fish = wil::scope_exit([&] noexcept {
        internal::TryStopPbsFish(fish);
    });
    if (!internal::WaitForSambaReadiness(
            fish, cancellation_event)) {
        devicefs::WriteToStream(
            devicefs::stdout, "Closing the selective-view operation.\n");
        return internal::kCancelledExitCode;
    }
    devicefs::WriteToStream(
        devicefs::stdout,
        "Samba reported that the view RPC service is ready.\n");

    const auto source = std::array{
        internal::DeviceFsSource{
            .name = "view.vhdx",
            .source = std::format(R"(\\\tcp:{}:{})", address, port),
        },
    };
    auto devicefs = internal::DeviceFsChild{
        internal::StartDeviceFs(internal::DeviceFsStartRequest{
            .sources = source,
            .mount_target = devicefs_mount.string(),
            .rpc_password = std::string_view{
                rpc_password->data(), rpc_password->size()},
            .vhdx = true,
        })};
    const auto stop_view = wil::scope_exit([&] noexcept {
        if (devicefs.TryStop()) {
            internal::TryStopPbsFish(fish);
        }
    });
    stop_fish.release();
    rpc_password.reset();
    if (!internal::WaitForDeviceFs(
            devicefs.Process(), cancellation_event)) {
        devicefs::WriteToStream(
            devicefs::stdout, "Closing the selective-view operation.\n");
        return internal::kCancelledExitCode;
    }
    if (internal::WaitForProcess(
            fish.Process(), std::chrono::milliseconds{0})) {
        throw std::runtime_error(std::format(
            "the Linux view operation exited before the VHDX could be "
            "attached with code {}",
            internal::ProcessExitCode(fish.Process())));
    }
    devicefs::WriteToStream(
        devicefs::stdout,
        L"DeviceFs is presenting the projected VHDX at {}.\n",
        projected_vhdx.native());

    constexpr auto privilege_names = std::array{
        wil::zwstring_view(SE_BACKUP_NAME),
        wil::zwstring_view(SE_MANAGE_VOLUME_NAME),
    };
    auto privileges = internal::ProcessPrivilegeEnabler{
        GetCurrentProcess(), privilege_names,
        std::string_view{"the backup-view privileges"}};
    devicefs::WriteToStream(
        devicefs::stdout,
        "\nPreparing the backup-view VHDX attachment:\n");
    auto attached = internal::AttachedVhdx::Attach(
        projected_vhdx, cancellation_event);
    auto mounted = internal::MountedVolume{
        volume_directory.Path(), attached.Root()};

    devicefs::WriteToStream(
        devicefs::stdout,
        L"The selective-view volume is ready.\n"
        L"  Attached volume: {}\n"
        L"  Browse and copy files from: {}\n"
        L"Press Ctrl+C here when you are finished.\n",
        attached.Root(), volume_directory.Path().native());

    internal::WaitForViewSession(
        devicefs, fish, cancellation_event);
    devicefs::WriteToStream(
        devicefs::stdout, "Closing the selective-view operation.\n");
    return internal::kCancelledExitCode;
}
