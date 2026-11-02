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

export module devicefs.supervisor.process_diagnostics;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import devicefs.supervisor.logging_console;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

// ProcessCommandLineInformation is an undocumented enumeration value.
constexpr auto kProcessCommandLineInformation =
    static_cast<PROCESSINFOCLASS>(60);

[[nodiscard]] auto TryGetProcessCommandLine(const HANDLE process)
    noexcept -> std::wstring {
    auto bytes = ULONG{};
    std::ignore = NtQueryInformationProcess(
        process, kProcessCommandLineInformation, nullptr, 0, &bytes);
    if (bytes < sizeof(UNICODE_STRING)) {
        return {};
    }
    const auto storage = NEW_MALLOCA_BUFFER_NOTHROW(std::byte, bytes);
    if (!storage) {
        return {};
    }
    if (!NT_SUCCESS(NtQueryInformationProcess(process,
            kProcessCommandLineInformation,
            storage.get(), bytes, &bytes))) {
        return {};
    }
    const auto *const command_line =
        std::start_lifetime_as<UNICODE_STRING>(storage.get());
    if (command_line->Buffer == nullptr) {
        return {};
    }
    try {
        return std::wstring(command_line->Buffer,
            command_line->Length / sizeof(wchar_t));
    } catch (...) {
        return {};
    }
}

auto TryTranscode(auto &&...args) noexcept -> std::string {
    try {
        return Transcode<std::string>(std::forward<decltype(args)>(args)...);
    } catch (...) {
        return {};
    }
}

// This function is nonthrowing, but marking it as `noexcept` raises spurious
// compiler warnings, and nothing depends on it being marked `noexcept`, so it
// is simplest not to mark it as `noexcept`.
export auto LogJobProcesses(Log &log, const HANDLE job) {
    using namespace std::string_view_literals;
    auto snapshot = wil::unique_tool_help_snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        const auto error = TryConstructWinError("");
        log.TryWrite(
            "backup-supervisor: failed to enumerate processes before "
            "terminating the backup job: {}",
            error ? std::string_view{error->what()} : "unknown error"sv);
        return;
    }

    auto entry = PROCESSENTRY32W{.dwSize = sizeof(PROCESSENTRY32W)};
    if (!Process32FirstW(snapshot.get(), &entry)) {
        const auto error = TryConstructWinError("");
        log.TryWrite(
            "backup-supervisor: failed to read the process snapshot: {}",
            error ? std::string_view{error->what()} : "unknown error"sv);
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

        const auto command_line = TryGetProcessCommandLine(process.get());
        [[gsl::suppress("26449", justification:
            "The temporary strings survive until the end of the entire "
            "expression and they are not referenced after that.")]]
        log.TryWrite(
            "backup-supervisor: terminating '{}' "
            "(PID {}, command line '{}')",
            TryTranscode(entry.szExeFile),
            entry.th32ProcessID,
            command_line.empty() ?
                "unavailable"sv :
                TryTranscode(command_line));
    } while (Process32NextW(snapshot.get(), &entry));
}
