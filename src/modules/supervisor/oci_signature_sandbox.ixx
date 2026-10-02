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

export module devicefs.supervisor.oci_signature_sandbox;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.supervisor.oci_verification;
import devicefs.supervisor.process_launch;
import devicefs.terminal.transcoding;

using namespace std::string_view_literals;

export constexpr auto kVerifyOpenPgpDetachedSignatureOption =
    "--internal-verify-openpgp-detached-signature"sv;

// The worker accepts only the digest and its detached signature. It is
// intended to be run with no AppContainer capabilities, so parsing an
// attacker-controlled OpenPGP packet does not give the parser the
// materializer's filesystem or network access.
// https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation
export auto RunOciSignatureWorker(
    const std::span<const std::string_view> arguments) -> int {
    if (arguments.size() != 2) {
        throw std::invalid_argument(
            std::format("{} requires DIGEST SIGNATURE",
                kVerifyOpenPgpDetachedSignatureOption));
    }
    VerifyOciLayerSignature(arguments[0], arguments[1]);
    return 0;
}

export auto VerifyOciLayerSignatureInSandbox(const std::filesystem::path &supervisor,
    const std::string_view digest,
    const std::string_view signature) -> void {
    // Windows command lines are NUL-terminated. Embedded NUL would otherwise
    // cause the child to verify only a prefix of the supplied input.
    if (digest.contains('\0') || signature.contains('\0')) {
        throw std::runtime_error("OCI signature verification input contains NUL");
    }
    // Parallel verifications share one profile. In Windows 11's `profext.dll`
    // (10.0.26100.1), `EnsureExistingState` deletes incomplete profile storage
    // before recreating it. Concurrent creation can therefore delete another
    // caller's unfinished profile. Static initialization makes callers wait
    // for profile preparation to finish before launching their workers.
    static const auto app_container_sid = [] {
        auto sid = wil::unique_sid{};
        constexpr auto container_name = L"DeviceFs.OpenPgpDetachedSignatureVerifier";
        // The profile provides Windows with per-user AppContainer folders and
        // registry storage. Creating it also returns its SID; an existing profile
        // needs only SID derivation on subsequent invocations.
        // https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer#creating-the-profile
        auto result = CreateAppContainerProfile(container_name,
            L"DeviceFs OpenPGP Detached Signature Verifier",
            L"Verifies DeviceFs OpenPGP detached signatures", nullptr, 0, sid.addressof());
        if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            result = DeriveAppContainerSidFromAppContainerName(container_name, sid.addressof());
        }
        if (FAILED(result)) {
            WinError("could not prepare the OpenPGP detached signature "
                "verifier's AppContainer profile",
                ExplicitHresult{result});
        }
        return sid;
    }();
    auto capabilities = SECURITY_CAPABILITIES{
        .AppContainerSid = app_container_sid.get()};
    const auto executable = devicefs::terminal::Transcode<std::string>(
        supervisor.native());
    auto command = wil::ArgvToCommandLine(std::array{
        std::string_view{executable}, std::string_view{kVerifyOpenPgpDetachedSignatureOption},
        digest, signature});
    auto output_read = wil::unique_handle{};
    auto output_write = wil::unique_handle{};
    if (!CreatePipe(output_read.addressof(), output_write.addressof(), nullptr, 0)) {
        WinError("could not create the OCI signature worker's diagnostic pipe");
    }
    const auto input = wil::unique_hfile{CreateFileA(
        "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!input) {
        WinError("could not open NUL for the OCI signature worker");
    }
    // A less privileged AppContainer also excludes grants to the broad
    // `ALL APPLICATION PACKAGES` group. The child receives no capabilities;
    // it can load the executable and Windows libraries, and write diagnostics.
    // https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer
    auto application_packages_policy = DWORD{PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT};
    const auto process = StartProcessWithHandles(
        input.get(), output_write.get(), output_write.get(),
        [&executable, &command](STARTUPINFOA *const startup,
            PROCESS_INFORMATION *const information) {
            return CreateProcessA(executable.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr, nullptr, startup, information);
        }, wil::zstring_view{"could not launch the OCI signature AppContainer"},
        std::tuple{PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &capabilities, sizeof(capabilities)},
        std::tuple{PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
            &application_packages_policy, sizeof(application_packages_policy)});
    output_write.reset();
    if (const auto result = ReadPipeOutput(output_read.get(),
            ForwardPipeOutput{GetStdHandle(STD_ERROR_HANDLE)});
        result != ERROR_SUCCESS) {
        WinError("could not read the OCI signature worker's diagnostic for "
            "layer '{}'", digest, ExplicitWin32Error{result});
    }
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        WinError("could not wait for the OCI signature worker for layer '{}'",
            digest);
    }
    if (const auto result = ProcessExitCode(process.hProcess);
        result != ERROR_SUCCESS) {
        throw std::runtime_error(std::format(
            "OCI signature verification failed for layer '{}': worker exited "
            "with 0x{:08x}", digest, result));
    }
}
