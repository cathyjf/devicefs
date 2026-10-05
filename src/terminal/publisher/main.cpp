// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
    #include <devicefs/strsafe_compat.h>
#else
    #include <cerrno>
    #include <clocale>
    #include <cstdlib>
#endif

import std;
import devicefs.publisher.release;
import devicefs.publisher.preview;
import devicefs.terminal;
import devicefs.terminal.formatting;
import devicefs.terminal.menu;
import devicefs.terminal.scope_exit;
#ifdef _WIN32
    import <clocale>;
    import <cstdlib>;
    import devicefs.terminal.windows;
    import devicefs.publisher.windows_process;
#else
    import devicefs.terminal.unix;
    import devicefs.publisher.unix_process;
#endif

using namespace std::string_view_literals;
using namespace devicefs::publisher;
using namespace devicefs::terminal;

namespace {

#ifdef _WIN32
using Console = WindowsConsole;
using NativeProcess = WindowsProcess;
#else
using Console = UnixConsole;
using NativeProcess = UnixProcess;
#endif

[[nodiscard]] auto FormatCommand(const std::span<const std::string> arguments)
    -> std::string {
    return arguments | std::views::transform([](const auto &argument) {
        return std::format("{:?}", argument);
    }) | std::views::join_with(' ') |
        std::ranges::to<std::string>();
}

class CommandFailure : public std::runtime_error {
public:
    CommandFailure(const std::span<const std::string> command,
        const ProcessOutput &result)
        : std::runtime_error(std::format(
            "command {} failed with exit status {}\n{}",
            FormatCommand(command), result.exit_code, result.diagnostic)),
          exit_code(result.exit_code) {}
    const int exit_code;
};

enum class CommandOutput { OnFailure, Stream };
enum class CommandExit { Zero, ZeroOrOne, Inspect };

// Commands keep separate machine-readable stdout and diagnostic stderr. The
// scrolling view receives both, but parsing never sees progress or warnings.
class Commands {
public:
    Commands(Console &terminal, const PublicationContext &context) noexcept
        : terminal_(terminal), context_(context) {}

    [[nodiscard]] auto Run(const std::string_view title,
        const std::vector<std::string> &arguments,
        const CommandOutput output = CommandOutput::OnFailure,
        const CommandExit exit = CommandExit::Zero, std::string input = {})
        -> ProcessOutput {
        auto view = OutputMenu{};
        view.AppendLine(FormatCommand(arguments));
        struct CommandState {
            std::mutex mutex;
            std::string output;
            std::optional<ProcessOutput> result;
            std::exception_ptr error;
        };
        auto state = CommandState{};
        auto process = NativeProcess{arguments, std::move(input),
            [&state](const std::string_view text) {
                const auto lock = std::lock_guard{state.mutex};
                state.output += text;
            }};
        auto command = std::async(std::launch::async, [&process, &state] {
            try {
                auto result = process.Wait();
                const auto lock = std::lock_guard{state.mutex};
                state.result.emplace(std::move(result));
            } catch (...) {
                const auto lock = std::lock_guard{state.mutex};
                state.error = std::current_exception();
            }
        });
        auto result = std::optional<ProcessOutput>{};
        // Cancelling the view must terminate the child before `command` joins
        // its worker. Otherwise destruction would wait for the command to
        // finish naturally, leaving cancellation unable to stop it.
        const auto stop = ScopeExit{[&process, &result] {
            if (!result) {
                process.Terminate();
            }
        }};
        const auto failed = [&result, exit] {
            return (result->exit_code != 0) &&
                !((exit == CommandExit::ZeroOrOne) &&
                    (result->exit_code == 1));
        };
        const auto update = [&state, &result, &failed, exit](auto &view) {
            if (result) {
                return;
            }
            auto pending = [&state] {
                const auto lock = std::lock_guard{state.mutex};
                if (state.error) {
                    std::rethrow_exception(state.error);
                }
                return std::pair{std::exchange(state.output, {}),
                    std::move(state.result)};
            }();
            view.AppendText(pending.first);
            if (pending.second) {
                result = std::move(pending.second);
                view.AppendLine(
                    std::format("Exit status: {}", result->exit_code));
                view.SetCommands(std::array{OutputCommand{1,
                    (failed() && (exit != CommandExit::Inspect)) ?
                        "Close"sv : "Continue"sv}});
            }
        };
        if (output == CommandOutput::OnFailure) {
            command.get();
            update(view);
            if (!failed()) {
                return std::move(*result);
            }
        }
        const auto choice = view.Select(terminal_,
            [&](auto &frame) {
                context_.Draw(frame);
                frame.Write("{}\n\n", title);
            }, update);
        if (!result) {
            throw InputCancelled{};
        }
        if (failed() && (exit != CommandExit::Inspect)) {
            throw CommandFailure{arguments, *result};
        }
        if (!choice) {
            throw InputCancelled{};
        }
        return std::move(*result);
    }

private:
    Console &terminal_;
    const PublicationContext &context_;
};

[[nodiscard]] auto ReadCheckout(Console &terminal) -> PublicationContext {
    auto context = PublicationContext{
        .checkout = PathText(std::filesystem::current_path()),
        .branch = std::nullopt};
    auto commands = Commands{terminal, context};
    context.branch = TrimTrailingNewlines(commands.Run(
        "Read checkout branch"sv, {"git", "branch", "--show-current"}).output);
    context.checkout = TrimTrailingNewlines(commands.Run(
        "Read checkout directory"sv,
        {"git", "rev-parse", "--show-toplevel"}).output);
    return context;
}

[[nodiscard]] auto Publish(Console &terminal, const PublicationContext &context,
    const Options &options) -> std::string {
    auto commands = Commands{terminal, context};
    const auto capture = [&](const std::vector<std::string> &arguments) {
        return TrimTrailingNewlines(
            commands.Run("Read release information"sv, arguments).output);
    };
    const auto repository = options.repository.empty() ?
        capture({"gh", "repo", "view", "--json", "nameWithOwner", "--jq",
            ".nameWithOwner"}) : options.repository;
    const auto root = Path(context.checkout);
    std::filesystem::current_path(root);
    const auto directory = std::filesystem::absolute(Path(options.directory));
    const auto confirm_directory = [&] {
        if (!Confirm(terminal, context, true,
                "Repository: {}\nUse release directory '{}'?", repository,
                PathText(directory))) {
            throw InputCancelled{};
        }
    };
    confirm_directory();
    const auto commit = capture({"git", "rev-parse", "HEAD"});
    const auto &branch = *context.branch;
    const auto remote = [&] {
        if (branch.empty()) {
            return std::string{"origin"};
        }
        const auto arguments = std::vector<std::string>{
            "git", "config", "--get", std::format("branch.{}.remote", branch)};
        const auto result =
            commands.Run("Read release information"sv, arguments,
                CommandOutput::OnFailure, CommandExit::ZeroOrOne);
        if (result.exit_code == 1) {
            return std::string{"origin"};
        }
        const auto lines = Lines(result.output);
        if (lines.empty()) {
            throw std::runtime_error(std::format(
                "Git branch '{}' has an empty remote configuration", branch));
        }
        return lines.front();
    }();
    auto tags =
        Lines(capture({"git", "tag", "--list", "--sort=-version:refname"}));
    std::erase_if(tags, [](const auto &tag) {
        return !NormalizeTag(tag);
    });

    enum class TagAction { Create, Reuse, Replace };
    struct Tag {
        std::string name;
        TagAction action;
    };
    const auto tag = [&]() -> Tag {
        for (;;) {
            const auto name = ChooseTag(terminal, context, tags);
            if (!name) {
                confirm_directory();
                continue;
            }
            const auto ref = std::format("refs/tags/{}", *name);
            const auto arguments = std::vector<std::string>{
                "git", "show-ref", "--verify", "--quiet", ref};
            const auto exists =
                commands.Run("Read release information"sv, arguments,
                    CommandOutput::OnFailure, CommandExit::ZeroOrOne);
            struct Decision {
                TagAction action;
                PreparedText question;
            };
            const auto decision = [&]() -> Decision {
                if (exists.exit_code == 1) {
                    return {.action = TagAction::Create,
                        .question = {FormatTerminalText(
                            "Create signed tag {} at {}?", *name, commit)}};
                }
                const auto tagged = capture(
                    {"git", "rev-parse", std::format("{}^{{commit}}", ref)});
                const auto annotated =
                    capture({"git", "cat-file", "-t", ref}) == "tag"sv;
                const auto signed_tag = annotated &&
                    (commands.Run("Verify existing tag"sv,
                        {"git", "verify-tag", *name}, CommandOutput::OnFailure,
                        CommandExit::Inspect).exit_code == 0);
                if (signed_tag && (tagged == commit)) {
                    return {.action = TagAction::Reuse,
                        .question = {FormatTerminalText(
                            "Reuse verified signed tag {} at {}?", *name,
                            commit)}};
                }
                return {.action = TagAction::Replace,
                    .question = {FormatTerminalText(
                        "Replace tag {}?\nTagged revision: {}\n"
                        "Release revision: {}\n{}",
                        *name, tagged, commit,
                        signed_tag ? "Its signature verifies."sv :
                            "It is unsigned or its signature could not "
                            "be verified."sv)}};
            }();
            if (!Confirm(terminal, context,
                    decision.action != TagAction::Replace, "{}",
                    decision.question)) {
                continue;
            }
            if ((decision.action == TagAction::Replace) &&
                (!Confirm(terminal, context, false,
                    "This requires a force-push. Proceed?"))) {
                continue;
            }
            return {.name = *name, .action = decision.action};
        }
    }();

    const auto assets = ReleaseAssets(options.assets, tag.name);
    if (!options.prepare.empty()) {
#ifdef _WIN32
        const auto result = _putenv_s("GITHUB_PUBLISHER_PREPARE", "1");
#else
        const auto result =
            (setenv("GITHUB_PUBLISHER_PREPARE", "1", 1) == 0) ? 0 : errno;
#endif
        if (result != 0) {
            throw std::system_error(result, std::generic_category(),
                "could not set GITHUB_PUBLISHER_PREPARE=1");
        }
        std::ignore = commands.Run("Prepare release assets"sv,
            {options.prepare, tag.name, PathText(directory)},
            CommandOutput::Stream);
    }
    const auto notes = commands.Run("Generate release notes"sv,
        {options.notes_command, tag.name, PathText(directory)}).output;
    std::filesystem::create_directories(root / "temp");
    auto random = std::random_device{};
    const auto staging = root / "temp" /
        std::format("publish-{:08x}{:08x}{:08x}{:08x}", random(), random(),
            random(), random());
    if (!std::filesystem::create_directory(staging)) {
        throw std::runtime_error(
            std::format("release staging directory '{}' already exists",
                PathText(staging)));
    }
    const auto cleanup = ScopeExit{[&staging] noexcept {
        auto error = std::error_code{};
        std::filesystem::remove_all(staging, error);
        if (error) {
            try {
                std::println(std::cerr,
                    "could not remove release staging directory '{}': {}",
                    PathText(staging), error.message());
            } catch (...) {}
        }
    }};
    auto uploads = std::vector<std::string>{};
    auto listing = std::string{};
    for (const auto &asset : assets) {
        const auto source = directory / Path(asset.source);
        const auto destination = staging / Path(asset.name);
        std::filesystem::copy_file(source, destination);
        uploads.push_back(PathText(destination));
        listing +=
            FormatTerminalText("{} -> {}\n", PathText(source), asset.name);
    }
    if (!Confirm(terminal, context, true,
            "Release {}\n{}\nSign, push and upload these assets?", tag.name,
            PreparedText{listing})) {
        throw InputCancelled{};
    }
    if (tag.action != TagAction::Reuse) {
        auto sign = std::vector<std::string>{
            "git", "tag", "--sign", "--message",
            std::format("version {}",
                tag.name.starts_with('v') ?
                    std::string_view{tag.name}.substr(1) :
                    std::string_view{tag.name})};
        if (tag.action == TagAction::Replace) {
            sign.push_back("--force");
        }
        sign.insert(sign.end(), {tag.name, commit});
        std::ignore = commands.Run(
            "Sign release tag"sv, sign, CommandOutput::Stream);
    }
    std::ignore =
        commands.Run("Verify release tag"sv, {"git", "verify-tag", tag.name},
            CommandOutput::Stream);
    auto push = std::vector<std::string>{"git", "push"};
    if (tag.action == TagAction::Replace) {
        push.push_back("--force");
    }
    push.insert(push.end(), {remote, std::format("refs/tags/{}", tag.name)});
    std::ignore = commands.Run(
        "Push release tag"sv, push, CommandOutput::Stream);

    const auto drafts = Lines(capture({"gh", "api", "--paginate",
        std::format("repos/{}/releases", repository), "--jq",
        std::format(
            ".[] | select(.draft and .tag_name == \"{}\") | .id", tag.name)}));
    if (!drafts.empty()) {
        if (!Confirm(terminal, context, false,
                "Delete {} existing draft release(s) for {}?", drafts.size(),
                tag.name)) {
            throw InputCancelled{};
        }
        for (const auto &id : drafts) {
            std::ignore = commands.Run("Remove existing drafts"sv,
                {"gh", "api", "--method", "DELETE",
                    std::format("repos/{}/releases/{}", repository, id)},
                CommandOutput::Stream);
        }
    }
    auto upload = std::vector<std::string>{"gh", "release", "create", tag.name,
        "--repo", repository, "--title", tag.name, "--notes-file", "-",
        "--draft", "--verify-tag"};
    upload.append_range(uploads);
    const auto uploaded = commands.Run("Upload draft release"sv, upload,
        CommandOutput::Stream, CommandExit::Zero, notes);
    const auto uploaded_lines = Lines(uploaded.output);
    if (uploaded_lines.empty()) {
        throw std::runtime_error(std::format(
            "GitHub reported success creating draft '{}' in '{}', "
            "but did not return its inspection URI", tag.name, repository));
    }
    const auto &uri = uploaded_lines.front();
    auto view = OutputMenu{};
    view.AppendLine(std::format("Draft uploaded: {}", uri));
    view.SetCommands(std::array{OutputCommand{1, "Open draft in browser"sv},
        OutputCommand{2, "Finish"sv}});
    while (const auto selected = view.Select(terminal,
        [&](auto &frame) {
            context.Draw(frame);
            frame.Write(
                "Inspect the draft before publishing it on GitHub.\n\n"sv);
        }, [](auto &) {})) {
        if (*selected == 2) {
            break;
        }
        try {
            std::ignore = commands.Run("Open draft in browser"sv,
                {"gh", "release", "view", tag.name, "--repo", repository,
                    "--web"});
        } catch (const InputCancelled &) {
            break;
        } catch (const std::exception &error) {
            view.AppendLine(error.what());
        }
    }
    return std::format(
        "You can inspect and finish publishing the draft at:\n{}", uri);
}

[[nodiscard]] auto Main(const std::span<const std::string_view> arguments)
    -> int {
    const auto options = ParseOptions(arguments.subspan(1));
    if (options.help) {
        std::println(
            "Usage: github-release-publisher [--preview]\n"
            "  [--repository OWNER/REPO] [--directory PATH]\n"
            "  --notes-command EXECUTABLE\n"
            "  --asset SOURCE=PUBLISHED-NAME [--asset ...]\n"
            "  [--prepare EXECUTABLE]\n\n"
            "Run from the project's Git checkout. Publication creates a\n"
            "signed tag and a draft release; final publication remains\n"
            "your decision.\n"
            "{{tag}} in filenames expands to the chosen tag (including v).\n"
            "Preparation and notes commands receive TAG and the absolute\n"
            "release directory. Before preparation, the publisher sets\n"
            "GITHUB_PUBLISHER_PREPARE=1 in its environment.\n"
            "The notes command writes its Markdown to stdout. Preview reads\n"
            "the checkout and branch, but runs no\n"
            "publication commands and makes no filesystem writes.");
        return 0;
    }
#ifdef _WIN32
    constexpr auto locale = ".UTF8";
#else
    constexpr auto locale = "C.UTF-8";
#endif
    if (std::setlocale(LC_CTYPE, locale) == nullptr) {
        throw std::runtime_error(
            std::format("could not select character encoding {}", locale));
    }
    const auto message = [&options] {
        auto terminal = Console{};
        const auto screen = terminal.EnterScreen();
        terminal.Write("Loading..."sv);
        const auto context = ReadCheckout(terminal);
        return options.preview ? Preview(terminal, context) :
            Publish(terminal, context, options);
    }();
    std::println("{}", message);
    return 0;
}

[[nodiscard]] auto Entry(const std::span<const std::string_view> arguments)
    -> int {
    try {
        return Main(arguments);
    } catch (const InputCancelled &) {
        std::println("Release preparation cancelled.");
        return 1;
    } catch (const CommandFailure &error) {
        std::println(std::cerr, "publisher: {}", error.what());
        return error.exit_code;
    } catch (const std::exception &error) {
        std::println(std::cerr, "publisher: {}", error.what());
        return 1;
    }
}

}

auto main(const int argc, char *const argv[]) -> int {
    const auto arguments = std::span{argv, argv + argc} |
        std::ranges::to<std::vector<std::string_view>>();
    return Entry(arguments);
}
