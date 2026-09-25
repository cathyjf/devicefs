// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.supervisor.view_output;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <cstdio>;
import <io.h>;
import <share.h>;
import devicefs.supervisor.temporary_paths;
import devicefs.terminal.menu;
import devicefs.terminal.windows;

namespace view_output_detail {

// `_dup` returns `-1` on failure; zero is a valid file descriptor. The final
// template argument makes WIL use `-1` as the empty value for this owner.
using FileDescriptor = wil::unique_any<int, decltype(&_close), &_close,
    wil::details::pointer_access_all, int, int, -1>;

// Redirect one standard stream until `Restore` or destruction. `Restore`
// reports failures; the destructor attempts restoration without throwing so
// that cleanup does not replace an exception already being propagated.
class RedirectedStream {
public:
    RedirectedStream(const int input_fd, const int output_fd,
        const std::string_view input_filename) :
        input_fd_(input_fd), input_filename_(input_filename),
        original_input_fd_(_dup(input_fd_)) {
        if (!original_input_fd_) {
            ThrowError("failed to save {} before redirecting view output");
        }
        // Changing `stdout` or `stderr` must also change the handle used by
        // child launches. For console applications, the Universal C Runtime's
        // `_dup2` implementation calls `SetStdHandle` when replacing either
        // descriptor (see `lowio/dup2.cpp` and `lowio/osfinfo.cpp`).
        if (_dup2(output_fd, input_fd_) != 0) {
            ThrowError("failed to redirect {} to the view output file");
        }
    }

    RedirectedStream(const RedirectedStream &) = delete;
    auto operator=(const RedirectedStream &) -> RedirectedStream & = delete;
    RedirectedStream(RedirectedStream &&) = delete;
    auto operator=(RedirectedStream &&) -> RedirectedStream & = delete;

    ~RedirectedStream() {
        TryRestore();
    }

    auto Restore() -> void {
        if (!TryRestore()) {
            return;
        }
        ThrowError("failed to restore {} after displaying view output");
    }

private:
    // `_doserrno` preserves failures from Windows, but the runtime sets it to
    // zero for its own failures, such as exhausting its file descriptors.
    // Those failures are described by `errno` instead.
    [[noreturn]] auto ThrowError(
        const std::format_string<const std::string_view &> format) const -> void {
        const auto crt_error = errno;
        const auto windows_error = _doserrno;
        const auto operation = std::format(format, input_filename_);
        if (windows_error != 0) {
            WinError("{}", operation, ExplicitWin32Error{windows_error});
        }
        throw std::system_error(crt_error, std::generic_category(), operation);
    }

    auto TryRestore() noexcept -> int {
        if (!original_input_fd_) {
            return 0;
        }
        if (_dup2(original_input_fd_.get(), input_fd_) != 0) {
            return errno;
        }
        original_input_fd_.reset();
        return 0;
    }

    const int input_fd_;
    const std::string_view input_filename_;
    FileDescriptor original_input_fd_;
};

class ViewOutput {
public:
    explicit ViewOutput(std::filesystem::path path) :
        path_(std::move(path)),
        file_([this] {
            // `wbT` creates a binary file and marks it as short-lived for
            // caching. `_SH_DENYNO` allows `reader_` to open the same file
            // while the redirected streams are writing to it.
            auto file = wil::unique_file{_wfsopen(path_.c_str(), L"wbT", _SH_DENYNO)};
            if (!file) {
                WinError("failed to create view output file '{}'",
                    std::wstring_view{path_.native()}, ExplicitWin32Error{_doserrno});
            }
            return file;
        }()),
        reader_([this] {
            auto reader = std::ifstream{path_, std::ios::binary};
            if (!reader) {
                WinError("failed to open view output file '{}' for reading",
                    std::wstring_view{path_.native()}, ExplicitWin32Error{_doserrno});
            }
            return reader;
        }()),
        standard_output_(_fileno(stdout), _fileno(file_.get()), "stdout"),
        standard_error_(_fileno(stderr), _fileno(file_.get()), "stderr") {}

    auto ReadOutput(devicefs::terminal::OutputMenu &view) {
        // Reaching the current end of the file does not mean output has
        // finished. Clearing that state allows newly written text to be read.
        reader_.clear();
        for (auto line = std::string{}; std::getline(reader_, line);) {
            view.AppendText(line);
            // `std::getline` removes the newline when it consumes one. At
            // end-of-file, the text remains open for the next read to continue.
            if (!reader_.eof()) {
                view.AppendText("\n");
            }
        }
        if (reader_.bad()) {
            WinError("failed to read view output file '{}'",
                std::wstring_view{path_.native()}, ExplicitWin32Error{_doserrno});
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
