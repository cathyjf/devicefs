// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
    #include <windows.h>
    #include <wil/resource.h>
    #include <wil/win32_helpers.h>
#else
    #include <cerrno>
    #include <cstdio>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

import std;
import devicefs.publisher.release;
import devicefs.terminal.test_support;
import devicefs.terminal.scope_exit;
#ifdef _WIN32
    import <cerrno>;
    import <fcntl.h>;
    import <io.h>;
    import devicefs.publisher.windows_process;
using NativeProcess = devicefs::publisher::WindowsProcess;
#else
    import devicefs.publisher.unix_process;
using NativeProcess = devicefs::publisher::UnixProcess;
#endif

#ifndef _WIN32
extern "C" {
    extern char **environ;
}
#endif

using namespace std::string_view_literals;
using namespace devicefs::publisher;
using namespace devicefs::terminal::tests;

namespace {

[[nodiscard]] auto AcquireFixtureLock(const std::filesystem::path &path,
    const bool create = false) {
#ifdef _WIN32
    auto file = wil::unique_hfile{CreateFileW(path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, create ? CREATE_NEW : OPEN_EXISTING, 0, nullptr)};
    if (!file) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not open the publisher fixture lock");
    }
    auto offset = OVERLAPPED{};
    if (!LockFileEx(file.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &offset)) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not acquire the publisher fixture lock");
    }
#else
    const auto close = [](std::FILE *const file) {
        std::ignore = std::fclose(file);
    };
    auto file = std::unique_ptr<std::FILE, decltype(close)>{
        std::fopen(path.c_str(), create ? "w+x" : "r+")};
    if (!file) {
        throw std::system_error(errno, std::generic_category(),
            "could not open the publisher fixture lock");
    }
    const auto descriptor = fileno(file.get());
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not prevent publisher fixture lock inheritance");
    }
    while (flock(descriptor, LOCK_EX) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                "could not acquire the publisher fixture lock");
        }
    }
#endif
    return file;
}

auto LaunchPipeHolder(const std::string &executable,
    const std::string &directory) -> void {
    auto arguments = std::array{
        executable, std::string{"--pipe-holder"}, directory};
    // The fixture intentionally passes through all three standard handles.
    // Its descendant holds them until the test explicitly releases the lock.
#ifdef _WIN32
    auto command = wil::ArgvToCommandLine(arguments);
    auto startup = STARTUPINFOA{.cb = sizeof(STARTUPINFOA),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
        .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
        .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    auto process = wil::unique_process_information{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
            0, nullptr, nullptr, &startup, &process)) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not start the pipe-holder fixture");
    }
#else
    const auto argv = std::views::iota(0uz, arguments.size() + 1) |
        std::views::transform([&arguments](const auto index) {
            return (index < arguments.size()) ?
                arguments[index].data() : nullptr;
        }) | std::ranges::to<std::vector>();
    auto pid = pid_t{};
    if (const auto result = posix_spawnp(&pid, argv.front(), nullptr,
            nullptr, argv.data(), environ); result != 0) {
        throw std::system_error(result, std::generic_category(),
            "could not start the pipe-holder fixture");
    }
#endif
}

enum class HeldPipeOperation { Terminate, Destroy, ReaderFailure };

auto TestHeldPipes(const std::string &executable,
    const HeldPipeOperation operation, const std::string &input = {}) -> void {
    const auto directory = std::filesystem::temp_directory_path() /
        std::format("devicefs-publisher-{:x}-{:x}",
            std::random_device{}(), std::random_device{}());
    Require(std::filesystem::create_directory(directory),
        "could not create the pipe-holder fixture directory"sv);
    const auto cleanup = devicefs::terminal::ScopeExit{[&directory] {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(directory, ignored);
    }};
    auto gate = AcquireFixtureLock(directory / "gate", true);
    {
        const auto completion = AcquireFixtureLock(directory / "done", true);
    }
    auto ready = std::promise<void>{};
    auto mutex = std::mutex{};
    auto received = std::string{};
    {
        auto process = NativeProcess{std::array{
            executable, std::string{"--retaining-child"}, PathText(directory)},
            input, [&ready, &mutex, &received, operation](
                const std::string_view text) {
                const auto lock = std::lock_guard{mutex};
                received += text;
                if (received == "ready\n"sv) {
                    ready.set_value();
                    if (operation == HeldPipeOperation::ReaderFailure) {
                        throw std::runtime_error("streaming callback failed");
                    }
                }
            }};
        ready.get_future().get();
        // The holder cannot close its pipes until this scope has finished.
        // Waiting for their EOF on cancellation would therefore deadlock.
        if (operation == HeldPipeOperation::Terminate) {
            process.Terminate();
            Require(process.Wait().exit_code != 0,
                "terminated child reported successful completion"sv);
        } else if (operation == HeldPipeOperation::ReaderFailure) {
            auto failed = false;
            try {
                std::ignore = process.Wait();
            } catch (const std::runtime_error &error) {
                Require(error.what() == "streaming callback failed"sv,
                    "streaming failure was replaced by another error"sv);
                failed = true;
            }
            Require(failed, "streaming failure was not reported"sv);
        }
    }
    gate.reset();
    const auto completion = AcquireFixtureLock(directory / "done");
}

[[nodiscard]] auto TestPolicy() -> bool {
    auto passed = Test("preserving Unicode in filesystem paths"sv, [] {
        constexpr auto text = "directory/日本語 — 👩‍💻.zip"sv;
        Require(PathText(Path(text)) == text,
            "filesystem path conversion changed Unicode text"sv);
    });
    passed &= Test("normalizing a new version tag"sv, [] {
        Require(NormalizeTag("1.2"sv) == "v1.2"sv,
            "version normalization failed"sv);
    });
    passed &= Test("preserving an existing version tag"sv, [] {
        Require(NormalizeTag("v1.2.3"sv) == "v1.2.3"sv,
            "tag preservation failed"sv);
    });
    for (const auto invalid :
        std::array{""sv, "v1"sv, "1.2.3.4"sv, "1.2\n"sv, "1.x"sv}) {
        passed &= Test(
            std::format("rejecting invalid version {:?}", invalid), [invalid] {
                Require(
                    !NormalizeTag(invalid), "invalid version was accepted"sv);
            });
    }
    passed &= Test("substituting the tag into asset paths and names"sv, [] {
        Require(ExpandTag("{tag}/archive-{tag}.zip", "v1.2"sv) ==
                "v1.2/archive-v1.2.zip"sv,
            "asset tag substitution failed"sv);
    });
    passed &= Test("parsing captured LF and CRLF output"sv, [] {
        Require(Lines("a\r\nb\n"sv) == std::vector<std::string>{"a", "b"},
            "line parsing failed"sv);
    });
    for (const auto invalid :
        std::array{"../asset"sv, ".."sv, "a#label"sv, "a\\b"sv}) {
        passed &=
            Test(std::format("rejecting published filename {:?}", invalid),
                [invalid] {
                    try {
                        std::ignore = ReleaseAssets(
                            std::array{Asset{"source", std::string{invalid}}},
                            "v1.2"sv);
                    } catch (const std::invalid_argument &) {
                        return;
                    }
                    throw std::runtime_error(
                        "invalid published filename was accepted");
                });
    }
    passed &= Test("parsing publication options"sv, [] {
        const auto options = ParseOptions(std::array{"--notes-command"sv,
            "make-notes"sv, "--asset"sv, "archive.zip=archive-{tag}.zip"sv,
            "--directory"sv, "prepared"sv});
        Require(
            (options.assets.size() == 1) && (options.directory == "prepared"sv),
            "publication options were not parsed"sv);
    });
    passed &= Test("rejecting duplicate published filenames"sv, [] {
        try {
            std::ignore = ReleaseAssets(
                std::array{Asset{"one", "same"}, Asset{"two", "same"}},
                "v1.2"sv);
        } catch (const std::invalid_argument &) {
            return;
        }
        throw std::runtime_error("duplicate published filenames were accepted");
    });
    return passed;
}

[[nodiscard]] auto TestProcesses(const std::string &executable) -> bool {
    auto passed = true;
    passed &= Test("capturing child output"sv, [&] {
        const auto arguments = std::vector<std::string>{executable, "--child",
            "", "two words", "quote\"", "trailing\\",
            "日本語 — 👩‍💻", "first line\nsecond line"};
        auto process = NativeProcess{arguments};
        const auto result = process.Wait();
        passed &=
            Test("preserving child argument boundaries and Unicode"sv, [&] {
                const auto expected = arguments | std::views::drop(2) |
                    std::views::transform([](const auto &argument) {
                        return std::format(
                            "{}:{:?}", argument.size(), argument);
                    }) | std::ranges::to<std::vector>();
                Require(Lines(result.output) == expected,
                    "child argument boundaries changed"sv);
            });
        passed &= Test("retaining stderr separately from stdout"sv, [&] {
            Require(Lines(result.diagnostic) ==
                    std::vector<std::string>{"diagnostic"},
                "stderr was merged into captured stdout"sv);
        });
        passed &= Test("preserving a nonzero child exit status"sv, [&] {
            Require(
                result.exit_code == 7, "nonzero child exit status was lost"sv);
        });
    });
    passed &= Test("capturing output larger than pipe capacity"sv, [&] {
        auto process =
            NativeProcess{std::vector<std::string>{executable, "--bulk-child"}};
        const auto result = process.Wait();
        passed &= Test(
            "retaining large stdout and its final unterminated tail"sv, [&] {
                Require(result.output == std::string(1048576, 'x') + "tail",
                    "large or final unterminated output was lost"sv);
            });
        passed &= Test("retaining large stderr"sv, [&] {
            Require(result.diagnostic == std::string(1048576, 'y'),
                "large stderr was lost"sv);
        });
        passed &= Test("preserving a successful child exit status"sv, [&] {
            Require(result.exit_code == 0,
                "successful child exit status was lost"sv);
        });
    });
    passed &= Test("streaming both output pipes during a blocking wait"sv,
        [&executable] {
            auto mutex = std::mutex{};
            auto streamed = std::string{};
            auto process = NativeProcess{
                std::vector<std::string>{executable, "--bulk-child"}, {},
                [&mutex, &streamed](const std::string_view text) {
                    const auto lock = std::lock_guard{mutex};
                    streamed += text;
                }};
            const auto result = process.Wait();
            Require(streamed.size() ==
                    result.output.size() + result.diagnostic.size(),
                "streaming lost output bytes"sv);
            Require((std::ranges::count(streamed, 'x') == 1048576) &&
                    (std::ranges::count(streamed, 'y') == 1048576),
                "streaming changed stdout or stderr"sv);
            for (const auto character : "tail"sv) {
                Require(std::ranges::count(streamed, character) == 1,
                    "streaming lost the final unterminated tail"sv);
            }
        });
    passed &= Test("joining a command worker after child termination"sv,
        [&executable] {
            auto process = NativeProcess{
                std::vector<std::string>{executable, "--blocked-child"}};
            auto completion = std::async(std::launch::async,
                [&process] { return process.Wait(); });
            const auto stop = devicefs::terminal::ScopeExit{
                [&process] { process.Terminate(); }};
            process.Terminate();
            Require(completion.get().exit_code != 0,
                "terminated child reported successful completion"sv);
        });
    passed &= Test("propagating a streaming callback failure"sv,
        [&executable] {
            try {
                auto process = NativeProcess{
                    std::vector<std::string>{executable, "--child"}, {},
                    [](const std::string_view) {
                        throw std::runtime_error("streaming callback failed");
                    }};
                std::ignore = process.Wait();
            } catch (const std::runtime_error &error) {
                Require(error.what() == "streaming callback failed"sv,
                    "streaming failure was replaced by another error"sv);
                return;
            }
            throw std::runtime_error("streaming failure was not reported");
        });
    passed &= Test("cancelling while a descendant retains both output pipes"sv,
        [&executable] {
            TestHeldPipes(executable, HeldPipeOperation::Terminate);
        });
    passed &= Test("destruction without waiting for a descendant's pipe EOF"sv,
        [&executable] {
            TestHeldPipes(executable, HeldPipeOperation::Destroy);
        });
    passed &= Test("cancelling stdin retained by a non-reading descendant"sv,
        [&executable] {
            TestHeldPipes(executable, HeldPipeOperation::Terminate,
                std::string(1048576, 'n'));
        });
    passed &= Test("reporting reader failure before a blocked child exits"sv,
        [&executable] {
            TestHeldPipes(executable, HeldPipeOperation::ReaderFailure);
        });
    passed &= Test("reporting writer failure before a blocked child exits"sv,
        [&executable] {
            auto process = NativeProcess{std::array{
                executable, std::string{"--close-stdin-child"}},
                std::string(1048576, 'n')};
            try {
                std::ignore = process.Wait();
            } catch (const std::system_error &error) {
                Require(std::string_view{error.what()}.starts_with(
                    "could not write the child's standard input"sv),
                    "stdin failure was replaced by another error"sv);
                return;
            }
            throw std::runtime_error("incomplete stdin was not reported");
        });
    passed &= Test("reporting a missing executable"sv, [] {
        try {
            auto absent = NativeProcess{std::array{
                std::string{"devicefs-nonexistent-publisher-test-command"}}};
        } catch (const std::system_error &) {
            return;
        }
        throw std::runtime_error("missing executable was not reported");
    });
    for (const auto &input : std::array{std::string{},
        std::string{"First line\nSecond line\r\n日本語 — 👩‍💻"},
        std::string(1048576, 'n') + std::string{"\0tail", 5}}) {
        passed &= Test(std::format(
            "sending {} stdin bytes while draining output", input.size()), [&] {
                auto process = NativeProcess{
                    std::vector<std::string>{executable, "--stdin-child"},
                    input};
                const auto result = process.Wait();
                Require(result.output == std::string(1048576, 'x') +
                    std::format("{:?}", input),
                    "stdin bytes or interleaved output changed"sv);
                Require(result.exit_code == 0,
                    "stdin-consuming child did not exit successfully"sv);
            });
    }
    passed &= Test("releasing a blocked stdin writer after launch failure"sv,
        [] {
            try {
                auto absent = NativeProcess{std::array{
                    std::string{"devicefs-nonexistent-publisher-test-command"}},
                    std::string(1048576, 'n')};
            } catch (const std::system_error &) {
                return;
            }
            throw std::runtime_error("missing executable was not reported");
        });
    passed &= Test("reporting a child that closes stdin before consuming it"sv,
        [&executable] {
            auto process = NativeProcess{
                std::vector<std::string>{executable, "--bulk-child"},
                std::string(1048576, 'n')};
            try {
                std::ignore = process.Wait();
            } catch (const std::system_error &error) {
                Require(std::string_view{error.what()}.starts_with(
                    "could not write the child's standard input"sv),
                    "stdin failure was replaced by another error"sv);
                return;
            }
            throw std::runtime_error(
                "incomplete stdin delivery was not reported");
        });
    return passed;
}

}

auto main(const int argc, char *const argv[]) -> int {
    const auto arguments = std::span{argv, argv + argc} |
        std::ranges::to<std::vector<std::string_view>>();
    if ((arguments.size() >= 2) && (arguments[1] == "--child"sv)) {
        for (const auto argument : arguments | std::views::drop(2)) {
            std::println("{}:{:?}", argument.size(), argument);
        }
        std::println(std::cerr, "diagnostic");
        return 7;
    }
    if ((arguments.size() == 2) && (arguments[1] == "--bulk-child"sv)) {
        std::print("{}tail", std::string(1048576, 'x'));
        std::print(std::cerr, "{}", std::string(1048576, 'y'));
        return 0;
    }
    if ((arguments.size() == 3) && (arguments[1] == "--pipe-holder"sv)) {
#ifdef _WIN32
        // This acknowledgement is a pipe protocol, so the CRT must not
        // translate its LF into CRLF.
        if (_setmode(1, _O_BINARY) < 0) {
            std::println(std::cerr,
                "could not select binary fixture stdout: {}",
                std::generic_category().message(errno));
            return 1;
        }
#endif
        const auto directory = Path(arguments[2]);
        const auto completion = AcquireFixtureLock(directory / "done");
        std::cout << "ready\n" << std::flush;
        const auto gate = AcquireFixtureLock(directory / "gate");
        return 0;
    }
    if ((arguments.size() == 3) && (arguments[1] == "--retaining-child"sv)) {
        LaunchPipeHolder(PathText(std::filesystem::absolute(
            Path(arguments.front()))), std::string{arguments[2]});
        const auto gate = AcquireFixtureLock(Path(arguments[2]) / "gate");
        return 0;
    }
    if ((arguments.size() == 2) && (arguments[1] == "--close-stdin-child"sv)) {
#ifdef _WIN32
        std::ignore = _close(0);
#else
        std::ignore = close(STDIN_FILENO);
#endif
    }
    if ((arguments.size() >= 2) &&
        ((arguments[1] == "--blocked-child"sv) ||
            (arguments[1] == "--close-stdin-child"sv))) {
        std::binary_semaphore{0}.acquire();
        return 0;
    }
    if ((arguments.size() == 2) && (arguments[1] == "--stdin-child"sv)) {
#ifdef _WIN32
        // The child compares the original pipe bytes, not the CRT's
        // text-mode translation of CRLF or its treatment of Ctrl+Z as EOF.
        if (_setmode(0, _O_BINARY) < 0) {
            std::println(std::cerr,
                "could not select binary test stdin: {}",
                std::generic_category().message(errno));
            return 1;
        }
#endif
        std::print("{}", std::string(1048576, 'x'));
        const auto input = std::string{
            std::istreambuf_iterator<char>{std::cin}, {}};
        std::print("{:?}", input);
        return 0;
    }
    try {
        auto passed = TestPolicy();
        passed &= TestProcesses(
            PathText(std::filesystem::absolute(Path(arguments.front()))));
        std::println("\nPublisher tests {}.", passed ? "passed" : "failed");
        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::println(std::cerr, "publisher test: {}", error.what());
        return 1;
    }
}
