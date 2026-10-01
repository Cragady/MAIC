#pragma once

#include "maic/service.hpp"
#include "maic/settings.hpp"

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace maic {

class Agent;
struct Provider;

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
};
std::vector<std::string> complete_argument(const std::string& command, const std::string& partial, const CompletionContext& ctx);

// `:help`. An empty topic is the index. Topics are commands, keys ("u", "Ctrl-W", "Alt+Enter") and concepts
// ("modes", "harness", "sessions"). Prefix matching like vim; several matches give a list.
std::string help_text(const std::string& topic);

// The error a failed turn shows. A transport failure to a local provider adds what to do about the service
// behind it (start it, link a model), from the service state on that port.
std::string failure_text(const Agent& agent, const std::exception& e);

// The context window of a llama server ("llamacpp" or "llamacpp-2"): exports ${MAIC_CONTEXT} or ${MAIC_CONTEXT_2}
// for its service file and sizes the matching provider's readout.
void set_context(std::vector<Provider>& providers, int tokens, const std::string& service = "llamacpp");

// When `query` names a preset: sets settings.model, the reviewer, thinking, the provider's context_window and,
// for a local llama.cpp model, settings.context. Returns the preset's name, "" when none matched.
std::string apply_preset(Settings& settings, const std::string& query);

// What `maic open NAME` / `:open NAME` should run: a service opens its URL in the chosen browser (the remote
// maic-server's copy when `remote` is set and answers), anything else opens the place's path with xdg-open.
// With `folder`, the containing directory is opened in the file manager instead: a file place's parent, a
// service's vendored checkout (vendor/NAME) when it has one.
// Returns {command, description}. Throws when NAME is neither a service nor a place.
std::pair<std::string, std::string> open_command(const std::string& name, const Settings& settings, const std::filesystem::path& workspace,
                                                 const std::vector<ServiceDef>& services, const std::optional<std::filesystem::path>& session,
                                                 const std::string& browser_override = "", bool folder = false);
// When the llama server `service` is running with another command than its file now gives (a new context
// size), restarts it. Returns a notice, "" when nothing had to happen.
std::string restart_llamacpp_if_changed(const std::string& service = "llamacpp");

}  // namespace maic
