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
#include <ntsecapi.h>

// `wil/resource.h` defines `wil::unique_hlsa` only when `_NTLSA_` is defined.
#define _NTLSA_
#include <wil/resource.h>

#include <wil/safecast.h>
#include <wil/stl.h>

#undef stderr
#undef stdout

module devicefs.supervisor.native_backup:s4u_logon;

import std;
import :privileges;
import devicefs.common;
import devicefs.stream_writer;

namespace internal {

using UniqueLsaLogonProcess = wil::unique_any<
    LSA_HANDLE,
    decltype(&::LsaDeregisterLogonProcess),
    ::LsaDeregisterLogonProcess>;

template <std::size_t Size>
[[nodiscard]] constexpr auto MakeLsaString(
    std::array<char, Size> &text) noexcept {
    static_assert(Size > 0);
    static_assert(Size <= std::numeric_limits<USHORT>::max());
    return LSA_STRING{
        .Length = USHORT{Size - 1},
        .MaximumLength = USHORT{Size},
        .Buffer = text.data(),
    };
}

[[nodiscard]] auto LsaWin32Error(const NTSTATUS status) noexcept {
    return ExplicitWin32Error{LsaNtStatusToWinError(status)};
}

// Changing the account policy is unnecessary when the account already has the
// right, so failure of this attempt is not itself a logon failure. The
// `LsaLogonUser` call below authoritatively determines whether the account can
// create the required batch logon.
[[nodiscard]] auto TryGrantBatchLogonRight(
    const wil::zwstring_view username) -> bool {
    auto sid = [username] -> std::optional<std::vector<BYTE>> {
        auto sid_size = DWORD{};
        auto domain_size = DWORD{};
        auto use = SID_NAME_USE{};
        std::ignore = LookupAccountNameW(
            nullptr, username.c_str(), nullptr, &sid_size,
            nullptr, &domain_size, &use);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::nullopt;
        }

        auto sid = std::vector<BYTE>(sid_size);
        auto domain = std::vector<wchar_t>(std::max<DWORD>(1, domain_size));
        if (!LookupAccountNameW(
            nullptr, username.c_str(), sid.data(), &sid_size,
            domain.data(), &domain_size, &use)) {
            return std::nullopt;
        }
        return sid;
    }();
    if (!sid) {
        return false;
    }

    auto policy_attributes = LSA_OBJECT_ATTRIBUTES{
        .Length = sizeof(LSA_OBJECT_ATTRIBUTES),
    };
    auto policy = wil::unique_hlsa{};
    const auto open = LsaOpenPolicy(
        nullptr, &policy_attributes,
        POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT,
        policy.addressof());
    if (open != 0) {
        return false;
    }

    // The configured account is already trusted to execute arbitrary backup
    // code and can establish persistence without this account right. Granting
    // the narrower ability to create a batch logon adds no independent
    // persistence boundary.
    auto right = [] {
        constexpr auto right_text =
            wil::zwstring_view{SE_BATCH_LOGON_NAME};
        [[gsl::suppress("26492",
            justification:
                "`LsaAddAccountRights` annotates `UserRights` with `_In_reads_`, "
                "indicating that it does not modify this parameter. However, the "
                "parameter does not carry a modern `const` qualifier, so "
                "`const_cast` is required.")]]
        return LSA_UNICODE_STRING{
            .Length = wil::safe_cast_failfast<USHORT>(
                right_text.size() * sizeof(wchar_t)),
            .MaximumLength = wil::safe_cast_failfast<USHORT>(
                (right_text.size() + 1) * sizeof(wchar_t)),
            .Buffer = const_cast<wchar_t *>(right_text.c_str()),
        };
    }();
    return LsaAddAccountRights(
        policy.get(), sid->data(), &right, 1) == 0;
}

[[nodiscard]] auto LogOnWindowsAccountWithS4u(
    const wil::zwstring_view username) {
    if (!TryGrantBatchLogonRight(username)) {
        devicefs::WriteToStream(
            devicefs::stderr,
            L"backup-supervisor: warning: could not grant "
            L"SeBatchLogonRight to configured WSL account '{}'; attempting "
            L"S4U logon in case the account already holds it\n",
            std::wstring_view{username.c_str(), username.size()});
    }

    constexpr auto privilege_names = std::array{
        wil::zwstring_view(SE_TCB_NAME),
    };
    auto privileges = ProcessPrivilegeEnabler{
        GetCurrentProcess(), privilege_names,
        std::string_view{"the S4U logon privilege"}};

    auto lsa_name_text = std::to_array("DeviceFs");
    auto lsa_name = MakeLsaString(lsa_name_text);
    auto lsa = UniqueLsaLogonProcess{};
    auto security_mode = LSA_OPERATIONAL_MODE{};
    const auto registration = LsaRegisterLogonProcess(
        &lsa_name, lsa.addressof(), &security_mode);
    if (registration != 0) {
        WinError("could not register the S4U logon process",
            LsaWin32Error(registration));
    }

    auto package_name_text = std::to_array(MSV1_0_PACKAGE_NAME);
    auto package_name = MakeLsaString(package_name_text);
    auto authentication_package = ULONG{};
    const auto package_lookup = LsaLookupAuthenticationPackage(
        lsa.get(), &package_name, &authentication_package);
    if (package_lookup != 0) {
        WinError("could not identify the MSV1_0 authentication package",
            LsaWin32Error(package_lookup));
    }

    const auto username_text = std::wstring_view{
        username.c_str(), username.length()};
    const auto domain_text = std::wstring_view{L"."};
    // `LsaLogonUser` accepts one `AuthenticationInformation` address and byte
    // count. For `MSV1_0_INTERACTIVE_LOGON` and `KERB_S4U_LOGON`, Microsoft
    // requires each `UNICODE_STRING::Buffer` to point to characters stored
    // contiguously with the packet structure, and requires
    // `AuthenticationInformationLength` to include those characters. The same
    // documentation page does not mention or discuss `MSV1_0_S4U_LOGON`:
    // <https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/nf-ntsecapi-lsalogonuser>
    //
    // Microsoft's Windows OpenSSH implementation demonstrates the omitted case.
    // It allocates one block for `MSV1_0_S4U_LOGON` and both strings, places
    // the characters directly after the structure, points both `UNICODE_STRING`
    // values into those locations, and passes that block and its complete size
    // to `LsaLogonUser`:
    // <https://github.com/PowerShell/openssh-portable/blob/0d88c34/contrib/win32/win32compat/win32_usertoken_utils.c#L160-L192>
    //
    // It would be simpler to store the username and domain in independent
    // `std::wstring` objects and point the `UNICODE_STRING` values at them.
    // However, their characters would then lie outside the `request_storage`
    // byte range passed as `AuthenticationInformation`; neither source
    // documents or demonstrates that layout.
    //
    // Consequently, for consistency with the Microsoft-endorsed implementation,
    // `request_storage` contains the structure followed by both strings; its
    // allocation must remain stable and its storage must include all three
    // objects until `LsaLogonUser` returns.
    auto request_storage = std::vector<std::byte>(
        sizeof(MSV1_0_S4U_LOGON) +
        (username_text.size() + 1) * sizeof(wchar_t) +
        (domain_text.size() + 1) * sizeof(wchar_t));
    auto &request = *::new (request_storage.data()) MSV1_0_S4U_LOGON{
        .MessageType = MsV1_0S4ULogon,
        .Flags = 0,
    };

    std::tie(request.UserPrincipalName, request.DomainName) =
        [&request_storage, username_text, domain_text] {
            auto next_string_storage =
                request_storage.data() + sizeof(MSV1_0_S4U_LOGON);
            const auto copy_and_advance = [&next_string_storage](
                const std::wstring_view text) {
                const auto storage = std::span{
                    std::start_lifetime_as_array<wchar_t>(
                        next_string_storage, text.size() + 1),
                    text.size() + 1,
                };
                std::ranges::copy(text, storage.begin());
                storage.back() = L'\0';
                const auto length = wil::safe_cast_failfast<USHORT>(
                    text.size() * sizeof(wchar_t));
                next_string_storage += storage.size_bytes();
                return UNICODE_STRING{
                    .Length = length,
                    .MaximumLength = length,
                    .Buffer = storage.data(),
                };
            };
            return std::pair{
                copy_and_advance(username_text),
                copy_and_advance(domain_text),
            };
        }();

    auto source = [] {
        auto result = TOKEN_SOURCE{};
        constexpr auto source_name = std::string_view{"DeviceFs"};
        static_assert(source_name.size() == TOKEN_SOURCE_LENGTH);
        std::ranges::copy(source_name, std::begin(result.SourceName));
        if (!AllocateLocallyUniqueId(&result.SourceIdentifier)) {
            WinError("could not allocate the S4U token source identifier");
        }
        return result;
    }();

    auto profile = wil::unique_lsa_ptr<void>{};
    auto profile_size = ULONG{};
    auto logon_id = LUID{};
    auto token = wil::unique_handle{};
    auto quotas = QUOTA_LIMITS{};
    auto substatus = NTSTATUS{};
    // `LsaLogonUser` describes this logon in two places. The request packet's
    // `MessageType` tells MSV1_0 to perform an S4U logon, which identifies the
    // configured account without its password. The separate `LogonType`
    // argument tells LSA what kind of logon session and token to create:
    // <https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/ne-ntsecapi-msv1_0_logon_submit_type>
    // <https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/nf-ntsecapi-lsalogonuser>
    //
    // MSV1_0 rejects `Interactive` when it is combined with this S4U packet.
    // `Batch` returns a primary token, whereas `Network` returns an
    // impersonation token. The caller passes the result to
    // `CreateProcessAsUserA`, which requires a primary token; choosing
    // `Network` would therefore require an additional `DuplicateTokenEx`
    // conversion:
    // <https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/nf-ntsecapi-lsalogonuser>
    // <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasusera>
    //
    // `Batch` classifies the Windows logon session, not the DeviceFs mode.
    // Modes that need this token can still be interactive; `--view` is one
    // example. Selecting `Batch` also requires the configured account to hold
    // `SeBatchLogonRight` ("Log on as a batch job"). The LocalSystem supervisor
    // tries to grant it above with `LsaAddAccountRights`; asking that API to
    // add an existing right is harmless:
    // <https://learn.microsoft.com/en-us/windows/win32/secauthz/account-rights-constants>
    // <https://learn.microsoft.com/en-us/windows/win32/api/ntsecapi/nf-ntsecapi-lsaaddaccountrights>
    const auto logon = LsaLogonUser(
        lsa.get(), &lsa_name, Batch, authentication_package,
        &request, wil::safe_cast_failfast<ULONG>(request_storage.size()),
        nullptr, &source, wil::out_param(profile), &profile_size,
        &logon_id, token.addressof(), &quotas, &substatus);
    if (logon != 0) {
        WinError("could not log on configured WSL account '{}' through S4U",
            std::wstring_view{username.c_str(), username.size()},
            LsaWin32Error((substatus != 0) ? substatus : logon));
    }

    privileges.Restore();
    return token;
}

} // namespace internal
