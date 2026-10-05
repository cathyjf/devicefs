// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

export module devicefs.publisher.unix_process;

import std;
export import devicefs.publisher.base_process;
import devicefs.terminal.scope_exit;
import devicefs.terminal.safecast;

extern "C" {
    extern char **environ;
}

namespace devicefs::publisher::unix_detail {

// A lambda inlined into the definition of `PipeStream` would be TU-local.
// By contrast, the type of this named `PipeStreamDeleter` object can be
// used in the module interface.
auto PipeStreamDeleter = [](std::FILE *const file) noexcept {
    std::ignore = std::fclose(file);
};

using PipeStream = std::unique_ptr<std::FILE, decltype(PipeStreamDeleter)>;

struct Pipe {
    PipeStream read;
    PipeStream write;
};

[[nodiscard]] auto MakePipe() -> Pipe {
    auto descriptors = std::array<int, 2>{};
    if (pipe(descriptors.data()) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not create a child-output pipe");
    }
    const auto cleanup = devicefs::terminal::ScopeExit{[&descriptors] {
        for (const auto descriptor : descriptors) {
            if (descriptor >= 0) {
                std::ignore = close(descriptor);
            }
        }
    }};
    for (auto &descriptor : descriptors) {
        // Keeping pipe descriptors above stderr prevents `dup2` from
        // overwriting a source if the publisher started with a closed standard
        // descriptor.
        if (descriptor < 3) {
            const auto duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
            if (duplicate < 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not relocate a child-output pipe descriptor");
            }
            std::ignore = close(std::exchange(descriptor, duplicate));
        }
        if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not prevent child-output pipe inheritance");
        }
    }
    const auto adopt = [&descriptors](const auto index) {
        auto stream = PipeStream{fdopen(
            descriptors[index], (index == 0) ? "r" : "w")};
        if (!stream) {
            throw std::system_error(errno, std::generic_category(),
                "could not open a stream for a child pipe");
        }
        descriptors[index] = -1;
        return stream;
    };
    return {.read = adopt(0), .write = adopt(1)};
}

struct Cancellation {
    const Pipe pipe = MakePipe();
    std::atomic_flag requested;

    auto Stop() noexcept -> void {
        if (!requested.test_and_set()) {
            // Leave the byte unread so every I/O worker observes cancellation.
            // Only one byte is written, so the cancellation pipe cannot fill.
            while ((write(fileno(pipe.write.get()), "x", 1) < 0) &&
                (errno == EINTR)) {}
        }
    }

    [[nodiscard]] auto Wait(const int descriptor, const short events) const
        -> bool {
        auto descriptors = std::array{
            pollfd{.fd = fileno(pipe.read.get()), .events = POLLIN,
                .revents = 0},
            pollfd{.fd = descriptor, .events = events, .revents = 0}};
        for (;;) {
            if (poll(descriptors.data(), descriptors.size(), -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                    "could not wait for child-pipe I/O or cancellation");
            }
            return descriptors.front().revents == 0;
        }
    }
};

struct Endpoint {
    PipeStream stream;
    std::shared_ptr<Cancellation> cancellation;
};

[[nodiscard]] auto Cancellable(PipeStream stream,
    const std::shared_ptr<Cancellation> &cancellation) -> Endpoint {
    if (fcntl(fileno(stream.get()), F_SETFL, O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not enable cancellable child-pipe I/O");
    }
    return {.stream = std::move(stream), .cancellation = cancellation};
}

}

export namespace devicefs::publisher {

// Launch an executable with inherited environment and working directory, EOF
// on stdin after sending any supplied input, and separate output pipes.
// A writer feeds stdin independently of output consumption. Destruction
// terminates the direct child, joins the I/O workers, and then reaps the child;
// it does not supervise descendants.
class UnixProcess : public BaseProcess {
public:
    explicit UnixProcess(const std::span<const std::string> arguments,
        std::string input = {},
        const std::function<void(std::string_view)> &write_output = {})
        : BaseProcess(arguments) {
        auto output = unix_detail::MakePipe();
        auto diagnostic = unix_detail::MakePipe();
        StartReaders(
            unix_detail::Cancellable(std::move(output.read), cancellation_),
            unix_detail::Cancellable(
                std::move(diagnostic.read), cancellation_), write_output);
        const auto input_source = [this, &input]() -> unix_detail::PipeStream {
            if (input.empty()) {
                return {};
            }
            auto pipe = unix_detail::MakePipe();
#ifdef __APPLE__
            // A failed launch or early child exit can close stdin during a
            // write. On macOS, XNU's `fp_writev` sends the resulting `SIGPIPE`
            // to the process through `psignal`, not to the writing thread.
            // Blocking it in the writer therefore leaves other threads
            // exposed; our launch-failure test terminated for this reason.
            // macOS `fcntl(2)` documents `F_SETNOSIGPIPE` for pipes. It stops
            // signal generation on this descriptor while retaining the
            // `EPIPE` error, without changing process-wide signal handling.
            // https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/kern/sys_generic.c#L571-L576
            if (fcntl(fileno(pipe.write.get()), F_SETNOSIGPIPE, 1) < 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not suppress SIGPIPE on the child-stdin pipe");
            }
#endif
            input_writer_ = StartWorker(
                [](const unix_detail::Endpoint writer,
                    const std::string input) {
#ifndef __APPLE__
                    // POSIX.1's 2004 corrigendum changed broken-pipe `SIGPIPE`
                    // from process-directed to thread-directed. Blocking it
                    // here lets `write` report `EPIPE` without terminating the
                    // publisher or changing its other threads' signal handling.
                    // https://pubs.opengroup.org/onlinepubs/9690949599/functions/write.html
                    auto mask = sigset_t{};
                    std::ignore = sigemptyset(&mask);
                    std::ignore = sigaddset(&mask, SIGPIPE);
                    if (const auto result =
                            pthread_sigmask(SIG_BLOCK, &mask, nullptr);
                        result != 0) {
                        throw std::system_error(result,
                            std::generic_category(),
                            "could not block SIGPIPE in the stdin writer");
                    }
#endif
                    auto remaining = std::string_view{input};
                    while (!remaining.empty()) {
                        const auto descriptor = fileno(writer.stream.get());
                        if (!writer.cancellation->Wait(descriptor, POLLOUT)) {
                            return;
                        }
                        const auto count = write(descriptor,
                            remaining.data(),
                            std::min(remaining.size(), 8192uz));
                        if (count < 0) {
                            if ((errno == EINTR) || (errno == EAGAIN) ||
                                (errno == EWOULDBLOCK)) {
                                continue;
                            }
                            throw std::system_error(errno,
                                std::generic_category(),
                                "could not write the child's standard input");
                        }
                        if (count == 0) {
                            throw std::system_error(
                                std::make_error_code(std::errc::io_error),
                                "child-stdin write made no progress");
                        }
                        remaining.remove_prefix(devicefs::terminal::
                            FailFastCast<std::size_t>(count));
                    }
                }, unix_detail::Cancellable(
                    std::move(pipe.write), cancellation_), std::move(input));
            return std::move(pipe.read);
        }();
        auto actions = posix_spawn_file_actions_t{};
        const auto check = [](const int result) {
            if (result != 0) {
                throw std::system_error(result, std::generic_category(),
                    "could not configure child-process descriptors");
            }
        };
        check(posix_spawn_file_actions_init(&actions));
        const auto cleanup = devicefs::terminal::ScopeExit{[&actions] {
            std::ignore = posix_spawn_file_actions_destroy(&actions);
        }};
        if (input_source) {
            check(posix_spawn_file_actions_adddup2(
                &actions, fileno(input_source.get()), STDIN_FILENO));
        } else {
            check(posix_spawn_file_actions_addopen(
                &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        }
        check(posix_spawn_file_actions_adddup2(
            &actions, fileno(output.write.get()), STDOUT_FILENO));
        check(posix_spawn_file_actions_adddup2(
            &actions, fileno(diagnostic.write.get()), STDERR_FILENO));
        auto argument_storage = arguments |
            std::ranges::to<std::vector<std::string>>();
        const auto argv =
            std::views::iota(0uz, argument_storage.size() + 1) |
            std::views::transform([&argument_storage](const auto index) {
                return (index < argument_storage.size()) ?
                    argument_storage[index].data() : nullptr;
            }) | std::ranges::to<std::vector>();
        const auto result = posix_spawnp(
            &pid_, argv.front(), &actions, nullptr, argv.data(), environ);
        if (result != 0) {
            throw std::system_error(result, std::generic_category(),
                std::format("could not start '{}'", arguments.front()));
        }
    }

    UnixProcess(const UnixProcess &) = delete;
    auto operator=(const UnixProcess &) -> UnixProcess & = delete;
    auto Terminate() const noexcept -> void {
        cancellation_->Stop();
        std::ignore = kill(pid_, SIGKILL);
    }

    ~UnixProcess() {
        if (pid_ > 0) {
            Terminate();
            JoinIo();
            // `waitpid` reaps the terminated child. A signal can interrupt
            // that wait without reaping it, so only `EINTR` warrants retrying.
            while ((waitpid(pid_, nullptr, 0) < 0) && (errno == EINTR)) {}
        }
    }

private:
    friend class BaseProcess;

    [[nodiscard]] static auto Read(const unix_detail::Endpoint &source)
        -> std::string {
        auto buffer = std::array<char, 8192>{};
        for (;;) {
            const auto descriptor = fileno(source.stream.get());
            if (!source.cancellation->Wait(descriptor, POLLIN)) {
                return {};
            }
            const auto count = read(
                descriptor, buffer.data(), buffer.size());
            if (count < 0) {
                if ((errno == EINTR) || (errno == EAGAIN) ||
                    (errno == EWOULDBLOCK)) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                    "could not read a child-output pipe");
            }
            return {buffer.data(),
                devicefs::terminal::FailFastCast<std::size_t>(count)};
        }
    }

    [[nodiscard]] auto WaitForExit() const -> int {
        // `WNOWAIT` leaves the exited child unreaped until destruction. Its PID
        // therefore remains reserved while another thread can call `Terminate`;
        // reaping it here would permit cancellation to signal a reused PID.
        auto status = siginfo_t{};
        while (waitid(P_PID,
            devicefs::terminal::FailFastCast<id_t>(pid_), &status,
            WEXITED | WNOWAIT) < 0) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::generic_category(),
                    "could not wait for the child process");
            }
        }
        return (status.si_code == CLD_EXITED) ?
            status.si_status : 128 + status.si_status;
    }

    const std::shared_ptr<unix_detail::Cancellation> cancellation_ =
        std::make_shared<unix_detail::Cancellation>();
    pid_t pid_ = -1;
};

}
