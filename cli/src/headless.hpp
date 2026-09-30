#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace maic {

struct HeadlessOptions {
    std::string prompt;  // "-" or empty reads stdin
    std::optional<std::string> model;
    std::optional<std::string> mode;
    bool json = false;   // JSONL events on stdout instead of text
    bool think = false;
    std::optional<std::filesystem::path> resume;
    std::vector<std::filesystem::path> context;  // files attached to the turn before the prompt ("-" = stdin)
    std::optional<std::string> system;
    std::optional<std::string> prefill;
    std::optional<int> ctx;
    std::vector<std::string> rules;
    std::optional<bool> load_instructions;
    std::vector<std::string> bans;
    std::vector<std::string> ban_patterns;
    nlohmann::json sampling = nlohmann::json::object();
    std::optional<std::string> harness;
    bool accept_dumb_auto = false;
    bool record = false;  // write a transcript at all (a one-shot -p leaves nothing behind by default)
    bool append = false;  // with resume: write into the old file (implies record) rather than a new one that points at it
    std::optional<size_t> fork_at;  // with resume: fork from its first N records (append is then false)
};

// `maic -p "..."`: one turn, no UI. The reply streams to stdout, tool activity goes to stderr.
// Approvals are asked on the terminal when stdin is one, otherwise denied.
int run_headless(const HeadlessOptions& options);

}  // namespace maic
