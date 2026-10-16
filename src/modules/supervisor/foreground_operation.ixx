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

export module devicefs.supervisor.foreground_operation;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.coherent_vector;
import devicefs.stream_writer;
import devicefs.supervisor.windows_service;

struct ForegroundContext {
    wil::unique_event_nothrow cancellation_event = CreateCancellationEvent(nullptr);
    wil::unique_event_nothrow completed_event = CreateCancellationEvent(nullptr);
};

// Each invocation has its own events. A handler's snapshot keeps those events
// alive even if the invocation returns while the handler is still using them.
auto foreground_contexts = std::optional<
    CoherentVector<std::shared_ptr<const ForegroundContext>>>{};
auto foreground_contexts_once = std::once_flag{};

auto WINAPI ForegroundControlHandler(const DWORD) noexcept -> BOOL {
    const auto contexts = foreground_contexts->Read();
    if (!contexts || contexts->empty()) {
        return FALSE;
    }
    // Windows terminates the process when a closure handler returns. Waiting
    // for the operations here lets them finish their cleanup first.
    // https://learn.microsoft.com/en-us/windows/console/handlerroutine
    //
    // Signal every operation before waiting, because one operation's cleanup
    // may depend on another operation finishing.
    for (const auto &context : *contexts) {
        context->cancellation_event.SetEvent();
    }
    for (const auto &context : *contexts) {
        context->completed_event.wait();
    }
    return TRUE;
}

// Return a context and a worker callback. The context is registered only while
// the operation is running.
export [[nodiscard]] auto MakeForegroundOperation(auto &&operation) {
    const auto context = std::make_shared<const ForegroundContext>();
    return std::pair{context, [context,
        operation = std::forward<decltype(operation)>(operation)] mutable {
        const auto completed = context->completed_event.SetEvent_scope_exit();
        std::call_once(foreground_contexts_once, [] {
            foreground_contexts.emplace();
            if (!SetConsoleCtrlHandler(ForegroundControlHandler, TRUE)) {
                WinError("failed to register the foreground console control handler");
            }
        });
        foreground_contexts->PushBack(context);
        const auto remove_context = wil::scope_exit([&context] {
            foreground_contexts->Erase(context);
        });
        return std::invoke(operation, context->cancellation_event.get());
    }};
}

export [[nodiscard]] auto RunForegroundOperation(auto &&operation) {
    auto [context, foreground_task] = MakeForegroundOperation(
        std::forward<decltype(operation)>(operation));

    // Disabling `ENABLE_PROCESSED_INPUT` prevents the operating system from
    // processing Ctrl+C and instead allows our process to read it as keyboard
    // input. When we receive it, we can coordinate orderly cancellation.
    //
    // A failure to disable `ENABLE_PROCESSED_INPUT` is not a fatal error
    // condition. The only consequence is that cancellation may be slightly
    // less coordinated, but it should still work correctly.
    //
    // Disabling `ENABLE_PROCESSED_INPUT` requires write access to `CONIN$`.
    // This requirement is not expressly documented by Microsoft, but it can be
    // seen in the conhost implementation of `ApiDispatchers::ServerSetConsoleMode`.
    // https://github.com/microsoft/terminal/blob/94dfb1/src/server/ApiDispatchers.cpp#L89
    const auto input = wil::unique_hfile{CreateFileA("CONIN$",
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr)};
    auto console_mode = DWORD{};
    if (!input || !GetConsoleMode(input.get(), &console_mode) ||
            !SetConsoleMode(input.get(),
                console_mode & ~ENABLE_PROCESSED_INPUT)) {
        return foreground_task();
    }
    auto restore_console_mode = wil::scope_exit(
        [input_ = input.get(), console_mode] {
        if (SetConsoleMode(input_, console_mode)) {
            return;
        }
        devicefs::WriteToStream(devicefs::stderr, "backup-supervisor: {}.\n",
            TryConstructWinError("failed to restore the previous foreground "
                "console input mode"));
    });

    auto result = std::async(std::launch::async, std::ref(foreground_task));
    auto cancel_on_monitor_failure =
        context->cancellation_event.SetEvent_scope_exit();
    // The first signaled handle wins, giving completion priority over input.
    // https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
    constexpr auto kInputBatchSize = 16uz;
    while (true) {
        if (wil::wait_any_failfast(context->completed_event,
                input.get()).status() == WAIT_OBJECT_0) {
            break;
        }
        auto records = std::array<INPUT_RECORD, kInputBatchSize>{};
        auto read = DWORD{};
        if (!ReadConsoleInputW(input.get(), records.data(),
                CompileTimeCast<DWORD, records.size()>(), &read)) {
            WinError("failed to read foreground console input");
        }
        for (const auto &record : std::span{records}.first(read)) {
            // With VT input enabled, Ctrl+C can arrive as the ETX character
            // (`0x03`) without a virtual key code or Ctrl modifier. This was
            // observed when running through CTest. Both representations
            // request cancellation, allowing callers to retain VT input.
            if ((record.EventType != KEY_EVENT) ||
                !record.Event.KeyEvent.bKeyDown ||
                ((record.Event.KeyEvent.uChar.UnicodeChar != L'\x03') &&
                    ((record.Event.KeyEvent.wVirtualKeyCode != L'C') ||
                        ((record.Event.KeyEvent.dwControlKeyState &
                            (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) == 0)))) {
                continue;
            }
            context->cancellation_event.SetEvent();
            devicefs::WriteToStream(devicefs::stdout,
                "Cancellation requested; waiting for cleanup.\n");
        }
    }
    cancel_on_monitor_failure.release();
    return result.get();
}
