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

#include <windows.h>
#include <wininet.h>
#include <lmerr.h>
#include <oleauto.h>
#include <wil/com.h>
#include <wil/resource.h>

import std;
import <devicefs/common.h>;
import devicefs.terminal.transcoding;

static_assert(std::same_as<DWORD, unsigned long>);
static_assert(std::same_as<HRESULT, long>);

ExplicitHresult::operator unsigned long() const noexcept {
    // An HRESULT's facility identifies the source of its error code.
    // Only `FACILITY_WIN32` wraps a Win32 error number; stripping any
    // other facility could report an unrelated Win32 failure.
    if (HRESULT_FACILITY(value) != FACILITY_WIN32) {
        return value;
    }
    return HRESULT_CODE(value);
}

namespace {

[[nodiscard]] auto TrimTrailingWhitespace(const std::string_view message) {
    // Suppressing line breaks can leave trailing spaces in the message.
    // The pipeline removes trailing whitespace. These ASCII bytes cannot
    // occur within a multibyte UTF-8 character, so trimming preserves UTF-8.
    return message |
        std::views::reverse |
        std::views::drop_while([](const char character) {
            return std::string_view{" \t\r\n"}.contains(character);
        }) |
        std::views::reverse |
        std::ranges::to<std::string>();
}

// Read an error description from the system message tables, or from `module`
// when one is supplied. Return an empty string if the message is unavailable.
[[nodiscard]] auto ReadErrorMessage(const DWORD error, const LPCVOID module = nullptr)
    -> std::string {
    const auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK |
        (module ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM);
    // This loop selects English first because this program's error messages
    // are all written in English. If English is unavailable, the loop allows
    // Windows to try another language.
    for (const auto language : {MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), 0}) {
        auto message = wil::unique_hlocal_ansistring{};
        const auto length = FormatMessageA(flags, module, error, language,
            wil::out_param_ptr<char *>(message), 0, nullptr);
        if (length == 0) {
            continue;
        }
        return TrimTrailingWhitespace(std::string_view{message.get(), length});
    }
    return {};
}

// Retrieve an error description from Windows message resources, or return an
// empty string if no description is available. Some Windows components
// keep their messages in separate DLLs, so a system-table lookup alone can
// return no description even though Windows has the message installed.
[[nodiscard]] auto ReadWindowsErrorMessage(const DWORD error) -> std::string {
    const auto system_message = ReadErrorMessage(error);
    if (!system_message.empty()) {
        return system_message;
    }
    const auto [library, message_id] = [error] -> std::pair<const char *, DWORD> {
        // Network-management errors use the message table in `netmsg.dll`.
        // https://learn.microsoft.com/en-us/windows/win32/netmgmt/looking-up-text-for-error-code-numbers
        if ((error >= NERR_BASE) && (error <= MAX_NERR)) {
            return {"netmsg.dll", error};
        }
        // Download errors in the WinINet range have messages in `wininet.dll`.
        // https://learn.microsoft.com/en-us/windows/win32/wininet/appendix-c-handling-errors
        if ((error >= INTERNET_ERROR_BASE) && (error <= INTERNET_ERROR_LAST)) {
            return {"wininet.dll", error};
        }
        // `HRESULT_FROM_NT` adds `FACILITY_NT_BIT` to an NT status. Removing
        // that marker recovers the message identifier used by `ntdll.dll`.
        // https://learn.microsoft.com/en-us/windows/win32/api/winerror/nf-winerror-hresult_from_nt
        if ((error & FACILITY_NT_BIT) != 0) {
            return {"ntdll.dll", error & ~FACILITY_NT_BIT};
        }
        // VSS publishes its error descriptions in `vsstrace.dll`. VSS shares
        // `FACILITY_ITF` with other COM interfaces, so the facility alone cannot
        // identify a VSS error; the message lookup determines whether it exists.
        // https://learn.microsoft.com/en-us/windows/win32/vss/what-s-new-in-vss-in-windows-vista#vss-error-reporting
        return {"vsstrace.dll", error};
    }();
    const auto module = wil::unique_hmodule{LoadLibraryExA(library, nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (module) {
        return ReadErrorMessage(message_id, module.get());
    }
    return {};
}

[[nodiscard]] auto FormatWindowsError(const DWORD error) -> std::string {
    const auto message = ReadWindowsErrorMessage(error);
    return std::format("Windows error 0x{:08x}{}{}", error,
        message.empty() ? "" : ": ", message);
}

// Windows API failures are system errors, so this exception derives from
// `std::system_error` and uses `std::system_category()`. Its `what()` reports
// the failed operation and the expanded Windows error description.
class WindowsError final : public std::system_error {
public:
    WindowsError(const DWORD error, const std::string &operation)
        : std::system_error(error, std::system_category()),
          message_(std::format("{}: {}", operation, FormatWindowsError(error))) {}

    [[nodiscard]] auto what() const noexcept -> const char * override {
        return message_.c_str();
    }

private:
    std::string message_;
};

} // namespace

namespace detail {

auto LastWin32Error() noexcept -> ExplicitWin32Error {
    return {GetLastError()};
}

auto TranscodeString(const std::wstring_view argument) -> std::string {
    return devicefs::terminal::Transcode<std::string>(argument);
}

[[noreturn]] auto ThrowWinError(const unsigned long error, const std::string &operation) -> void {
    throw WindowsError(error, operation);
}

// Format the complete HRESULT and any description supplied by the failed COM
// call. `GetErrorInfo` consumes the current thread's error object, so this runs
// before formatting the operation's arguments or looking up message resources.
// https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-geterrorinfo
[[nodiscard]] auto FormatHresult(const HRESULT error) -> std::string {
    const auto description = [] -> std::string {
        auto information = wil::com_ptr_nothrow<IErrorInfo>{};
        if (GetErrorInfo(0, information.put()) != S_OK) {
            return {};
        }
        auto text = wil::unique_bstr{};
        if (FAILED(information->GetDescription(text.put())) || (SysStringLen(text.get()) == 0)) {
            return {};
        }
        return TranscodeString(std::wstring_view{text.get(), SysStringLen(text.get())});
    }();
    return std::format(" (HRESULT 0x{:08x}){}{}", static_cast<DWORD>(error),
        description.empty() ? "" : ": ", description);
}

} // namespace detail

auto HardenProcess() -> void {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        WinError("could not restrict DLL search directories");
    }

    auto dynamic_code = PROCESS_MITIGATION_DYNAMIC_CODE_POLICY{};
    dynamic_code.ProhibitDynamicCode = 1;
    if (!SetProcessMitigationPolicy(
            ProcessDynamicCodePolicy, &dynamic_code, sizeof(dynamic_code))) {
        WinError("could not prohibit dynamic code");
    }

    auto strict_handles = PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY{};
    strict_handles.RaiseExceptionOnInvalidHandleReference = 1;
    strict_handles.HandleExceptionsPermanentlyEnabled = 1;
    if (!SetProcessMitigationPolicy(
            ProcessStrictHandleCheckPolicy, &strict_handles, sizeof(strict_handles))) {
        WinError("could not enable strict handle checking");
    }

    auto extension_points = PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY{};
    extension_points.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy,
            &extension_points, sizeof(extension_points))) {
        WinError("could not disable legacy extension points");
    }

    auto image_load = PROCESS_MITIGATION_IMAGE_LOAD_POLICY{};
    image_load.NoRemoteImages = 1;
    image_load.NoLowMandatoryLabelImages = 1;
    if (!SetProcessMitigationPolicy(
            ProcessImageLoadPolicy, &image_load, sizeof(image_load))) {
        WinError("could not restrict image loading");
    }
}
