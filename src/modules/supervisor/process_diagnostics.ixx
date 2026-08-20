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
#include <tlhelp32.h>
#include <winternl.h>

#include <wil/resource.h>

export module devicefs.supervisor.process_diagnostics;

import std;
import devicefs.supervisor.logging_console;

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose `std::start_lifetime_as`.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
} // namespace std
#endif

[[gsl::suppress("type.1",
    justification: "ProcessCommandLineInformation is NT process information class 60.")]]
constexpr auto kProcessCommandLineInformation =
    static_cast<PROCESSINFOCLASS>(60);

[[nodiscard]] auto Utf8(const std::wstring_view value) {
    const auto encoded = std::filesystem::path(value).u8string();
    return std::string(encoded.begin(), encoded.end());
}

[[nodiscard]] auto ProcessCommandLine(const HANDLE process) {
    auto bytes = ULONG{};
    static_cast<void>(NtQueryInformationProcess(
        process, kProcessCommandLineInformation,
        nullptr, 0, &bytes));
    if (bytes < sizeof(UNICODE_STRING)) {
        return std::wstring{};
    }
    auto storage = std::vector<std::byte>(bytes);
    if (NtQueryInformationProcess(process, kProcessCommandLineInformation,
            storage.data(), bytes, &bytes) < 0) {
        return std::wstring{};
    }
    const auto *const command_line =
        std::start_lifetime_as<UNICODE_STRING>(storage.data());
    if (command_line->Buffer == nullptr) {
        return std::wstring{};
    }
    return std::wstring(command_line->Buffer,
        command_line->Length / sizeof(wchar_t));
}

export [[gsl::suppress("26447",
    justification: "Potentially throwing work is caught locally; catch diagnostics use noexcept operations.")]]
auto LogJobProcesses(Log &log, const HANDLE job) noexcept {
    try {
        auto snapshot = wil::unique_tool_help_snapshot(
            CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            log.TryWrite(
                "backup-supervisor: could not enumerate processes before "
                "terminating the backup job: error {}", GetLastError());
            return;
        }

        auto entry = PROCESSENTRY32W{.dwSize = sizeof(PROCESSENTRY32W)};
        if (!Process32FirstW(snapshot.get(), &entry)) {
            log.TryWrite(
                "backup-supervisor: could not read the process snapshot: "
                "error {}", GetLastError());
            return;
        }
        do {
            auto process = wil::unique_process_handle(OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process) {
                continue;
            }
            auto in_job = FALSE;
            if (!IsProcessInJob(process.get(), job, &in_job) || !in_job) {
                continue;
            }

            const auto command_line = ProcessCommandLine(process.get());
            log.Write(
                "backup-supervisor: terminating '{}' "
                "(PID {}, command line '{}')",
                Utf8(entry.szExeFile), entry.th32ProcessID,
                command_line.empty() ? "unavailable" : Utf8(command_line));
        } while (Process32NextW(snapshot.get(), &entry));
    } catch (const std::exception &error) {
        log.TryWrite(
            "backup-supervisor: could not identify processes before "
            "terminating the backup job: {}", error.what());
    } catch (...) {
        log.TryWrite(
            "backup-supervisor: could not identify processes before "
            "terminating the backup job: unknown error");
    }
}
