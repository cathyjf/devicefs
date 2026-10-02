// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise `WinError` with system errors, component message tables, and COM
// descriptions. The COM cases install an error object on the current thread,
// just as a failing COM call would, and inspect the resulting exception.
// Run `devicefs-error-messages-test` without arguments. The read-failure test
// creates and removes its own directory under the system temporary directory.

#include <devicefs/strsafe_compat.h>

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <share.h>;
import devicefs.file_reader;

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
        std::println("Expected test diagnostic: \u2192 {}", failure.what());
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

auto TestReadFailure() -> void {
    const auto directory = std::filesystem::temp_directory_path() /
        std::format("devicefs-crt-io-{}", GetCurrentProcessId());
    if (!std::filesystem::create_directory(directory)) {
        throw std::runtime_error(std::format("test directory already exists: {}", directory.string()));
    }
    const auto cleanup = wil::scope_exit([&directory] {
        auto error = std::error_code{};
        std::filesystem::remove_all(directory, error);
    });
    const auto path = directory / "locked.txt";
    constexpr auto contents = std::string_view{"readable before and after the lock\n"};
    {
        auto output = std::ofstream{path, std::ios::binary};
        output << contents;
        output.close();
        if (!output) {
            WinError("could not write the read-failure test file '{}'",
                std::wstring_view{path.native()}, ExplicitCrtIoError{});
        }
    }
    // A separate handle's exclusive byte-range lock permits opening the file
    // but prevents reading its first byte. This causes a real read failure
    // without replacing the production reader or its error handling.
    auto lock = wil::unique_hfile{CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!lock) {
        WinError("could not open the read-failure test file for locking");
    }
    auto operation = OVERLAPPED{};
    if (!LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &operation)) {
        WinError("could not lock the read-failure test file");
    }
    {
        const auto file = wil::unique_file{_wfsopen(path.c_str(), L"rb", _SH_DENYWR)};
        if (!file) {
            WinError("could not open the locked test file", ExplicitCrtIoError{});
        }
        auto input = std::ifstream{file.get()};
        auto line = std::string{};
        std::getline(input, line);
        if (!std::ferror(file.get()) || !input.eof() || input.bad()) {
            throw std::runtime_error("the locked read did not demonstrate a CRT error presented as C++ EOF");
        }
    }
    const auto unreadable = ReadEntireFile(path);
    if (unreadable) {
        throw std::runtime_error("the whole-file reader accepted a failed read as EOF");
    }
    try {
        WinError("could not read '{}'", std::wstring_view{path.native()}, unreadable.error());
    } catch (const std::system_error &failure) {
        const auto message = std::string_view{failure.what()};
        if ((failure.code() != std::error_code{ERROR_LOCK_VIOLATION, std::system_category()}) ||
            !message.contains("locked.txt") || !message.contains("Windows error 0x00000021")) {
            throw std::runtime_error(std::format("the reader lost its underlying read error: {}", message));
        }
    }
    // The next assertion requires the byte-range lock to be released. Explicit
    // unlocking avoids relying on close-time release, whose timing is qualified
    // in https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex#remarks.
    if (!UnlockFileEx(lock.get(), 0, 1, 0, &operation)) {
        WinError("could not unlock the read-failure test file");
    }
    lock.reset();
    const auto readable = ReadEntireFile(path);
    if (!readable || (*readable != contents)) {
        throw std::runtime_error("the whole-file reader did not recover after releasing the lock");
    }
    std::filesystem::remove_all(directory);
    PrintPass("a real read failure reported as C++ EOF retains its Windows error and file context.");
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
        std::println("Expected test diagnostic: \u2192 {}", failure.what());
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

    struct CrtCase {
        int crt_error;
        unsigned long windows_error;
        std::error_code expected;
    };
    for (const auto &test : std::array{
            CrtCase{EINVAL, ERROR_ACCESS_DENIED,
                {ERROR_ACCESS_DENIED, std::system_category()}},
            CrtCase{EMFILE, 0, {EMFILE, std::generic_category()}},
            CrtCase{0, 0, std::make_error_code(std::io_errc::stream)}}) {
        errno = test.crt_error;
        _doserrno = test.windows_error;
        const auto constructed = TryConstructWinError("", ExplicitCrtIoError{});
        if (!constructed) {
            throw std::runtime_error("could not construct the test CRT I/O error");
        }
        errno = test.crt_error;
        _doserrno = test.windows_error;
        try {
            WinError("could not redirect '{}'", "stdout", ExplicitCrtIoError{});
        } catch (const std::system_error &failure) {
            const auto message = std::string_view{failure.what()};
            if ((failure.code() != test.expected) ||
                !message.contains("could not redirect 'stdout'") ||
                !message.contains(constructed->what())) {
                throw std::runtime_error(std::format(
                    "inconsistent constructed and thrown CRT I/O errors: {} / {}",
                    constructed->what(), message));
            }
        }
    }
    PrintPass("constructed and thrown CRT I/O errors agree on Windows errors, "
        "CRT-only errors, and stream failures without a recorded error code.");

    errno = EACCES;
    _doserrno = ERROR_ACCESS_DENIED;
    const auto saved = ExplicitCrtIoError{};
    errno = ENOENT;
    _doserrno = ERROR_FILE_NOT_FOUND;
    const auto saved_diagnostic = TryConstructWinError("", saved);
    if (!saved_diagnostic) {
        throw std::runtime_error("could not construct the saved CRT I/O error");
    }
    try {
        WinError("could not read '{}'", std::wstring_view{L"caf\u00e9"}, saved);
    } catch (const std::system_error &failure) {
        if ((failure.code() != std::error_code{ERROR_ACCESS_DENIED, std::system_category()}) ||
            !std::string_view{failure.what()}.contains("caf\u00e9") ||
            !std::string_view{failure.what()}.contains(saved_diagnostic->what())) {
            throw std::runtime_error("a saved CRT I/O error lost its code or wide context");
        }
    }
    PrintPass("a saved CRT I/O error survives later errors and formats a wide path.");

    // Exhaust CRT stream slots without consuming disk space, then exercise the
    // real reader. Releasing the streams before reporting also checks that the
    // result carries the error independently of subsequent CRT operations.
    auto streams = std::vector<wil::unique_file>{};
    while (auto stream = wil::unique_file{_wfsopen(L"NUL", L"rb", _SH_DENYNO)}) {
        streams.push_back(std::move(stream));
    }
    // The failing CRT open sets `EMFILE` without changing `_doserrno`. An
    // unrelated Windows error must not take precedence over that real failure.
    _doserrno = ERROR_ACCESS_DENIED;
    const auto unreadable = ReadEntireFile(std::filesystem::path{"NUL"});
    streams.clear();
    if (unreadable) {
        throw std::runtime_error("the reader unexpectedly opened a file with no CRT stream slots available");
    }
    try {
        WinError("could not read 'NUL'", unreadable.error());
    } catch (const std::system_error &failure) {
        if (failure.code() != std::error_code{EMFILE, std::generic_category()}) {
            throw std::runtime_error(std::format("the reader lost its CRT-only error: {}", failure.what()));
        }
    }
    PrintPass("the whole-file reader preserves a real CRT-only open failure.");
    TestReadFailure();
    return 0;
} catch (const std::exception &error) {
    std::println("{}FAIL: {}{}", kRedForeground, error.what(), kDefaultForeground);
    return 1;
}
