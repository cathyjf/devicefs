// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.publisher.preview;

import std;
import devicefs.publisher.release;
import devicefs.terminal.menu;

// The preview uses the same native console as `Console` in `main.cpp`.
// MSVC 14.51.36231 targeting ARM64 reported C1001 there when calling the
// imported `Preview` template. Giving `Preview` a concrete parameter type
// compiles its body here instead of instantiating it in the importer.
#ifdef _WIN32
    import devicefs.terminal.windows;
using PreviewConsole = devicefs::terminal::WindowsConsole;
#else
    import devicefs.terminal.unix;
using PreviewConsole = devicefs::terminal::UnixConsole;
#endif

using namespace std::string_view_literals;
using namespace std::chrono_literals;

export namespace devicefs::publisher {

[[nodiscard]] auto Preview(
    PreviewConsole &terminal, const PublicationContext &context)
    -> std::string {
    using namespace devicefs::terminal;
    const auto tags = std::vector<std::string>{"v1.4.0", "v1.3.0", "v1.2.0",
        "v1.1.0", "v1.0.0", "v0.9.0", "v0.8.0", "v0.7.0", "v0.6.0", "v0.5.0",
        "v0.4.0"};
    while (const auto tag = ChooseTag(terminal, context, tags)) {
        if (!Confirm(terminal, context, true,
                "UI preview: prepare release {}?\n"
                "No publication commands or filesystem writes will occur.",
                *tag)) {
            continue;
        }
        if ((*tag == "v1.3.0"sv) &&
            (!Confirm(terminal, context, false,
                "This sample tag identifies another revision. Replace it?"))) {
            continue;
        }
        if ((*tag == "v1.3.0"sv) &&
            (!Confirm(terminal, context, false,
                "Replacing this sample tag requires a force-push. Proceed?"))) {
            continue;
        }
        if ((*tag == "v1.4.0"sv) &&
            (!Confirm(terminal, context, false,
                "Delete the sample draft for this tag before creating a "
                "new one?"))) {
            continue;
        }
        auto view = OutputMenu{};
        auto index = 0uz;
        constexpr auto messages = std::array{"Preparing release assets..."sv,
            "Signing annotated tag..."sv, "Verifying tag signature..."sv,
            "Pushing signed tag..."sv, "Uploading assets to draft..."sv,
            "Preview complete. No release was created."sv};
        auto next = std::chrono::steady_clock::now();
        const auto choice = view.Select(terminal,
            [&](auto &frame) {
                context.Draw(frame);
                frame.Write("Release publication — UI preview\n\n"sv);
            },
            [&](auto &output) {
                if ((index < messages.size()) &&
                    (std::chrono::steady_clock::now() >= next)) {
                    output.AppendLine(messages[index++]);
                    next += 250ms;
                }
                if (index == messages.size()) {
                    output.SetCommands(
                        std::array{OutputCommand{1, "Try another tag"sv},
                            OutputCommand{2, "Finish preview"sv}});
                }
            });
        if (choice && (*choice == 2)) {
            return "UI preview finished; no release was created.";
        }
    }
    return "UI preview cancelled; no release was created.";
}

}
