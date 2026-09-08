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
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.h>;
import <winrt/Windows.Foundation.Collections.h>;
import <winrt/Windows.Security.Cryptography.h>;
import <winrt/Windows.Security.Cryptography.Core.h>;
import <winrt/Windows.Storage.h>;
import <winrt/Windows.Storage.Streams.h>;
import <winrt/Windows.Web.Http.h>;
import <winrt/Windows.Web.Http.Headers.h>;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.https_download;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.winrt_apartment;

#undef GetObject
#undef stderr
#undef stdout

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace wil::literals;

namespace {

// Installing WinFsp executes its MSI with administrator privileges. We select
// a specific release and record its SHA-256 digest so `VerifyWinFspMsi` can
// reject a changed download before Windows Installer executes it. Accepting
// another WinFsp installer therefore requires changing these source constants.
// WSL uses Microsoft's current release because Windows already relies on
// Microsoft to supply trusted operating system code.
//
// The WinFsp release page publishes the version and SHA-256 recorded here:
// https://github.com/winfsp/winfsp/releases/tag/v2.2B4
constexpr auto kWinFspVersion = std::array{2u, 2u, 26215u};
constexpr auto kWinFspRelease =
    "https://github.com/winfsp/winfsp/releases/download/v2.2B4"sv;
constexpr auto kWinFspSha256 =
    "2ECB5C89405488A95BBD8A01875E02C48534FD37BBDFD84488F7590464D65944"sv;

[[nodiscard]] auto InstallMsi(
    const std::filesystem::path &path,
    const std::string_view description) -> bool {
    const auto mutex = [description] {
        // Windows Installer rejects overlapping installations rather than
        // waiting. Acquiring this execution mutex first lets us wait for
        // another installation to finish. The mutex stays owned by this thread
        // through `MsiInstallProductW`, which acquires the same mutex
        // recursively. Releasing the mutex before that call would allow
        // another installation to start first.
        // https://devblogs.microsoft.com/setup/waiting-to-install/
        constexpr auto kMsiMutexName = "Global\\_MSIExecute"_zv;
        auto mutex = wil::unique_mutex_nothrow{CreateMutexExA(
            nullptr, kMsiMutexName.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE)};
        if (!mutex) {
            WinError("could not create or open Windows Installer mutex '{}' "
                "before installing {}", std::string_view{kMsiMutexName}, description);
        }
        return mutex;
    }();
    const auto lock = [&mutex, description] {
        if (auto lock = mutex.acquire(nullptr, 0)) {
            return lock;
        }
        auto finished = std::binary_semaphore{0};
        auto reporter = std::jthread{[&finished, description] {
            do {
                devicefs::WriteToStream(devicefs::stdout,
                    "backup-supervisor: waiting for another installation to finish "
                    "before installing {}\n", description);
            } while (!finished.try_acquire_for(10s));
        }};
        auto lock = mutex.acquire();
        finished.release();
        return lock;
    }();
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: installing {}\n", description);
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    // Windows Installer can restart the machine during a silent install.
    // `REBOOT=ReallySuppress` prevents that restart and leaves the caller
    // responsible for reporting any restart requirement after installation.
    const auto error = MsiInstallProductW(path.c_str(), L"REBOOT=ReallySuppress");
    if ((error != ERROR_SUCCESS) && (error != ERROR_SUCCESS_REBOOT_REQUIRED)) {
        WinError("could not install {} from '{}'", description,
            std::wstring_view{path.native()}, ExplicitWin32Error{error});
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: installed {}\n", description);
    if (error == ERROR_SUCCESS_REBOOT_REQUIRED) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: Windows must be restarted to complete "
            "installation of {}\n", description);
        return true;
    }
    return false;
}

auto VerifyWinFspMsi(const std::filesystem::path &path) {
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Security::Cryptography::Core;
    using namespace winrt::Windows::Storage;

    try {
        // The pinned installer is about 2 MiB, so reading the whole file for
        // hashing has a modest memory cost and avoids a separate streaming
        // implementation.
        const auto file = StorageFile::GetFileFromPathAsync(path.native()).get();
        const auto algorithm = HashAlgorithmProvider::OpenAlgorithm(
            HashAlgorithmNames::Sha256());
        const auto digest = algorithm.HashData(FileIO::ReadBufferAsync(file).get());
        if (!CryptographicBuffer::Compare(digest,
                CryptographicBuffer::DecodeFromHexString(
                    winrt::to_hstring(kWinFspSha256)))) {
            throw std::runtime_error(std::format(
                "WinFsp MSI '{}' has SHA-256 {}; expected {}",
                path.string(),
                winrt::to_string(CryptographicBuffer::EncodeToHexString(digest)),
                kWinFspSha256));
        }
    } catch (const winrt::hresult_error &error) {
        WinError("could not verify the SHA-256 of WinFsp MSI '{}': {}",
            std::wstring_view{path.native()}, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}

} // namespace

auto EnsureWinFsp() -> bool {
    // To decide whether an existing WinFsp installation is suitable, we read
    // the product version that its MSI recorded in the `Version` value under
    // `HKLM\SOFTWARE\Classes\Installer\Dependencies\WinFsp`. The separate
    // `HKLM\SOFTWARE\WinFsp` key records installation paths but has no version.
    //
    // WinFsp's installer source enables this version record through its
    // `<dep:Provides>` declaration:
    // https://github.com/winfsp/winfsp/blob/v2.2B4/build/VStudio/installer/Product.wxs#L94-L103
    constexpr auto registration =
        L"SOFTWARE\\Classes\\Installer\\Dependencies\\WinFsp"_zv;
    const auto version = std::format("{}.{}.{}",
        kWinFspVersion[0], kWinFspVersion[1], kWinFspVersion[2]);
    if (IsSuitablePackageInstalled(registration, kWinFspVersion)) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: WinFsp {} or later is already installed\n", version);
        return false;
    }

    const auto apartment = WinrtApartment{
        "could not initialize WinRT to install WinFsp", RO_INIT_MULTITHREADED};
    const auto directory = TemporaryDirectory{
        TemporarySystemDirectoryPath("devicefs-winfsp-package")};
    const auto name = std::format("winfsp-{}.msi", version);
    const auto destination = directory.Path() / name;
    const auto url = std::format("{}/{}", kWinFspRelease, name);
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: downloading pinned WinFsp MSI '{}' from '{}'\n",
        name, url);
    try {
        const auto client = winrt::Windows::Web::Http::HttpClient{};
        const auto bytes = DownloadFile(client,
            winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}, destination);
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: downloaded '{}' ({} bytes)\n", name, bytes);
    } catch (const winrt::hresult_error &error) {
        WinError("could not acquire WinFsp MSI from '{}': {}",
            url, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: verifying the pinned SHA-256 of '{}'\n", name);
    VerifyWinFspMsi(destination);
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: verified the pinned SHA-256 of '{}'\n", name);
    return InstallMsi(destination, std::format("WinFsp MSI '{}'", name));
}

auto InstallWslPackage() -> bool {
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Web::Http;

    // Installation must also work when the existing `wsl.exe` is too old to
    // support `wsl --update`. The release MSI supplies the new executable
    // independently of that command and of the optional WSL Windows component.
    constexpr auto releases_url =
        L"https://api.github.com/repos/microsoft/WSL/releases/latest"sv;
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
        const auto suffix = NativeMachineArchitecture() == IMAGE_FILE_MACHINE_ARM64
            ? L".arm64.msi"sv
            : L".x64.msi"sv;
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

        const auto directory = TemporaryDirectory{
            TemporarySystemDirectoryPath("devicefs-wsl-package")};

        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: downloading WSL release '{}' MSI '{}' from '{}'\n",
            std::wstring_view{release.GetNamedString(L"tag_name")},
            std::wstring_view{name}, std::wstring_view{download_url.AbsoluteUri()});
        const auto destination = directory.Path() / "wsl.msi";
        const auto bytes = DownloadFile(client, download_url, destination);
        devicefs::WriteToStream(devicefs::stdout,
            L"backup-supervisor: downloaded '{}' ({} bytes)\n",
            std::wstring_view{name}, bytes);

        return InstallMsi(destination,
            std::format("WSL MSI '{}'", winrt::to_string(name)));
    } catch (const winrt::hresult_error &error) {
        WinError("could not acquire or install the WSL package from '{}': {}",
            releases_url, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}
