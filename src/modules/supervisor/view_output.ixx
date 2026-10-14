// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.supervisor.view_output;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <cstdio>;
import <io.h>;
import <share.h>;
import devicefs.stream_redirector;
import devicefs.supervisor.temporary_paths;
import devicefs.terminal.menu;
import devicefs.terminal.windows;

namespace view_output_detail {

class ViewOutput {
public:
    explicit ViewOutput(std::filesystem::path path) :
        path_(std::move(path)),
        file_([this] {
            // `wbT` creates a binary file and marks it as short-lived for
            // caching. `_SH_DENYNO` allows `reader_` to open the same file
            // while the redirected streams are writing to it.
            // `D` enables delete-on-close and delete sharing for both opens.
            ClearCrtIoError();
            auto file = wil::unique_file{_wfsopen(path_.c_str(), L"wbTD", _SH_DENYNO)};
            if (!file) {
                WinError("failed to create view output file '{}'",
                    std::wstring_view{path_.native()}, ExplicitCrtIoError{});
            }
            return file;
        }()),
        reader_file_([this] {
            auto file = wil::unique_file{_wfsopen(path_.c_str(), L"rbD", _SH_DENYNO)};
            if (!file) {
                WinError("failed to open view output file '{}' for reading",
                    std::wstring_view{path_.native()}, ExplicitCrtIoError{});
            }
            return file;
        }()),
        reader_(reader_file_.get()),
        standard_output_(_fileno(stdout), _fileno(file_.get()), "stdout"),
        standard_error_(_fileno(stderr), _fileno(file_.get()), "stderr") {}

    auto ReadOutput(devicefs::terminal::OutputMenu &view) {
        // Reaching the current end of the file does not mean output has
        // finished. Clearing that state allows newly written text to be read.
        reader_.clear();
        std::clearerr(reader_file_.get());
        ClearCrtIoError();
        for (auto line = std::string{};;) {
            std::getline(reader_, line);
            if (std::ferror(reader_file_.get()) || reader_.bad()) {
                WinError("failed to read view output file '{}'",
                    std::wstring_view{path_.native()}, ExplicitCrtIoError{});
            }
            if (!reader_) {
                break;
            }
            view.AppendText(line);
            // `std::getline` removes the newline when it consumes one. At
            // end-of-file, the text remains open for the next read to continue.
            if (!reader_.eof()) {
                view.AppendText("\n");
            }
        }
    }

private:
    const std::filesystem::path path_;
    // `reader_` opens the file independently so reading cannot move the file
    // position shared by `stdout` and `stderr`. Member destruction restores
    // both streams before closing `file_`.
    const wil::unique_file file_;
    // MSVC's file buffer returns EOF for both EOF and a failed CRT read. Keeping
    // the underlying `FILE` lets `ReadOutput` distinguish those cases with
    // `ferror` before appending text can overwrite the error information.
    // MSVC's `ifstream(FILE *)` borrows the file, so `reader_` is destroyed first.
    // https://github.com/microsoft/STL/blob/main/stl/inc/__msvc_filebuf.hpp
    const wil::unique_file reader_file_;
    std::ifstream reader_;
    RedirectedStream standard_output_;
    RedirectedStream standard_error_;
};

} // namespace view_output_detail

export enum class OperationOutput { Stream, OnFailure };

// The operation runs on a worker thread with captured stdout and stderr.
// Stream displays output while the worker runs, then offers Close on automatic
// completion. Success after cancellation returns without another selection.
// OnFailure waits without displaying output or entering a screen. Success
// discards the output; failure displays it with Continue.
// The return value is the operation's exit code.
//
// Escape or Ctrl+C invokes cancel() while the worker runs.
// If the callback returns, this function waits and preserves the operation's
// exit code.
// A presentation failure also invokes cancel() during stack unwinding before
// waiting for a running worker.
// Worker exceptions are displayed and return EXIT_FAILURE after the view closes.
// The caller keeps terminal alive and, for Stream, retains its EnterScreen
// owner. OnFailure owns screen entry only while failure output is displayed.
export template <typename Operation, typename Header, typename Cancellation>
[[nodiscard]] auto RunOperationWithOutput(devicefs::terminal::WindowsConsole &terminal,
    Operation operation, const Header draw_header, Cancellation cancel,
    const OperationOutput visibility = OperationOutput::Stream) -> int {
    using namespace std::chrono_literals;
    auto output = view_output_detail::ViewOutput{
        TemporarySystemDirectoryPath("devicefs-view-output")};
    auto view = devicefs::terminal::OutputMenu{};
    view.SetVisible(visibility == OperationOutput::Stream);
    auto screen = std::optional<decltype(terminal.EnterScreen())>{};
    auto task = std::async(std::launch::async, std::move(operation));
    // The destructor of `task` waits for its worker. This guard is destroyed
    // first, giving `cancel()` a chance to stop the worker before that wait.
    auto cancel_on_failure = wil::scope_exit([&task, &cancel] noexcept {
        if (task.valid() && (task.wait_for(0ms) != std::future_status::ready)) {
            try {
                cancel();
            } catch (...) {
                // The presentation failure remains the reported error even if
                // requesting cancellation also fails.
            }
        }
    });
    std::ignore = view.Select(terminal, draw_header,
        [&output, &task, visibility](auto &menu) {
            if (visibility == OperationOutput::Stream) {
                output.ReadOutput(menu);
            }
            if (task.wait_for(0ms) == std::future_status::ready) {
                menu.Close();
            }
        });
    const auto cancelled = task.wait_for(0ms) != std::future_status::ready;
    if (cancelled) {
        cancel();
    }
    const auto [result, failure_message] = [&task]
        -> std::pair<int, std::optional<std::string>> {
        try {
            return {task.get(), std::nullopt};
        } catch (const std::exception &error) {
            return {EXIT_FAILURE, error.what()};
        } catch (...) {
            return {EXIT_FAILURE, "Unknown exception from operation."};
        }
    }();
    cancel_on_failure.release();
    if ((result != EXIT_SUCCESS) ||
        ((visibility == OperationOutput::Stream) && !cancelled)) {
        output.ReadOutput(view);
        if (failure_message) {
            view.AppendLine(*failure_message);
        }
        if (cancelled) {
            view.AppendLine("Operation failed after cancellation was requested.");
        }
        if (visibility == OperationOutput::OnFailure) {
            screen.emplace(terminal.EnterScreen());
        }
        view.SetVisible(true);
        view.SetCommands(std::array{devicefs::terminal::OutputCommand{
            0, visibility == OperationOutput::OnFailure ? "Continue" : "Close"}});
        std::ignore = view.Select(terminal, draw_header, [](auto &) {});
    }
    return result;
}
