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
#include <wslapi.h>

#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#undef GetObject

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

export module devicefs.supervisor.materialize_oci;

import std;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.account_management;
import devicefs.supervisor.https_download;
import devicefs.supervisor.installation;
import devicefs.supervisor.process_launch;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.winrt_apartment;

#undef stderr
#undef stdout

namespace {

[[nodiscard]] auto IsDistributionRegistered(const std::string_view distribution) {
    // The WSL API is loaded explicitly because Windows SDK 10.0.26100.0
    // supplies `wslapi.h` but not the documented `Wslapi.lib` import library.
    // Delaying this load until materialization also lets `--install` start
    // before the WSL component is installed.
    const auto library = wil::unique_hmodule{LoadLibraryExA(
        "wslapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!library) {
        WinError("could not load 'wslapi.dll' to query WSL distribution '{}'", distribution);
    }
    const auto is_registered =
        GetProcAddressByFunctionDeclaration(library.get(), WslIsDistributionRegistered);
    if (!is_registered) {
        WinError("could not find WslIsDistributionRegistered in wslapi.dll");
    }
    return is_registered(std::filesystem::path{distribution}.c_str()) != FALSE;
}

auto RunCommand(
    const std::span<const std::string> arguments,
    const HANDLE standard_output) {
    const auto &executable = arguments.front();
    auto command = wil::ArgvToCommandLine(arguments);
    const auto input = wil::unique_hfile{CreateFileA(
        "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!input) {
        WinError("could not open NUL for command '{}'", command);
    }
    const auto operation = std::format("could not launch '{}'", executable);
    const auto process = StartProcessWithHandles(
        input.get(), standard_output, GetStdHandle(STD_ERROR_HANDLE),
        [&](STARTUPINFOA *const startup, PROCESS_INFORMATION *const information) {
            return CreateProcessA(executable.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr, nullptr, startup, information);
        }, wil::zstring_view{operation});
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        WinError("could not wait for command '{}'", command);
    }
    auto exit_code = DWORD{};
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        WinError("could not obtain the exit code for command '{}'", command);
    }
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "command '{}' failed with exit code 0x{:08x}",
            command, exit_code));
    }
}

auto ExtractArchiveMember(
    const std::filesystem::path &tar,
    const std::filesystem::path &archive,
    const std::string_view member,
    const std::filesystem::path &destination) {
    const auto file = wil::unique_hfile{CreateFileW(destination.c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        WinError("could not create OCI extraction file '{}'",
            std::wstring_view{destination.native()});
    }
    // OCI members are emitted to the handle above rather than restored at
    // their archive paths. The layer therefore remains a tar archive for WSL
    // to unpack with Linux filesystem semantics.
    RunCommand(std::to_array<std::string>({
        tar.string(), "-xOf", archive.string(), "--", std::string{member},
    }), file.get());
}

[[nodiscard]] auto BlobMember(const winrt::hstring &digest) {
    auto member = winrt::to_string(digest);
    std::ranges::replace(member, ':', '/');
    return std::format("blobs/{}", member);
}

[[nodiscard]] auto OciLayerDigest(
    const winrt::Windows::Data::Json::JsonObject &manifest,
    const std::string_view image) -> std::optional<winrt::hstring> {
    const auto layers = manifest.GetNamedArray(L"layers");
    if (layers.Size() != 1) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: OCI image '{}' has {} layers; skipping import\n",
            image, layers.Size());
        return std::nullopt;
    }
    return layers.GetObjectAt(0).GetNamedString(L"digest");
}

[[nodiscard]] auto ReadOciLayerMember(
    const std::filesystem::path &tar,
    const std::filesystem::path &archive,
    const std::filesystem::path &directory) -> std::optional<std::string> {
    const auto apartment = WinrtApartment{
        "could not initialize WinRT to read the OCI image metadata"};
    const auto read_metadata = [&](const std::string_view member) {
        const auto path = directory / "metadata.json";
        ExtractArchiveMember(tar, archive, member, path);
        auto file = std::ifstream{path, std::ios::binary};
        if (!file.is_open()) {
            throw std::runtime_error(std::format(
                "could not open OCI metadata '{}'", path.string()));
        }
        const auto source = std::string{std::istreambuf_iterator<char>{file}, {}};
        if (file.bad()) {
            throw std::runtime_error(std::format(
                "could not read OCI metadata '{}'", path.string()));
        }
        return winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(source));
    };
    try {
        const auto manifests = read_metadata("index.json").GetNamedArray(L"manifests");
        const auto digest = OciLayerDigest(read_metadata(BlobMember(
            manifests.GetObjectAt(0).GetNamedString(L"digest"))), archive.string());
        return digest ? std::optional{BlobMember(*digest)} : std::nullopt;
    } catch (const winrt::hresult_error &error) {
        WinError("could not read OCI image metadata from '{}': {}",
            std::wstring_view{archive.native()}, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}

[[nodiscard]] auto ReadRegistryJson(
    const winrt::Windows::Web::Http::HttpClient &client,
    const winrt::Windows::Foundation::Uri &url) {
    try {
        const auto response = client.GetAsync(url).get();
        response.EnsureSuccessStatusCode();
        return winrt::Windows::Data::Json::JsonObject::Parse(
            response.Content().ReadAsStringAsync().get());
    } catch (const winrt::hresult_error &error) {
        WinError("could not read OCI registry response from '{}': {}",
            std::wstring_view{url.AbsoluteUri()}, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
}

[[nodiscard]] auto DownloadGhcrRootfs(const std::filesystem::path &rootfs) -> bool {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Headers;

    constexpr auto registry_host = std::wstring_view{L"ghcr.io"};
    constexpr auto github_username = std::wstring_view{L"cathyjf"};
    constexpr auto image_name = std::wstring_view{L"devicefs-wsl"};
    constexpr auto image_tag = std::wstring_view{L"latest"};
    const auto repository = std::format(L"{}/{}", github_username, image_name);
    const auto repository_url = std::format(L"https://{}/v2/{}", registry_host, repository);
    const auto image = winrt::to_string(std::format(
        L"{}/{}:{}", registry_host, repository, image_tag));
    const auto architecture = NativeMachineArchitecture() == IMAGE_FILE_MACHINE_ARM64
        ? std::wstring_view{L"arm64"} : std::wstring_view{L"amd64"};
    const auto apartment = WinrtApartment{
        "could not initialize WinRT to download the WSL root filesystem",
        RO_INIT_MULTITHREADED};
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: querying OCI image '{}' for linux/{}\n",
        image, winrt::to_string(architecture));
    const auto client = [&] {
        try {
            const auto client = HttpClient{};
            client.DefaultRequestHeaders().UserAgent().ParseAdd(L"backup-supervisor");
            // Public GHCR images still require a pull token. This exchange is
            // anonymous; the resulting token authorizes the registry requests.
            const auto token = ReadRegistryJson(client, Uri{std::format(
                L"https://{}/token?service={}&scope=repository:{}:pull",
                registry_host, registry_host, repository)}).GetNamedString(L"token");
            client.DefaultRequestHeaders().Authorization(
                HttpCredentialsHeaderValue{L"Bearer", token});
            client.DefaultRequestHeaders().Accept().ParseAdd(
                L"application/vnd.oci.image.index.v1+json, "
                L"application/vnd.oci.image.manifest.v1+json");
            return client;
        } catch (const winrt::hresult_error &error) {
            WinError("could not obtain anonymous pull authorization for '{}': {}",
                image, std::wstring_view{error.message()},
                ExplicitWin32Error::FromHresult(error.code()));
        }
    }();
    const auto layer_url = [&]() -> std::optional<Uri> {
        try {
            const auto index = ReadRegistryJson(client, Uri{std::format(
                L"{}/manifests/{}", repository_url, image_tag)});
            for (const auto &value : index.GetNamedArray(L"manifests")) {
                const auto descriptor = value.GetObject();
                const auto platform = descriptor.GetNamedObject(L"platform");
                if ((platform.GetNamedString(L"os") != L"linux") ||
                    (platform.GetNamedString(L"architecture") != architecture)) {
                    continue;
                }
                const auto manifest_url = Uri{std::format(
                    L"{}/manifests/{}", repository_url,
                    std::wstring_view{descriptor.GetNamedString(L"digest")})};
                const auto digest = OciLayerDigest(
                    ReadRegistryJson(client, manifest_url), image);
                if (!digest) {
                    return std::nullopt;
                }
                return Uri{std::format(
                    L"{}/blobs/{}", repository_url,
                    std::wstring_view{*digest})};
            }
            throw std::runtime_error(std::format(
                "OCI image '{}' has no suitable linux/{} manifest",
                image, winrt::to_string(architecture)));
        } catch (const winrt::hresult_error &error) {
            WinError("could not read OCI image metadata for '{}': {}",
                image, std::wstring_view{error.message()},
                ExplicitWin32Error::FromHresult(error.code()));
        }
    }();
    if (!layer_url) {
        return false;
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: downloading the linux/{} root filesystem from '{}'\n",
        winrt::to_string(architecture), image);
    const auto bytes = DownloadFile(client, *layer_url, rootfs);
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: downloaded the root filesystem from '{}' ({} bytes)\n",
        image, bytes);
    return true;
}

} // namespace

export [[nodiscard]] auto MaterializeOci(
    const std::string_view distribution,
    const std::optional<std::filesystem::path> &oci) -> bool {
    if (IsDistributionRegistered(distribution)) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: WSL distribution '{}' is already registered\n",
            distribution);
        return true;
    }

    const auto temporary = TemporaryDirectory{
        std::filesystem::temp_directory_path() /
            std::format("devicefs-oci-{}", UniqueName())};
    const auto rootfs = temporary.Path() / "rootfs.tar";
    if (oci) {
        const auto tar = [] {
            auto directory = std::wstring{};
            if (const auto result = wil::GetSystemDirectoryW(directory); FAILED(result)) {
                WinError("could not find the Windows directory containing tar.exe",
                    ExplicitWin32Error::FromHresult(result));
            }
            return std::filesystem::path{directory} / "tar.exe";
        }();
        const auto archive = std::filesystem::absolute(*oci);
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: reading OCI image '{}' for WSL distribution '{}'\n",
            archive.string(), distribution);
        const auto layer = ReadOciLayerMember(tar, archive, temporary.Path());
        if (!layer) {
            return false;
        }
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: extracting the root filesystem from '{}'\n",
            archive.string());
        ExtractArchiveMember(tar, archive, *layer, rootfs);
    } else if (!DownloadGhcrRootfs(rootfs)) {
        return false;
    }

    const auto executable = WslExecutablePath();
    // Each import gets a separate directory so an upgrade can materialize a
    // replacement alongside the existing distribution. Including the
    // distribution name keeps these directories recognizable to administrators.
    const auto installation = WslDistributionDirectory() /
        std::format("{}-{}", distribution, UniqueName());
    std::filesystem::create_directories(installation.parent_path());

    // WSL opens its welcome window after importing a Windows user's first
    // distribution. Marking that user's welcome experience complete before
    // import prevents the window from interrupting unattended installation.
    // Microsoft's developer setup uses the same registry setting:
    // <https://github.com/microsoft/WindowsDeveloperConfig/blob/b5561d14cac689ec5256a99abc99e474e73766b1/windows-dev-config/dev-config.winget#L178-L181>.
    constexpr auto wsl_registration =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss";
    if (const auto result = wil::reg::set_value_dword_nothrow(
            HKEY_CURRENT_USER, wsl_registration, L"OOBEComplete", 1);
        FAILED(result)) {
        devicefs::WriteToStream(devicefs::stderr,
            L"backup-supervisor: could not suppress the WSL welcome window "
            L"by setting 'HKCU\\{}\\OOBEComplete' (Windows error 0x{:08x})\n",
            std::wstring_view{wsl_registration},
            ExplicitWin32Error::FromHresult(result).value);
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: importing WSL1 distribution '{}' into '{}'\n",
        distribution, installation.string());
    RunCommand(std::to_array<std::string>({
        executable.string(), "--import", std::string{distribution},
        installation.string(), rootfs.string(), "--version", "1",
    }), GetStdHandle(STD_OUTPUT_HANDLE));
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: imported WSL1 distribution '{}'\n", distribution);
    return true;
}
