#pragma once

#include "maic/service.hpp"
#include "maic/settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace maic {

// The `:` commands: one registry for running, completing and `:help`.
struct CommandInfo {
    std::string name;
    std::vector<std::string> aliases;
    std::string args;     // shown after the name, e.g. "[now]"
    std::string summary;  // one line, for the palette
    std::string help;     // the `:h name` text (markdown)
};

const std::vector<CommandInfo>& commands();

// Commands whose name or alias starts with `word`; an exact match comes first.
std::vector<const CommandInfo*> match_commands(const std::string& word);

// Argument completions for a command: mode names, settings keys, help topics, service or provider names.
struct CompletionContext {
    std::vector<std::string> services;
    std::vector<std::string> providers;
    std::vector<std::string> models;  // installed models, as they would be typed ("qwen3.5:4b", "lab/gemma")
    std::vector<std::string> themes;       // theme names, for :theme
    std::vector<std::string> nvim_colors;  // nvim's colorschemes, for :theme nvim:
};
std::vector<std::string> complete_argument(const std::string& command, const std::string& partial, const CompletionContext& ctx);

// `:help`. An empty topic is the index. Topics are commands, keys ("u", "Ctrl-W", "Alt+Enter") and concepts
// ("modes", "harness", "sessions"). Prefix matching like vim; several matches give a list.
std::string help_text(const std::string& topic);

// One page of `:h`: its name as shown (":w" for a command), one-line summary and Markdown text.
struct HelpPage {
    std::string name;
    std::string summary;
    std::string text;
};
// The page `topic` names; none for the index, an unknown topic or several matches.
std::optional<HelpPage> help_page(const std::string& topic);
// The tags of a page's first line: `*artifact* *maic artifact*` gives "artifact" and "maic artifact".
std::vector<std::string> help_tags(const HelpPage& page);
// Markdown for a terminal, as `maic help` prints it: the tag line, `code` and **bold** styled by the theme of `paint`
// (none: plain), the markers dropped either way. The wording and line breaks are the text's.
std::string render_markdown(const std::string& text, const Settings* paint);
// A page laid out like a man page: NAME, SYNOPSIS (`usage`, when the page's command has one), DESCRIPTION (the text,
// rendered), FILES and SEE ALSO (the paths, docs and topics the text mentions).
std::string render_help(const HelpPage& page, const std::string& usage, const Settings* paint);

// What `maic open NAME` / `:open NAME` should run: a service opens its URL in the chosen browser (the remote
// maic-server's copy when `remote` is set and answers), anything else opens the place's path with xdg-open.
// With `folder`, the containing directory is opened in the file manager instead: a file place's parent, a
// service's vendored checkout (vendor/NAME) when it has one.
// Returns {command, description}. Throws when NAME is neither a service nor a place.
std::pair<std::string, std::string> open_command(const std::string& name, const Settings& settings, const std::filesystem::path& workspace,
                                                 const std::vector<ServiceDef>& services, const std::optional<std::filesystem::path>& session,
                                                 const std::string& browser_override = "", bool folder = false);

}  // namespace maic
