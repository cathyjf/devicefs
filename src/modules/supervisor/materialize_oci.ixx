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
#include <objbase.h>
#include <sddl.h>

#include <wil/stl.h>
#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/token_helpers.h>
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

using namespace std::string_literals;
using namespace std::string_view_literals;

constexpr auto kWslRegistration =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss";
constexpr auto kOciLayerDigestFile = "oci-layer-digest";

auto SetDefaultTokenAcl() {
    // WSL creates the distribution directory while impersonating its caller,
    // without supplying a security descriptor. Our WSL parent directory has
    // no inheritable grants, so Windows takes the new directory's permissions
    // from the caller token's default DACL:
    // <https://github.com/microsoft/WSL/blob/556440f392aef150dc2e8d39152b20a4d7d5b83c/src/windows/service/exe/LxssUserSession.cpp#L1499-L1505>
    // <https://learn.microsoft.com/en-us/windows/win32/secauthz/dacl-for-a-new-object>.
    //
    // `CreateProcessWithLogonW` uses the installer's logon SID even though the
    // materializer runs as the backup account. A grant to that session can
    // therefore let the installing administrator read the distribution without
    // elevation. Replacing this process's defaults before launching WSL gives
    // newly created objects the intended permissions without rewriting the
    // imported filesystem's ACLs.
    // <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw#remarks>.
    auto token = wil::unique_handle{};
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, token.addressof())) {
        WinError("could not open the process token to set its default ACL");
    }
    auto user = wil::unique_tokeninfo_ptr<TOKEN_USER>{};
    if (const auto result = wil::get_token_information_nothrow(user, token.get());
        FAILED(result)) {
        WinError("could not identify the current user for the default token ACL",
            ExplicitWin32Error::FromHresult(result));
    }
    auto sid = wil::unique_hlocal_ansistring{};
    if (!ConvertSidToStringSidA(user->User.Sid, sid.addressof())) {
        WinError("could not format the current user's SID for the default token ACL");
    }
    // The materializer runs as the backup user. Only that user, SYSTEM, and the
    // built-in Administrators group receive full control. These grants are not
    // inheritable; the token supplies them separately when each object needs
    // default permissions. The default owner is a separate token field, which
    // is also set to the backup user below.
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            std::format("D:(A;;GA;;;{})(A;;GA;;;SY)(A;;GA;;;BA)", sid.get()).c_str(),
            SDDL_REVISION_1, descriptor.addressof(), nullptr)) {
        WinError("could not create the security descriptor for the default token ACL");
    }
    auto defaults = TOKEN_DEFAULT_DACL{};
    auto present = BOOL{};
    auto defaulted = BOOL{};
    if (!GetSecurityDescriptorDacl(
            descriptor.get(), &present, &defaults.DefaultDacl, &defaulted)) {
        WinError("could not extract the DACL from the new default token security descriptor");
    }
    if (!SetTokenInformation(token.get(), TokenDefaultDacl,
            &defaults, sizeof(defaults))) {
        WinError("could not set the process token's default DACL");
    }
    auto owner = TOKEN_OWNER{.Owner = user->User.Sid};
    if (!SetTokenInformation(token.get(), TokenOwner, &owner, sizeof(owner))) {
        WinError("could not set the process token's default owner");
    }
}

[[nodiscard]] auto FindDistribution(const std::string_view distribution)
    -> wil::shared_hkey {
    auto registrations = wil::unique_hkey{};
    if (const auto result = wil::reg::open_unique_key_nothrow(
            HKEY_CURRENT_USER, kWslRegistration, registrations);
        wil::reg::is_registry_not_found(result)) {
        return wil::shared_hkey{};
    } else if (FAILED(result)) {
        WinError("could not open WSL registrations to find distribution '{}'",
            distribution, ExplicitWin32Error::FromHresult(result));
    }
    const auto requested = std::filesystem::path{distribution}.wstring();
    auto iterator = wil::reg::key_heap_string_nothrow_iterator{registrations.get()};
    for (; !iterator.at_end(); ++iterator) {
        auto name = wil::unique_cotaskmem_string{};
        if (const auto result = wil::reg::get_value_string_nothrow(
                registrations.get(), iterator->name.get(), L"DistributionName", name);
            wil::reg::is_registry_not_found(result)) {
            continue;
        } else if (FAILED(result)) {
            WinError("could not read the WSL distribution name in registration '{}'",
                std::wstring_view{iterator->name.get()},
                ExplicitWin32Error::FromHresult(result));
        }
        if (CompareStringOrdinal(name.get(), -1, requested.c_str(), -1, TRUE) != CSTR_EQUAL) {
            continue;
        }
        auto registration = wil::shared_hkey{};
        if (const auto result = wil::reg::open_shared_key_nothrow(
                registrations.get(), iterator->name.get(), registration,
                wil::reg::key_access{KEY_QUERY_VALUE | KEY_SET_VALUE});
            FAILED(result)) {
            WinError("could not open the registration of WSL distribution '{}'",
                distribution, ExplicitWin32Error::FromHresult(result));
        }
        return registration;
    }
    if (const auto result = iterator.last_error(); FAILED(result)) {
        WinError("could not enumerate WSL registrations to find distribution '{}'",
            distribution, ExplicitWin32Error::FromHresult(result));
    }
    return wil::shared_hkey{};
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
    RunCommand(std::array{
        tar.string(), "-xOf"s, archive.string(), "--"s, std::string{member},
    }, file.get());
}

[[nodiscard]] auto BlobMember(std::string digest) {
    std::ranges::replace(digest, ':', '/');
    return std::format("blobs/{}", digest);
}

[[nodiscard]] auto OciLayerDigest(
    const winrt::Windows::Data::Json::JsonObject &manifest,
    const std::string_view image) -> std::optional<std::string> {
    const auto layers = manifest.GetNamedArray(L"layers");
    if (layers.Size() != 1) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: OCI image '{}' has {} layers; skipping import\n",
            image, layers.Size());
        return std::nullopt;
    }
    return winrt::to_string(layers.GetObjectAt(0).GetNamedString(L"digest"));
}

[[nodiscard]] auto ReadOciLayerDigest(
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
        return OciLayerDigest(read_metadata(BlobMember(winrt::to_string(
            manifests.GetObjectAt(0).GetNamedString(L"digest")))), archive.string());
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

[[nodiscard]] auto DownloadGhcrRootfsIfChanged(
    const std::filesystem::path &rootfs,
    const std::optional<std::string> &previous_digest) -> std::optional<std::string> {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Headers;

    constexpr auto registry_host = L"ghcr.io"sv;
    constexpr auto github_username = L"cathyjf"sv;
    constexpr auto image_name = L"devicefs-wsl"sv;
    constexpr auto image_tag = L"latest"sv;
    const auto repository = std::format(L"{}/{}", github_username, image_name);
    const auto repository_url = std::format(L"https://{}/v2/{}", registry_host, repository);
    const auto image = winrt::to_string(std::format(
        L"{}/{}:{}", registry_host, repository, image_tag));
    const auto architecture = NativeMachineArchitecture() == IMAGE_FILE_MACHINE_ARM64
        ? L"arm64"sv : L"amd64"sv;
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
    const auto layer = [&]() -> std::optional<std::pair<Uri, std::string>> {
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
                return std::pair{Uri{std::format(
                    L"{}/blobs/{}", repository_url,
                    std::wstring_view{winrt::to_hstring(*digest)})}, *digest};
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
    if (!layer) {
        return std::nullopt;
    }
    const auto &[layer_url, digest] = *layer;
    if (previous_digest && (digest == *previous_digest)) {
        return digest;
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: downloading the linux/{} root filesystem from '{}'\n",
        winrt::to_string(architecture), image);
    const auto bytes = DownloadFile(client, layer_url, rootfs);
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: downloaded the root filesystem from '{}' ({} bytes)\n",
        image, bytes);
    return digest;
}

auto ReplaceDistribution(
    const std::string_view distribution,
    const std::string_view replacement,
    const wil::weak_hkey previous,
    const std::filesystem::path &previous_directory,
    const std::filesystem::path &executable) {
    const auto registration = FindDistribution(replacement);
    if (!registration) {
        throw std::runtime_error(std::format(
            "could not find the imported WSL distribution '{}'", replacement));
    }
    const auto retired = std::format("devicefs-old-{}", UniqueName());
    const auto retired_name = std::filesystem::path{retired}.wstring();
    const auto canonical_name = std::filesystem::path{distribution}.wstring();
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: replacing WSL distribution '{}' with '{}'\n",
        distribution, replacement);

    // WSL resolves distribution names by reading `DistributionName` from each
    // registration, so changing that value gives the replacement its canonical
    // name without changing either registration's GUID or filesystem location:
    // <https://github.com/microsoft/WSL/blob/556440f392aef150dc2e8d39152b20a4d7d5b83c/src/windows/service/exe/LxssUserSession.cpp#L1262-L1297>.
    // Both handles and names are ready before the first write to minimize the
    // interval with no canonical registration. The two writes are not atomic;
    // interruption can leave temporary distributions for the administrator to
    // remove. A later install can import afresh if the canonical name is absent.
    if (const auto result = wil::reg::set_value_string_nothrow(
            previous.lock().get(), L"DistributionName", retired_name.c_str());
        FAILED(result)) {
        WinError("could not rename WSL distribution '{}' to '{}'",
            distribution, retired, ExplicitWin32Error::FromHresult(result));
    }
    if (const auto result = wil::reg::set_value_string_nothrow(
            registration.get(), L"DistributionName", canonical_name.c_str());
        FAILED(result)) {
        WinError("could not rename WSL distribution '{}' to '{}'",
            replacement, distribution, ExplicitWin32Error::FromHresult(result));
    }

    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: unregistering old WSL distribution '{}'\n", retired);
    try {
        RunCommand(std::array{
            executable.string(), "--unregister"s, retired,
        }, GetStdHandle(STD_OUTPUT_HANDLE));
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: WSL distribution '{}' was replaced, but could not "
            "unregister old distribution '{}': {}\n", distribution, retired, error.what());
        return;
    }
    // WSL removes its own files during unregistration, but leaves a nonempty
    // base directory. Remove our digest and then the directory only if empty;
    // any other files left there remain available for administrator inspection.
    // <https://github.com/microsoft/WSL/blob/556440f392aef150dc2e8d39152b20a4d7d5b83c/src/windows/service/exe/LxssUserSession.cpp#L3064-L3177>.
    try {
        std::filesystem::remove(previous_directory / kOciLayerDigestFile);
        std::filesystem::remove(previous_directory);
    } catch (const std::filesystem::filesystem_error &error) {
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: could not remove old WSL distribution directory '{}': {}\n",
            previous_directory.string(), error.what());
    }
}

} // namespace

export [[nodiscard]] auto MaterializeOci(
    const std::string_view distribution,
    const std::optional<std::filesystem::path> &oci) -> bool {
    const auto previous = FindDistribution(distribution);
    const auto previous_directory = [&]() -> std::filesystem::path {
        if (!previous) {
            return {};
        }
        auto directory = wil::unique_cotaskmem_string{};
        if (const auto result = wil::reg::get_value_string_nothrow(
                previous.get(), L"BasePath", directory);
            FAILED(result)) {
            WinError("could not read the directory of WSL distribution '{}'",
                distribution, ExplicitWin32Error::FromHresult(result));
        }
        return directory.get();
    }();
    const auto previous_digest = [&]() -> std::optional<std::string> {
        if (!previous || oci) {
            return std::nullopt;
        }
        // Missing or unreadable metadata does not establish which filesystem
        // was imported. A fresh import restores both the image and its record.
        auto file = std::ifstream{previous_directory / kOciLayerDigestFile};
        auto digest = std::string{};
        if (!std::getline(file, digest) || digest.empty()) {
            return std::nullopt;
        }
        return digest;
    }();

    SetDefaultTokenAcl();
    const auto temporary = TemporaryDirectory{
        std::filesystem::temp_directory_path() /
            std::format("devicefs-oci-{}", UniqueName())};
    const auto rootfs = temporary.Path() / "rootfs.tar";
    const auto digest = [&]() -> std::optional<std::string> {
        if (!oci) {
            return DownloadGhcrRootfsIfChanged(rootfs, previous_digest);
        }
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
        const auto layer = ReadOciLayerDigest(tar, archive, temporary.Path());
        if (!layer) {
            return std::nullopt;
        }
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: extracting the root filesystem from '{}'\n",
            archive.string());
        ExtractArchiveMember(tar, archive, BlobMember(*layer), rootfs);
        return layer;
    }();
    if (!digest) {
        return false;
    }
    if (!oci && (digest == previous_digest)) {
        devicefs::WriteToStream(devicefs::stdout,
            "backup-supervisor: WSL distribution '{}' already has OCI layer '{}'\n",
            distribution, *digest);
        return true;
    }

    const auto executable = WslExecutablePath();
    // Temporary registration names have a fixed length so upgrading does not
    // add a suffix that could exceed WSL's limit for a valid canonical name.
    const auto import_name = previous
        ? std::format("devicefs-new-{}", UniqueName()) : std::string{distribution};
    // Each import gets a separate directory so an upgrade can materialize a
    // replacement alongside the existing distribution. Including the
    // distribution name keeps these directories recognizable to administrators.
    const auto installation = ResolvePersistentPaths().wsl /
        std::format("{}-{}", distribution, UniqueName());

    // WSL opens its welcome window after importing a Windows user's first
    // distribution. Marking that user's welcome experience complete before
    // import prevents the window from interrupting unattended installation.
    // Microsoft's developer setup uses the same registry setting:
    // <https://github.com/microsoft/WindowsDeveloperConfig/blob/b5561d14cac689ec5256a99abc99e474e73766b1/windows-dev-config/dev-config.winget#L178-L181>.
    if (const auto result = wil::reg::set_value_dword_nothrow(
            HKEY_CURRENT_USER, kWslRegistration, L"OOBEComplete", 1);
        FAILED(result)) {
        devicefs::WriteToStream(devicefs::stderr,
            L"backup-supervisor: could not suppress the WSL welcome window "
            L"by setting 'HKCU\\{}\\OOBEComplete' (Windows error 0x{:08x})\n",
            std::wstring_view{kWslRegistration},
            ExplicitWin32Error::FromHresult(result).value);
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: importing WSL1 distribution '{}' into '{}'\n",
        import_name, installation.string());
    RunCommand(std::array{
        executable.string(), "--import"s, import_name,
        installation.string(), rootfs.string(), "--version"s, "1"s,
    }, GetStdHandle(STD_OUTPUT_HANDLE));
    // The layer digest identifies the imported filesystem for subsequent update
    // checks. Record it only after WSL reports a successful import. Failure to
    // record this metadata does not invalidate the imported distribution.
    {
        const auto digest_path = installation / kOciLayerDigestFile;
        auto digest_file = std::ofstream{digest_path, std::ios::binary};
        std::println(digest_file, "{}", *digest);
        digest_file.flush();
        if (!digest_file) {
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: could not record OCI layer digest in '{}'\n",
                digest_path.string());
        }
    }
    if (previous) {
        ReplaceDistribution(distribution, import_name,
            previous, previous_directory, executable);
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: imported WSL1 distribution '{}'\n", distribution);
    return true;
}
