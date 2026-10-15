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
import devicefs.stream_writer;
import devicefs.supervisor.temporary_paths;
import devicefs.terminal.drawing;
import devicefs.terminal.menu;

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

template <class T>
class FutureWithSavedResult {
    using ResultType = decltype(T{}.get());

public:
    FutureWithSavedResult(T &&future) noexcept : future_(std::move(future)) {}

    auto Get() {
        if (!result_) {
            result_ = future_.get();
        }
        return *result_;
    }

    auto MaybeCompleteTask() {
        using namespace std::chrono_literals;
        if (result_ ||
            (future_.wait_for(0ms) != std::future_status::ready)) {
            return false;
        }
        result_ = future_.get();
        return true;
    }

    auto operator*() const
        noexcept(std::is_nothrow_copy_constructible_v<ResultType>) {
        return result_;
    }

private:
    std::optional<ResultType> result_;
    T future_;
};

template <typename T>
concept OutputOperation = requires(T &callbacks,
    devicefs::terminal::Frame<
        std::remove_reference_t<decltype(callbacks.terminal)>> &frame,
    devicefs::terminal::OutputMenu &menu, int exit_code,
    FutureWithSavedResult<std::future<int>> &task,
    std::optional<std::size_t> command,
    const decltype([] noexcept {}) &close) {
    { callbacks.terminal } -> devicefs::terminal::MenuTerminal;
    { callbacks.initially_visible } -> std::same_as<const bool &>;
    { callbacks.operation() } -> std::same_as<int>;
    callbacks.draw_header(frame);
    { callbacks.notify_completion(menu, exit_code, close) } -> std::same_as<void>;
    { callbacks.dispatch(menu, task, command, close) } -> std::same_as<void>;
};

#undef stderr
#undef stdout

export [[nodiscard]] auto RunOperationWithOutput(
    OutputOperation auto &&callbacks) -> int {
    auto view = devicefs::terminal::OutputMenu{};
    auto closed = false;
    const auto close = [&view, &closed] noexcept {
        closed = true;
        view.Close();
    };
    view.SetVisible(callbacks.initially_visible);

    // While this `output` object is alive, standard output and standard error
    // are redirected into temporary files.
    auto output = view_output_detail::ViewOutput{
        TemporarySystemDirectoryPath("devicefs-view-output")};

    // This menu does not allow exceptions to escape from the supervised
    // asynchronous operation. Instead, exceptions are informationally printed
    // and then translated into a nonzero exit status code.
    auto task = FutureWithSavedResult{
        std::async(std::launch::async, [&callbacks] noexcept {
        try {
            return callbacks.operation();
        } catch (const std::exception &error) {
            devicefs::WriteToStream(devicefs::stderr, "{}\n", error.what());
            if (const auto system_error_ = dynamic_cast<
                    const std::system_error *>(&error);
                    system_error_ && system_error_->code()) {
                return system_error_->code().value();
            }
        } catch (...) {
            devicefs::WriteToStream(devicefs::stderr,
                "Unknown exception from operation.\n");
        }
        return EXIT_FAILURE;
    })};

    auto dismiss_on_failure = wil::scope_exit(
        [&callbacks, &view, &task, &close, &closed = std::as_const(closed)]
        noexcept {
            // An exception arising from the menu machinery (and not from the
            // supervised asynchronous operation) is treated as a request to
            // dismiss the menu. If the menu has already been closed, no action
            // is taken.
            if (closed) {
                return;
            }
            try {
                callbacks.dispatch(view, task, std::nullopt, close);
            } catch (...) {}
        });
    while (!closed) {
        const auto command = view.Select(callbacks.terminal,
            [&callbacks](auto &frame) { callbacks.draw_header(frame); },
            [&task, &output, &callbacks, &close](auto &menu) {
                output.ReadOutput(menu);
                if (task.MaybeCompleteTask()) {
                    callbacks.notify_completion(menu, **task, close);
                }
            });
        if (!closed) {
            callbacks.dispatch(view, task, command, close);
        }
    }
    return task.Get();
}
