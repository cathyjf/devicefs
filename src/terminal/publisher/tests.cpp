// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import devicefs.publisher.release;
import devicefs.terminal.test_support;
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

using namespace std::string_view_literals;
using namespace devicefs::publisher;
using namespace devicefs::terminal::tests;

namespace {

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
        while (!process.Poll().exit_code) {}
        const auto &result = process.Result();
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
        while (!process.Poll().exit_code) {}
        const auto &result = process.Result();
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
                while (!process.Poll().exit_code) {}
                const auto &result = process.Result();
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
        [&] {
            try {
                auto process = NativeProcess{
                    std::vector<std::string>{executable, "--bulk-child"},
                    std::string(1048576, 'n')};
                while (!process.Poll().exit_code) {}
            } catch (const std::system_error &) {
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
