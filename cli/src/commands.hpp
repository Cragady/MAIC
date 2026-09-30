#pragma once

#include <exception>
#include <string>
#include <vector>

namespace maic {

class Agent;

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

}  // namespace maic
