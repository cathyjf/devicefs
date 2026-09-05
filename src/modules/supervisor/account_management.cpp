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
#include <msi.h>
#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.account_management;

import std;
import <devicefs/windows_imports.h>;
import <wil/filesystem.h>;
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.h>;
import <winrt/Windows.Foundation.Collections.h>;
import <winrt/Windows.Storage.h>;
import <winrt/Windows.Storage.Streams.h>;
import <winrt/Windows.Web.Http.h>;
import <winrt/Windows.Web.Http.Headers.h>;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.winrt_apartment;

#undef GetObject
#undef stderr
#undef stdout

auto InstallWslPackage() -> bool {
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Web::Http;

    // Installation must also work when the existing `wsl.exe` is too old to
    // support `wsl --update`. The release MSI supplies the new executable
    // independently of that command and of the optional WSL Windows component.
    constexpr auto releases_url = std::wstring_view{
        L"https://api.github.com/repos/microsoft/WSL/releases/latest"};
    // The HTTP and storage operations below wait synchronously.
    // C++/WinRT permits these waits only in a multithreaded apartment, so this
    // operation selects that apartment through the shared WinRT lifetime owner.
    const auto apartment = WinrtApartment{
        "could not initialize WinRT to install the WSL package",
        RO_INIT_MULTITHREADED};
    try {
        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: querying the latest WSL release from '{}'\n",
            releases_url);
        const auto client = HttpClient{};
        client.DefaultRequestHeaders().UserAgent().ParseAdd(L"backup-supervisor");
        const auto response = client.GetAsync(Uri{releases_url}).get();
        response.EnsureSuccessStatusCode();
        const auto release = JsonObject::Parse(
            response.Content().ReadAsStringAsync().get());
        const auto suffix = [] {
            auto process_machine = USHORT{};
            auto native_machine = USHORT{};
            if (!IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
                WinError("could not determine the machine architecture for the WSL MSI");
            }
            return native_machine == IMAGE_FILE_MACHINE_ARM64
                ? std::wstring_view{L".arm64.msi"}
                : std::wstring_view{L".x64.msi"};
        }();
        const auto package = [&] {
            for (const auto &value : release.GetNamedArray(L"assets")) {
                const auto asset = value.GetObject();
                if (std::wstring_view{asset.GetNamedString(L"name")}.ends_with(suffix)) {
                    return asset;
                }
            }
            throw std::runtime_error(std::format(
                "WSL release '{}' from '{}' has no MSI with suffix '{}'",
                winrt::to_string(release.GetNamedString(L"tag_name")),
                winrt::to_string(releases_url), winrt::to_string(suffix)));
        }();
        const auto name = package.GetNamedString(L"name");
        const auto download_url = Uri{package.GetNamedString(L"browser_download_url")};
        if (download_url.SchemeName() != L"https") {
            throw std::runtime_error(std::format(
                "WSL MSI '{}' has a download URL that does not use HTTPS: '{}'",
                winrt::to_string(name), winrt::to_string(download_url.AbsoluteUri())));
        }

        const auto directory = TemporarySystemDirectoryPath("devicefs-wsl-package");
        if (!CreateDirectoryW(directory.c_str(), nullptr)) {
            WinError("could not create the WSL package temporary directory '{}'",
                std::wstring_view{directory.native()});
        }
        const auto remove_directory = wil::scope_exit([&] {
            if (const auto result =
                    wil::RemoveDirectoryRecursiveNoThrow(directory.c_str());
                FAILED(result)) {
                devicefs::WriteToStream(devicefs::stderr,
                    L"backup-supervisor: could not remove WSL package temporary "
                    L"directory '{}' (Windows error 0x{:08x})\n",
                    std::wstring_view{directory.native()},
                    ExplicitWin32Error::FromHresult(result).value);
            }
        });

        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: downloading WSL release '{}' MSI '{}' from '{}'\n",
            std::wstring_view{release.GetNamedString(L"tag_name")},
            std::wstring_view{name}, std::wstring_view{download_url.AbsoluteUri()});
        const auto folder = StorageFolder::GetFolderFromPathAsync(directory.native()).get();
        const auto file = folder.CreateFileAsync(L"wsl.msi").get();
        const auto download = client.GetAsync(
            download_url, HttpCompletionOption::ResponseHeadersRead).get();
        download.EnsureSuccessStatusCode();
        const auto bytes = RandomAccessStream::CopyAndCloseAsync(
            download.Content().ReadAsInputStreamAsync().get(),
            file.OpenAsync(FileAccessMode::ReadWrite).get()).get();
        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: downloaded '{}' ({} bytes)\n",
            std::wstring_view{name}, bytes);

        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: installing WSL MSI '{}'\n",
            std::wstring_view{name});
        const auto previous_ui = MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
        const auto restore_ui = wil::scope_exit([previous_ui] {
            MsiSetInternalUI(previous_ui, nullptr);
        });
        // Windows Installer can restart the machine during a silent install.
        // `REBOOT=ReallySuppress` prevents that restart and leaves the caller
        // responsible for reporting any restart requirement after installation.
        const auto error = MsiInstallProductW(file.Path().c_str(), L"REBOOT=ReallySuppress");
        if ((error != ERROR_SUCCESS) && (error != ERROR_SUCCESS_REBOOT_REQUIRED)) {
            WinError("could not install WSL MSI '{}'",
                std::wstring_view{name}, ExplicitWin32Error{error});
        }
        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: installed WSL MSI '{}'\n",
            std::wstring_view{name});
        if (error == ERROR_SUCCESS_REBOOT_REQUIRED) {
            devicefs::WriteToStream(devicefs::stdout,
                "backup-supervisor: Windows must be restarted to complete "
                "installation of the WSL package\n");
            return true;
        }
        return false;
    } catch (const winrt::hresult_error &error) {
        WinError("could not acquire or install the WSL package from '{}': {}",
            releases_url, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}
