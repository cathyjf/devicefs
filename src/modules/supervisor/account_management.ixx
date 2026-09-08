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
#include <DismApi.h>
#include <intrin.h>
#include <lm.h>
#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>

#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#undef stderr
#undef stdout

export module devicefs.supervisor.account_management;

import std;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.process_launch;

using namespace std::string_view_literals;
using namespace wil::literals;

export constexpr auto kMaterializeOciOption = "--materialize-oci"sv;

[[nodiscard]] auto InstallWslPackage() -> bool;
[[nodiscard]] auto EnsureWinFsp() -> bool;

[[nodiscard]] auto IsSuitablePackageInstalled(
    const wil::zwstring_view registration,
    const std::span<const unsigned> minimum_version) -> bool {
    // The package version check compares numeric components and treats omitted
    // trailing components as zero, so `2.7.12` meets a minimum of `2.7.12.0`.
    // However, `std::ranges::lexicographical_compare` considers a matching
    // shorter sequence smaller. The caller therefore omits trailing zeros from
    // `minimum_version` to avoid rejecting an equivalent shorter version.
    auto version = wil::unique_cotaskmem_string{};
    if (const auto result = wil::reg::get_value_string_nothrow(
            HKEY_LOCAL_MACHINE, registration.c_str(), L"Version", version);
        wil::reg::is_registry_not_found(result)) {
        return false;
    } else if (FAILED(result)) {
        WinError("could not read the package version from 'HKLM\\{}'",
            std::wstring_view{registration},
            ExplicitWin32Error::FromHresult(result));
    }

    return !std::ranges::lexicographical_compare(
        std::wstring_view{version.get()} | std::views::split(L'.') |
            std::views::transform([](const auto component) {
                return std::stoul(std::wstring{component.begin(), component.end()});
            }),
        minimum_version);
}

namespace {

constexpr auto kWslRegistration =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI"_zv;

// Ceiling division requires a nonzero divisor. Rounding the quotient up after
// division avoids the overflow possible when adding `divisor - 1` beforehand.
template <std::size_t Dividend, std::size_t Divisor>
    requires (Divisor != 0)
[[nodiscard]] constexpr auto Ceil() {
    return (Dividend / Divisor) + ((Dividend % Divisor) != 0);
}

// Windows can require passwords to be 128 characters long. Repeating the
// encoded random block reaches that length without adding independent random
// substrings that could accidentally contain the account name. The first
// complete block preserves all 256 bits of entropy from the 32 random bytes;
// the repetitions increase the length, not the entropy. The documented maximum
// minimum-password-length setting is 128:
// https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-devicelock#minimumpasswordlength
//
// The inbox Passfilt.dll complexity filter requires three character categories
// and rejects the account name or full-name tokens of at least three characters,
// using case-insensitive comparisons. Padded Base64 supplies a special character
// through its trailing '=' and almost certainly supplies uppercase and lowercase
// letters as well. A fixed prefix is unnecessary because a fresh candidate can
// remedy a rare category failure, and a prefix could itself contain a forbidden
// name that would remain present on every attempt. Windows owns these checks;
// querying or reproducing its policy here would duplicate that responsibility.
// https://learn.microsoft.com/en-us/windows/win32/secmgmt/strong-password-enforcement-and-passfilt-dll
//
// A three-letter ASCII account name has roughly 41 possible matches in the
// 43 Base64 content characters. Either letter case matches, so the approximate
// collision probability is 41 * (2/64)^3 = 0.125%, ignoring overlapping matches
// and the final character's restricted values. For comparison, a three-character
// hexadecimal name occurs in 64 random hexadecimal digits with probability
// approximately 62 / 16^3 = 1.5%. Repeating the padded Base64 block does not add
// account-name matches: every new boundary contains '=', which local account
// names cannot contain. Full-name tokens have different character restrictions,
// so the account-name estimate is not a bound on all possible policy rejections.
// https://learn.microsoft.com/en-us/windows/win32/api/lmaccess/nf-lmaccess-netuseradd#remarks
//
// Each rejected attempt replaces the random bytes and the password in their
// existing buffers. Encoding directly into the final secure string avoids an
// intermediate copy of the secret; returning it transfers its allocation. Both
// buffers are erased when their owners release them, including on failure.
[[nodiscard]] auto GenerateAccountPassword(const auto &retry) -> wil::secure_wstring {
    constexpr auto kPasswordLength = DWORD{128};
    constexpr auto kArbitraryAttemptBound = 10;
    auto random = std::array<unsigned char, 32>{};
    static_assert((random.size() % 3) != 0,
        "The entropy must leave a partial three-byte Base64 group for '=' padding.");
    constexpr auto kEncodedLength = Ceil<random.size(), 3>() * 4;
    static_assert(kEncodedLength < kPasswordLength,
        "The password buffer must fit the Base64 block and its terminating NUL.");
    const auto erase_random =
        wil::SecureZeroMemory_scope_exit(random.data(), random.size());
    auto password = wil::secure_wstring(kPasswordLength, L'\0');
    for (auto attempt = 0; attempt < kArbitraryAttemptBound; ++attempt) {
        const auto status = BCryptGenRandom(
            nullptr, random.data(),
            wil::safe_cast_failfast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            throw std::runtime_error(std::format(
                "could not generate the backup account password "
                "(NTSTATUS 0x{:08x})",
                std::bit_cast<std::uint32_t>(status)));
        }

        // The 128-character destination also has room for the initial encoding:
        // 32 bytes produce 44 padded Base64 characters and a terminating NUL.
        // On success, `encoded_length` excludes that NUL. The wide API writes
        // directly into the UTF-16 password required by the account APIs.
        {
            auto encoded_length = kPasswordLength;
            if (!CryptBinaryToStringW(
                random.data(), CompileTimeCast<DWORD, random.size()>(),
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                password.data(), &encoded_length)) {
                WinError("could not encode the backup account password");
            }
            if (encoded_length != kEncodedLength) {
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
            }
        }
        // The static assertion ensures that the first encoded block fits in the
        // password. Every source index is below `kEncodedLength`, and the loop
        // condition keeps every destination index below `kPasswordLength`.
        for (auto index = kEncodedLength; index < kPasswordLength; ++index) {
            password[index] = password[index % kEncodedLength];
        }
        // Both NetUser APIs document `NERR_PasswordTooShort` for password-policy
        // failures beyond length alone. The password-filter contract also names
        // `ERROR_ILL_FORMED_PASSWORD` for a rejected candidate. The callback
        // requests a fresh candidate only for these errors and retains the
        // Windows result for its caller to check, including on the final attempt.
        // Returning a password here does not imply that Windows accepted it.
        // https://learn.microsoft.com/en-us/windows/win32/api/lmaccess/nf-lmaccess-netusersetinfo#return-value
        // https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/nc-ntsecapi-psam_password_filter_routine#return-value
        //
        // Ten independent candidates reduce the approximate account-name risk
        // above to (0.00125)^10, about 9.3e-30, for all ten to collide. This is
        // not a guarantee about other filters: a rule that rejects every
        // candidate cannot be solved by retries, so the final rejection must
        // reach the caller rather than leaving it in an unbounded loop.
        if (!retry(std::span{password})) {
            break;
        }
    }
    return password;
}

[[nodiscard]] auto CreateAccountIfMissing(const wil::zwstring_view username) {
    auto name = std::wstring{username.c_str(), username.size()};
    auto error = NET_API_STATUS{};
    const auto password = GenerateAccountPassword([&name, &error](const std::span<wchar_t> candidate) {
        auto account = USER_INFO_1{
            .usri1_name = name.data(),
            .usri1_password = candidate.data(),
            .usri1_priv = USER_PRIV_USER,
            .usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT,
        };
        [[gsl::suppress("26490",
            justification:
                "`NetUserAdd` receives the structure through its generic "
                "`BYTE *` buffer parameter. The value 1 tells it to interpret "
                "that buffer as a `USER_INFO_1` containing the new account's "
                "name, initial password, and account settings.")]]
        error = NetUserAdd(
            nullptr, 1, reinterpret_cast<BYTE *>(&account), nullptr);
        return (error == NERR_PasswordTooShort) ||
            (error == ERROR_ILL_FORMED_PASSWORD);
    });
    if (error == NERR_UserExists) {
        return false;
    }
    if (error != NERR_Success) {
        WinError("could not create internal Windows account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{error});
    }
    return true;
}

auto HideAccountFromLogonScreen(const wil::zwstring_view username) {
    const auto result = wil::reg::set_value_dword_nothrow(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\"
        L"SpecialAccounts\\UserList",
        username.c_str(), 0);
    if (FAILED(result)) {
        devicefs::WriteToStream(
            devicefs::stderr,
            L"backup-supervisor: could not hide internal Windows account '{}' "
            L"from the logon screen (Windows error 0x{:08x})\n",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error::FromHresult(result).value);
    }
}

auto EnsureWslDistributionDirectory(
    const wil::zwstring_view username,
    const std::filesystem::path &directory) {
    auto information = std::unique_ptr<USER_INFO_23,
        wil::function_deleter<decltype(&NetApiBufferFree), NetApiBufferFree>>{};
    const auto error = NetUserGetInfo(
        nullptr, username.c_str(), 23, wil::out_param_ptr<BYTE **>(information));
    if (error != NERR_Success) {
        WinError("could not query the SID for internal Windows account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{error});
    }
    auto sid = wil::unique_hlocal_ansistring{};
    if (!ConvertSidToStringSidA(information->usri23_user_sid, sid.addressof())) {
        WinError("could not format the SID for internal Windows account '{}'",
            std::wstring_view{username.c_str(), username.size()});
    }

    // A newly created directory is owned by the built-in Administrators group.
    // SYSTEM and that group have full control; the internal backup account can
    // read and write the directory. It does not inherit permissions from
    // ProgramData, and none of these grants pass down to files or subdirectories.
    // This lets the backup account create distributions while leaving WSL
    // import to set their ACLs. For an existing directory, only the access
    // grants are replaced; its owner remains unchanged.
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            std::format("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGW;;;{})",
                sid.get()).c_str(),
            SDDL_REVISION_1, descriptor.addressof(), nullptr)) {
        WinError("could not create the security descriptor for WSL directory '{}'",
            std::wstring_view{directory.native()});
    }
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
    };
    if (CreateDirectoryW(directory.c_str(), &attributes)) {
        return;
    }
    const auto directory_error = GetLastError();
    if (directory_error != ERROR_ALREADY_EXISTS) {
        WinError("could not create WSL directory '{}'",
            std::wstring_view{directory.native()},
            ExplicitWin32Error{directory_error});
    }

    // The configured account can change between installations. Replace the
    // parent's grants so the new account can create distributions; their own
    // ACLs remain separate because none of these grants are inheritable.
    auto dacl = PACL{};
    auto present = BOOL{};
    auto defaulted = BOOL{};
    if (!GetSecurityDescriptorDacl(
            descriptor.get(), &present, &dacl, &defaulted)) {
        WinError(
            "could not extract the DACL from the new security descriptor for "
            "WSL directory '{}'",
            std::wstring_view{directory.native()});
    }
    auto directory_text = directory.wstring();
    const auto update_error = SetNamedSecurityInfoW(
        directory_text.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    if (update_error != ERROR_SUCCESS) {
        WinError("could not update the access grants for WSL directory '{}'",
            std::wstring_view{directory.native()},
            ExplicitWin32Error{update_error});
    }
}

[[nodiscard]] auto EnsureWsl1Component() -> bool {
    constexpr auto feature =
        L"Microsoft-Windows-Subsystem-Linux"_zv;
    if (const auto result =
            DismInitialize(DismLogErrorsWarnings, nullptr, nullptr);
        FAILED(result)) {
        WinError("could not initialize DISM to prepare Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    const auto shutdown =
        wil::unique_call<decltype(&DismShutdown), DismShutdown>{};
    auto session = wil::unique_any<
        DismSession, decltype(&DismCloseSession), DismCloseSession>{};
    if (const auto result = DismOpenSession(
            DISM_ONLINE_IMAGE, nullptr, nullptr, session.addressof());
        FAILED(result)) {
        WinError("could not open the running Windows installation in DISM",
            ExplicitWin32Error::FromHresult(result));
    }

    auto information = std::unique_ptr<DismFeatureInfo,
        wil::function_deleter<decltype(&DismDelete), DismDelete>>{};
    if (const auto result = DismGetFeatureInfo(
            session.get(), feature.c_str(), nullptr,
            DismPackageNone, wil::out_param(information));
        FAILED(result)) {
        WinError("could not query Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    if (information->FeatureState == DismStateInstalled) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows component '{}' is already installed\n",
            std::wstring_view{feature});
        return false;
    }
    if (information->FeatureState == DismStateInstallPending) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows component '{}' is awaiting a restart "
            L"to complete installation\n",
            std::wstring_view{feature});
        return true;
    }

    devicefs::WriteToStream(
        devicefs::stdout,
        L"backup-supervisor: installing Windows component '{}'\n",
        std::wstring_view{feature});
    const auto result = DismEnableFeature(
        session.get(), feature.c_str(), nullptr, DismPackageNone, FALSE,
        nullptr, 0, TRUE, nullptr, nullptr, nullptr);
    if (FAILED(result)) {
        WinError("could not install Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    devicefs::WriteToStream(
        devicefs::stdout,
        L"backup-supervisor: installed Windows component '{}'\n",
        std::wstring_view{feature});
    if (result == ERROR_SUCCESS_REBOOT_REQUIRED) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows must be restarted to complete "
            L"installation of '{}'\n",
            std::wstring_view{feature});
        return true;
    }
    return false;
}

} // namespace

export [[nodiscard]] auto NativeMachineArchitecture() {
    auto process_machine = USHORT{};
    auto native_machine = USHORT{};
    if (!IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
        WinError("could not determine the native machine architecture");
    }
    return native_machine;
}

export [[nodiscard]] auto WslExecutablePath() {
    auto location = wil::unique_cotaskmem_string{};
    if (const auto result = wil::reg::get_value_string_nothrow(
            HKEY_LOCAL_MACHINE, kWslRegistration.c_str(), L"InstallLocation", location);
        FAILED(result)) {
        WinError("could not read the WSL installation location from 'HKLM\\{}'",
            std::wstring_view{kWslRegistration},
            ExplicitWin32Error::FromHresult(result));
    }
    return std::filesystem::path{location.get()} / L"wsl.exe";
}

// A password must remain unchanged between its reset and the logon that uses
// it. Keeping the mutex ownership in this result extends the lock through an
// inline `CreateProcessWithLogonW` call, until the full expression destroys the
// temporary. Windows mutex ownership belongs to the acquiring thread, so the
// result cannot be copied or moved to another thread.
class BackupAccountPassword : private wil::secure_wstring {
  public:
    using wil::secure_wstring::c_str;

    BackupAccountPassword(
        wil::unique_mutex_nothrow mutex,
        wil::mutex_release_scope_exit lock,
        wil::secure_wstring password) noexcept
        : wil::secure_wstring(std::move(password)),
          mutex_(std::move(mutex)), lock_(std::move(lock)) {}

    [[gsl::suppress("26432",
        justification:
            "The mutex members prevent copying, and this user-declared "
            "destructor prevents implicit move operations. All four operations "
            "are therefore already unavailable without explicit declarations.")]]
    ~BackupAccountPassword() {
        // The caller reads a failed logon's last error after this temporary
        // dies. Move the secure allocation into this scope so its erasure and
        // deallocation, as well as mutex cleanup, finish before the error is
        // restored. Moving the string transfers its allocation without copying
        // the password.
        const auto preserve_error = wil::last_error_context{};
        const auto password = wil::secure_wstring{std::move(*this)};
        lock_.reset();
        mutex_.reset();
    }

  private:
    wil::unique_mutex_nothrow mutex_;
    wil::mutex_release_scope_exit lock_;
};

export [[nodiscard]] auto ResetBackupAccountPassword(
    const wil::zwstring_view username) -> BackupAccountPassword {
    auto information = std::unique_ptr<USER_INFO_1,
        wil::function_deleter<decltype(&NetApiBufferFree), NetApiBufferFree>>{};
    const auto query_error = NetUserGetInfo(
        nullptr, username.c_str(), 1, wil::out_param_ptr<BYTE **>(information));
    if (query_error != NERR_Success) {
        WinError("could not query privileges for backup account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{query_error});
    }
    if (information->usri1_priv == USER_PRIV_ADMIN) {
        throw std::runtime_error(std::format(
            "refusing to reset the password for backup account '{}' "
            "because it is an administrator",
            std::filesystem::path{username.c_str()}.string()));
    }

    auto mutex = [] {
        // The backup account is shared by every supervisor process and Windows
        // session on this computer. A single name in the global namespace makes
        // all of their password resets and subsequent logons use the same mutex.
        constexpr auto kPasswordMutexName = "Global\\devicefs-account-password"_zv;
        // This DACL grants full control to SYSTEM and Administrators, allowing
        // different administrators to open the mutex created by the first caller.
        auto descriptor = wil::unique_hlocal_security_descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
                descriptor.addressof(), nullptr)) {
            WinError("could not create the security descriptor for password mutex '{}'",
                std::string_view{kPasswordMutexName});
        }
        auto attributes = SECURITY_ATTRIBUTES{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = descriptor.get(),
            .bInheritHandle = FALSE,
        };
        auto result = wil::unique_mutex_nothrow{CreateMutexExA(
            &attributes, kPasswordMutexName.c_str(), 0,
            SYNCHRONIZE | MUTEX_MODIFY_STATE)};
        if (!result) {
            WinError("could not create or open password mutex '{}'",
                std::string_view{kPasswordMutexName});
        }
        return result;
    }();
    auto lock = mutex.acquire();

    auto error = NET_API_STATUS{};
    auto password = GenerateAccountPassword([username, &error](const std::span<wchar_t> candidate) {
        auto account = USER_INFO_1003{
            .usri1003_password = candidate.data(),
        };
        [[gsl::suppress("26490",
            justification:
                "`NetUserSetInfo` receives the structure through its generic "
                "`BYTE *` buffer parameter. The value 1003 tells it to interpret "
                "that buffer as a `USER_INFO_1003` containing the replacement "
                "password.")]]
        error = NetUserSetInfo(
            nullptr, username.c_str(), 1003,
            reinterpret_cast<BYTE *>(&account), nullptr);
        return (error == NERR_PasswordTooShort) ||
            (error == ERROR_ILL_FORMED_PASSWORD);
    });
    if (error != NERR_Success) {
        WinError("could not reset the password for backup account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{error});
    }
    return BackupAccountPassword{
        std::move(mutex), std::move(lock), std::move(password)};
}

export [[nodiscard]] auto RunInternalWindowsAccountProcess(
    const wil::zwstring_view username,
    const std::filesystem::path &executable,
    std::wstring command) -> DWORD {
    const auto working_directory = executable.parent_path();
    // `CreateProcessWithLogonW` creates a separate console, so passing console
    // handles would leave the caller unable to see the child's output. Both
    // output streams use one pipe that the caller drains while the child
    // runs, allowing progress to reach the caller without forwarding threads.
    auto output_read = wil::unique_handle{};
    auto output_write = wil::unique_handle{};
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
    };
    if (!CreatePipe(output_read.addressof(), output_write.addressof(),
            &attributes, 0)) {
        WinError("could not create the output pipe for '{}' as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()});
    }
    const auto input = wil::unique_hfile{CreateFileA(
        "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!input) {
        WinError("could not open NUL for input to '{}' as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()});
    }
    auto startup = STARTUPINFOW{
        .cb = sizeof(STARTUPINFOW),
        .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
        .wShowWindow = SW_HIDE,
        .hStdInput = input.get(),
        .hStdOutput = output_write.get(),
        .hStdError = output_write.get(),
    };
    auto process = wil::unique_process_information{};
    // Loading the profile and leaving the environment null gives the child the
    // backup account's registry hive and temporary directory. WSL and MSIX
    // registration, as well as rootfs extraction, must belong to that account,
    // not the caller.
    if (!CreateProcessWithLogonW(
            username.c_str(), L".", ResetBackupAccountPassword(username).c_str(),
            LOGON_WITH_PROFILE, executable.c_str(), command.data(),
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr, working_directory.c_str(), &startup, &process)) {
        WinError("could not start '{}' as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()});
    }
    output_write.reset();
    auto forward = ForwardPipeOutput{GetStdHandle(STD_OUTPUT_HANDLE)};
    const auto output_result = ReadPipeOutput(output_read.get(), forward);
    output_read.reset();
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        WinError("could not wait for '{}' running as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()});
    }
    auto exit_code = DWORD{};
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        WinError("could not obtain the exit code for '{}' running as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()});
    }
    if ((exit_code == 0) && (output_result != ERROR_SUCCESS)) {
        WinError("could not forward the output from '{}' running as internal Windows account '{}'",
            std::wstring_view{executable.native()},
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{output_result});
    }
    return exit_code;
}

namespace {

auto EnsureMaterializedWslDistribution(
    const wil::zwstring_view username,
    const std::string_view distribution,
    const std::filesystem::path &installed_executable) {
    const auto arguments = std::array{
        installed_executable.string(), std::string{kMaterializeOciOption},
        std::string{distribution},
    };
    auto command = std::filesystem::path{wil::ArgvToCommandLine(arguments)}.wstring();
    devicefs::WriteToStream(devicefs::stdout,
        L"backup-supervisor: preparing WSL distribution '{}' as internal Windows account '{}'\n",
        std::filesystem::path{distribution}.native(),
        std::wstring_view{username.c_str(), username.size()});
    const auto exit_code = RunInternalWindowsAccountProcess(
        username, installed_executable, std::move(command));
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "installed backup supervisor failed to materialize WSL distribution '{}' "
            "(exit code 0x{:08x})", distribution, exit_code));
    }
}

} // namespace

export auto EnsureInternalWindowsAccountAndEnvironment(
    const wil::zwstring_view username,
    const std::string_view distribution,
    const std::filesystem::path &installed_executable,
    const std::filesystem::path &wsl_directory) {
    auto account_preparation = std::async(std::launch::async, [username, &wsl_directory] {
        if (CreateAccountIfMissing(username)) {
            devicefs::WriteToStream(
                devicefs::stdout,
                L"backup-supervisor: created internal Windows account '{}'\n",
                std::wstring_view{username.c_str(), username.size()});
            HideAccountFromLogonScreen(username);
        }
        EnsureWslDistributionDirectory(username, wsl_directory);
    });
    auto winfsp_preparation = std::async(std::launch::async, EnsureWinFsp);
    auto package_preparation = std::async(std::launch::async, [] {
        constexpr auto minimum_version = std::array{2u, 7u, 13u};
        if (IsSuitablePackageInstalled(kWslRegistration, minimum_version)) {
            devicefs::WriteToStream(
                devicefs::stdout,
                "backup-supervisor: a suitable version of 'wsl.exe' is "
                "installed\n");
            return false;
        }
        return InstallWslPackage();
    });
    // Materialization runs as the internal Windows account, which must first exist
    // and have permission to create a distribution beneath `wsl_directory`.
    // Materialization also requires the WSL package and component to be usable;
    // a restart required by either defers import until the installer is rerun.
    //
    // WinFsp is needed later to expose the backup images, so its preparation can
    // continue during materialization. The installer collects the WinFsp result
    // after materialization completes or is deferred.
    const auto component_restart_needed = EnsureWsl1Component();
    account_preparation.get();
    const auto package_restart_needed = package_preparation.get();
    if (package_restart_needed || component_restart_needed) {
        devicefs::WriteToStream(
            devicefs::stdout,
            "The installation is not complete. After restarting the computer, "
            "please run `backup-supervisor.exe --install` again to complete "
            "the installation.\n");
    } else {
        EnsureMaterializedWslDistribution(username, distribution,
            installed_executable);
    }
    if (winfsp_preparation.get()) {
        devicefs::WriteToStream(devicefs::stdout,
            "The computer must be restarted before backups can be performed.\n");
    }
}
