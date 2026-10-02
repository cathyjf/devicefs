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
            ClearCrtIoError();
            auto file = wil::unique_file{_wfsopen(path_.c_str(), L"wbT", _SH_DENYNO)};
            if (!file) {
                WinError("failed to create view output file '{}'",
                    std::wstring_view{path_.native()}, ExplicitCrtIoError{});
            }
            return file;
        }()),
        reader_file_([this] {
            auto file = wil::unique_file{_wfsopen(path_.c_str(), L"rb", _SH_DENYNO)};
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

    auto Restore() -> void {
        standard_error_.Restore();
        standard_output_.Restore();
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

// Run `operation(cancellation_event)` while displaying its standard output
// and standard error in a scrolling menu. Choosing Close signals cancellation
// and waits for the operation to stop. If the operation finishes first, its
// output remains visible until Close is chosen. Return its exit code, or an
// empty optional if the user closes the menu while it is still running.
// The caller keeps `terminal` and its `EnterScreen` owner alive for this call.
export template <typename Operation, typename Header>
[[nodiscard]] auto RunViewWithOutput(devicefs::terminal::WindowsConsole &terminal,
    const HANDLE cancellation_event,
    Operation operation, const Header draw_header) -> std::optional<int> {
    using namespace std::chrono_literals;
    const auto path = TemporarySystemDirectoryPath("devicefs-view-output");
    // Declaring `remove_file` before `output` makes deletion happen after
    // `output` has restored the streams and closed the file.
    const auto remove_file = wil::scope_exit([&path] noexcept {
        auto error = std::error_code{};
        std::filesystem::remove(path, error);
    });
    auto output = view_output_detail::ViewOutput{path};
    auto view = devicefs::terminal::OutputMenu{};
    view.SetCommands(std::array{devicefs::terminal::OutputCommand{0, "Close"}});
    auto task = std::async(std::launch::async, std::move(operation), cancellation_event);
    // Destruction of `task` waits for the operation. If reading or displaying
    // output throws, this guard requests cancellation before that wait begins.
    auto cancel_on_failure = wil::scope_exit([cancellation_event] noexcept {
        std::ignore = SetEvent(cancellation_event);
    });
    auto finished = false;
    auto result = std::optional<int>{};
    auto failure = std::exception_ptr{};
    std::ignore = view.Select(terminal, draw_header,
        [&output, &task, &finished, &result, &failure](auto &menu) {
            if (finished) {
                return;
            }
            if (task.wait_for(0ms) != std::future_status::ready) {
                output.ReadOutput(menu);
                return;
            }
            output.ReadOutput(menu);
            try {
                result = task.get();
            } catch (const std::exception &error) {
                failure = std::current_exception();
                menu.AppendLine(error.what());
            }
            finished = true;
            menu.AppendLine("The backup view operation has finished.");
        });
    if (!finished) {
        if (!SetEvent(cancellation_event)) {
            WinError("failed to request closing the backup view");
        }
        try {
            std::ignore = task.get();
        } catch (...) {
            // An exception during shutdown must not replace the user's
            // cancellation result.
        }
    }
    cancel_on_failure.release();
    if (failure) {
        std::rethrow_exception(failure);
    }
    output.Restore();
    return result;
}
