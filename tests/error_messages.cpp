// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise `WinError` with system errors, component message tables, and COM
// descriptions. The COM cases install an error object on the current thread,
// just as a failing COM call would, and inspect the resulting exception.
// Run `devicefs-error-messages-test` without arguments.

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <lmerr.h>
#include <ntstatus.h>
#include <oleauto.h>
#include <vsserror.h>
#include <wininet.h>
#include <wil/com.h>

import std;
import <devicefs/common.h>;

namespace {

constexpr auto kGreenForeground = "\x1b[32m";
constexpr auto kRedForeground = "\x1b[31m";
constexpr auto kDefaultForeground = "\x1b[39m";

// Print a successful test result in green, then restore the default text color.
auto PrintPass(const std::string_view description) -> void {
    std::println("{}PASS: {}{}", kGreenForeground, description, kDefaultForeground);
}

struct CapturedError {
    std::string message;
    std::error_code code;
};

// Capture the message and code through the exception reference. Copying the
// exception as a `std::system_error` would discard its overridden `what()`.
[[nodiscard]] auto CaptureError(const auto error) -> CapturedError {
    try {
        WinError("could not open '{}'", "test object", error);
    } catch (const std::system_error &failure) {
        std::println("\u2192 {}", failure.what());
        if (failure.code().category() != std::system_category()) {
            throw std::runtime_error("a Windows error lost its system category");
        }
        return {failure.what(), failure.code()};
    }
}

// Install a COM error description for the next HRESULT failure. The Automation
// error object copies `description`, so it remains available after this returns.
auto SetComDescription(std::wstring description) -> void {
    auto information = wil::com_ptr<ICreateErrorInfo>{};
    if (const auto result = CreateErrorInfo(information.put()); FAILED(result)) {
        WinError("could not create the test COM error object", ExplicitHresult{result});
    }
    if (const auto result = information->SetDescription(description.data()); FAILED(result)) {
        WinError("could not set the test COM description", ExplicitHresult{result});
    }
    if (const auto result = SetErrorInfo(0, information.query<IErrorInfo>().get()); FAILED(result)) {
        WinError("could not install the test COM error object", ExplicitHresult{result});
    }
}

} // namespace

auto main() -> int try {
    const auto win32 = CaptureError(ExplicitWin32Error{ERROR_ACCESS_DENIED});
    if (!win32.message.contains("Windows error 0x00000005: Access is denied") ||
        (win32.code != std::error_code{ERROR_ACCESS_DENIED, std::system_category()}) ||
        (win32.code != std::errc::permission_denied)) {
        throw std::runtime_error(std::format("incorrect Win32 error: {}", win32.message));
    }
    PrintPass("a Win32 failure displays its description and hexadecimal code, "
        "and retains its error number and portable condition.");

    const auto cancelled = CaptureError(ExplicitWin32Error{ERROR_CANCELLED});
    if (cancelled.code != std::error_code{ERROR_CANCELLED, std::system_category()}) {
        throw std::runtime_error("a cancellation error no longer matches its system error code");
    }
    PrintPass("a cancellation failure matches the system error code used by cancellation handlers.");

    const auto network = CaptureError(ExplicitWin32Error{NERR_UserNotFound});
    if (!network.message.contains(std::format(
        "Windows error 0x{:08x}: The user name could not be found", NERR_UserNotFound))) {
        throw std::runtime_error(std::format("missing network-management message: {}", network.message));
    }
    PrintPass("a network-management failure displays its Netmsg.dll description "
        "and hexadecimal code.");

    const auto download = CaptureError(ExplicitHresult{HRESULT_FROM_WIN32(ERROR_INTERNET_TIMEOUT)});
    if (!download.message.contains("The operation timed out") ||
        !download.message.contains("HRESULT 0x80072ee2")) {
        throw std::runtime_error(std::format("missing download error message: {}", download.message));
    }
    PrintPass("a download failure has its WinINet description and complete HRESULT.");

    const auto snapshot = CaptureError(ExplicitHresult{VSS_E_INSUFFICIENT_STORAGE});
    if (!snapshot.message.contains("Insufficient storage available") ||
        !snapshot.message.contains("HRESULT 0x8004231f")) {
        throw std::runtime_error(std::format("missing VSS error message: {}", snapshot.message));
    }
    PrintPass("a VSS failure has its shadow-copy description and complete HRESULT.");

    const auto nt = CaptureError(ExplicitHresult{HRESULT_FROM_NT(STATUS_ACCESS_DENIED)});
    if (!nt.message.contains("Access Denied") ||
        !nt.message.contains("HRESULT 0xd0000022")) {
        throw std::runtime_error(std::format("missing NT status message: {}", nt.message));
    }
    PrintPass("an NT-derived HRESULT has its Ntdll.dll description and complete HRESULT.");

    SetComDescription(L"The requested object is named caf\u00e9.");
    const auto com = CaptureError(ExplicitHresult{E_FAIL});
    if (!com.message.contains("The requested object is named caf\u00e9.") ||
        !com.message.contains("HRESULT 0x80004005") ||
        !com.message.contains("could not open 'test object'") ||
        (com.code.value() != E_FAIL)) {
        throw std::runtime_error(std::format("incorrect COM diagnostic: {}", com.message));
    }
    const auto next = CaptureError(ExplicitHresult{E_FAIL});
    if (next.message.contains("caf\u00e9")) {
        throw std::runtime_error("a second failure reused the previous COM description");
    }
    PrintPass("a COM description appears as UTF-8 with the operation and HRESULT, once.");

    SetComDescription(L"");
    const auto empty = CaptureError(ExplicitHresult{E_FAIL});
    if (!empty.message.contains("Unspecified error")) {
        throw std::runtime_error(std::format("empty COM description hid the system message: {}", empty.message));
    }
    PrintPass("an empty COM description leaves the standard HRESULT description available.");

    SetComDescription(L"This description belongs to a COM failure.");
    SetLastError(ERROR_FILE_NOT_FOUND);
    try {
        WinError("could not open the test file");
    } catch (const std::system_error &failure) {
        std::println("\u2192 {}", failure.what());
        if ((failure.code() != std::error_code{ERROR_FILE_NOT_FOUND, std::system_category()}) ||
            std::string_view{failure.what()}.contains("COM failure")) {
            throw std::runtime_error("implicit Win32 error capture used the wrong error information");
        }
    }
    const auto pending = CaptureError(ExplicitHresult{E_FAIL});
    if (!pending.message.contains("This description belongs to a COM failure.")) {
        throw std::runtime_error("a Win32 failure consumed the pending COM description");
    }
    PrintPass("implicit Win32 errors preserve GetLastError and leave COM information alone.");

    // This customer-defined HRESULT deliberately has no installed message.
    // The diagnostic must still identify the failure by its complete code.
    constexpr auto unknown_hresult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0xFFFF) |
        APPLICATION_ERROR_MASK;
    const auto unknown = CaptureError(ExplicitHresult{unknown_hresult});
    if (!unknown.message.contains("HRESULT 0xa004ffff") ||
        unknown.message.contains("unknown error")) {
        throw std::runtime_error(std::format("missing numeric fallback: {}", unknown.message));
    }
    PrintPass("a code without a message resource produces a numeric diagnostic.");
    return 0;
} catch (const std::exception &error) {
    std::println("{}FAIL: {}{}", kRedForeground, error.what(), kDefaultForeground);
    return 1;
}
