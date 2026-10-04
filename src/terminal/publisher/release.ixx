// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.publisher.release;

import std;
import devicefs.terminal.formatting;
import devicefs.terminal.menu;
import devicefs.terminal.text_input;
import devicefs.terminal.transcoding;

using namespace std::string_view_literals;

export namespace devicefs::publisher {

struct Asset {
    std::string source;
    std::string name;
};

struct Options {
    bool preview = false;
    bool help = false;
    std::string repository;
    std::string directory = ".";
    std::string notes_command;
    std::string prepare;
    std::vector<Asset> assets;
};

[[nodiscard]] auto ParseOptions(
    const std::span<const std::string_view> arguments) -> Options {
    auto result = Options{};
    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        if (option == "--preview"sv) {
            result.preview = true;
            continue;
        }
        if ((option == "--help"sv) || (option == "-h"sv)) {
            result.help = true;
            continue;
        }
        if ((option != "--repository"sv) && (option != "--directory"sv) &&
            (option != "--notes-command"sv) && (option != "--prepare"sv) &&
            (option != "--asset"sv)) {
            throw std::invalid_argument(
                std::format("unknown option '{}'", option));
        }
        if (++index == arguments.size()) {
            throw std::invalid_argument(
                std::format("option '{}' requires a value", option));
        }
        const auto value = arguments[index];
        if (option == "--repository"sv) {
            result.repository = value;
        } else if (option == "--directory"sv) {
            result.directory = value;
        } else if (option == "--notes-command"sv) {
            result.notes_command = value;
        } else if (option == "--prepare"sv) {
            result.prepare = value;
        } else {
            const auto separator = value.find('=');
            if ((separator == std::string_view::npos) || (separator == 0) ||
                (separator + 1 == value.size())) {
                throw std::invalid_argument(std::format(
                    "asset '{}' must have the form SOURCE=PUBLISHED-NAME",
                    value));
            }
            result.assets.push_back({
                .source = std::string{value.substr(0, separator)},
                .name = std::string{value.substr(separator + 1)},
            });
        }
    }
    if (!result.help && !result.preview &&
        ((result.assets.empty()) || (result.notes_command.empty()))) {
        throw std::invalid_argument(
            "publication requires --asset and --notes-command");
    }
    return result;
}

[[nodiscard]] auto Path(const std::string_view text) -> std::filesystem::path {
#ifdef _WIN32
    return devicefs::terminal::Transcode<std::wstring>(text);
#else
    return text;
#endif
}

[[nodiscard]] auto PathText(const std::filesystem::path &path) -> std::string {
#ifdef _WIN32
    return devicefs::terminal::Transcode<std::string>(
        std::wstring_view{path.native()});
#else
    return path.native();
#endif
}

struct PublicationContext {
    std::string checkout;
    std::optional<std::string> branch;

    auto Draw(auto &frame) const -> void {
        frame.Write("Checkout: {}\n", checkout);
        if (branch) {
            frame.Write("Branch: {}\n", branch->empty() ?
                "(detached HEAD)"sv : std::string_view{*branch});
        }
        frame.Write("\n"sv);
    }
};

[[nodiscard]] auto TrimTrailingNewlines(std::string text) noexcept
    -> std::string {
    while (!text.empty() &&
        ((text.back() == '\n') || (text.back() == '\r'))) {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] auto NormalizeTag(const std::string_view input)
    -> std::optional<std::string> {
    static const auto pattern = std::regex{R"(v?[0-9]+\.[0-9]+(\.[0-9]+)?)"};
    if (!std::regex_match(input.begin(), input.end(), pattern)) {
        return std::nullopt;
    }
    return input.starts_with('v') ?
        std::string{input} : std::format("v{}", input);
}

[[nodiscard]] auto ExpandTag(std::string text, const std::string_view tag)
    -> std::string {
    constexpr auto token = "{tag}"sv;
    for (auto offset = text.find(token); offset != std::string::npos;
        offset = text.find(token, offset + tag.size())) {
        text.replace(offset, token.size(), tag);
    }
    return text;
}

[[nodiscard]] auto ReleaseAssets(const std::span<const Asset> assets,
    const std::string_view tag) -> std::vector<Asset> {
    auto result = std::vector<Asset>{};
    auto names = std::set<std::string>{};
    for (const auto &asset : assets) {
        auto name = ExpandTag(asset.name, tag);
        if (name.empty() || (name == "."sv) || (name == ".."sv) ||
            (name.find_first_of("/\\#") != std::string::npos) ||
            (!names.insert(name).second)) {
            throw std::invalid_argument(std::format(
                "invalid or duplicate published asset name '{}'", name));
        }
        result.push_back(
            {.source = ExpandTag(asset.source, tag), .name = std::move(name)});
    }
    return result;
}

[[nodiscard]] auto Lines(const std::string_view text)
    -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    for (const auto line : text | std::views::split('\n')) {
        auto view = std::string_view{line};
        if (view.ends_with('\r')) {
            view.remove_suffix(1);
        }
        if (!view.empty()) {
            result.emplace_back(view);
        }
    }
    return result;
}

// Ask for a release tag using the existing menu and editor. Previous versions
// remain in the editor's header; `Frame` supplies wrapping and text
// preparation.
[[nodiscard]] auto ChooseTag(auto &terminal,
    const PublicationContext &context, const std::span<const std::string> tags)
    -> std::optional<std::string> {
    using namespace devicefs::terminal;
    auto entries = std::vector<std::string_view>{"New tag"sv};
    for (const auto &tag : tags) {
        entries.push_back(tag);
    }
    const auto previous = tags | std::views::take(10) |
        std::views::join_with(", "sv) | std::ranges::to<std::string>();
    while (const auto selected = SelectMenuItem(terminal,
        [&](auto &frame) {
            context.Draw(frame);
            frame.Write("Choose a release tag\n\n"sv);
        },
        entries)) {
        if (*selected != 0) {
            return tags[*selected - 1];
        }
        auto entered = std::string{};
        auto error = std::string{};
        for (;;) {
            auto edited = EditText(terminal,
                [&](auto &frame) {
                    context.Draw(frame);
                    frame.Write(
                        "New release tag\nPrevious tags: {}\n\n"
                        "Enter X.Y or X.Y.Z, with an optional v prefix.\n"
                        "{}\n\n", previous, error);
                },
                entered, {.rows = 3});
            if (!edited) {
                break;
            }
            if (auto tag = NormalizeTag(*edited)) {
                return tag;
            }
            entered = std::move(*edited);
            error = "Invalid version. Use X.Y or X.Y.Z.";
        }
    }
    return std::nullopt;
}

template <class... Arguments>
[[nodiscard]] auto Confirm(auto &terminal, const PublicationContext &context,
    const bool default_yes,
    const devicefs::terminal::TerminalFormatString<Arguments...> format,
    Arguments &&...arguments) -> bool {
    const auto question =
        devicefs::terminal::PreparedText{devicefs::terminal::FormatTerminalText(
            format, std::forward<Arguments>(arguments)...)};
    const auto choice = devicefs::terminal::SelectMenuItem(terminal,
        [&](auto &frame) {
            context.Draw(frame);
            frame.Write("{}\n\n", question);
        },
        std::array{"Proceed"sv, "Go back"sv}, {}, default_yes ? 0 : 1);
    return choice && (*choice == 0);
}

}
