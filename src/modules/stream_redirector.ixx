// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.stream_redirector;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <cerrno>;
import <io.h>;

namespace stream_redirector_detail {

// `_dup` returns `-1` on failure; zero is a valid file descriptor. The final
// template argument makes WIL use `-1` as the empty value for this owner.
using FileDescriptor = wil::unique_any<int, decltype(&_close), &_close,
    wil::details::pointer_access_all, int, int, -1>;

} // namespace stream_redirector_detail

// Redirect one standard stream until `Restore` or destruction. `Restore`
// reports failures; the destructor attempts restoration without throwing so
// that cleanup does not replace an exception already being propagated.
export class RedirectedStream {
public:
    RedirectedStream(const int input_fd, const int output_fd,
        const std::string_view input_filename) :
        input_fd_(input_fd), input_filename_(input_filename),
        original_input_fd_(_dup(input_fd_)) {
        if (!original_input_fd_) {
            WinError("failed to save {} before redirection", input_filename_,
                ExplicitCrtIoError{});
        }
        // Changing `stdout` or `stderr` must also change the handle used by
        // child launches. For console applications, the Universal C Runtime's
        // `_dup2` implementation calls `SetStdHandle` when replacing either
        // descriptor (see `lowio/dup2.cpp` and `lowio/osfinfo.cpp`).
        if (_dup2(output_fd, input_fd_) != 0) {
            WinError("failed to redirect {} to the destination descriptor", input_filename_,
                ExplicitCrtIoError{});
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
        ClearCrtIoError();
        if (!TryRestore()) {
            return;
        }
        WinError("failed to restore {} after redirection", input_filename_,
            ExplicitCrtIoError{});
    }

private:
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
    stream_redirector_detail::FileDescriptor original_input_fd_;
};
