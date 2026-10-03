// Other tools' transcripts as MAID sessions. Two sources: a claude.ai export (one JSON document, a
// conversation or a list of them, messages threaded by parent uuid) and a Claude Code transcript (JSONL,
// one record per content block, tool results in later user records). Both become the same records the
// agent writes itself, so the result resumes and lists like any session.
#include "maid/import.hpp"

#include "maid/llm.hpp"
#include "maid/session.hpp"

#include <unistd.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>

namespace maid {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Lenient field access: exports are not under our control, so a missing or oddly typed field is "" or empty.
std::string str(const json& j, const char* key) {
    auto it = j.is_object() ? j.find(key) : j.end();
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

const json& arr(const json& j, const char* key) {
    static const json empty = json::array();
    auto it = j.is_object() ? j.find(key) : j.end();
    return it != j.end() && it->is_array() ? *it : empty;
}

bool flag(const json& j, const char* key) {
    auto it = j.is_object() ? j.find(key) : j.end();
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

void append_text(std::string& to, const std::string& part) {
    if (part.empty()) return;
    if (!to.empty()) to += "\n\n";
    to += part;
}

// Appends the records for one turn. A tool result becomes a tool message and the tool display record,
// named after the call it answers.
struct Builder {
    ImportedSession& out;
    int generated = 0;
    std::map<std::string, ToolCall> calls;

    void user(const std::string& text) {
        out.records.push_back({"msg", message_to_json({"user", text})});
        out.records.push_back({"user", {{"text", text}}});
        ++out.messages;
    }
    void assistant(const std::string& text, std::vector<ToolCall> tool_calls) {
        if (text.empty() && tool_calls.empty()) return;
        for (const auto& c : tool_calls) calls[c.id] = c;
        Message m{"assistant", text, std::move(tool_calls)};
        out.records.push_back({"msg", message_to_json(m)});
        if (!text.empty()) out.records.push_back({"assistant", {{"text", text}}});
        ++out.messages;
    }
    void tool(const std::string& id, const std::string& name_hint, const std::string& result, bool ok) {
        auto it = calls.find(id);
        ToolCall call = it != calls.end() ? it->second : ToolCall{id, name_hint, json::object()};
        Message m{"tool", result, {}, call.name, id, !ok};
        out.records.push_back({"msg", message_to_json(m)});
        out.records.push_back({"tool", {{"tool", call.name}, {"arguments", call.arguments}, {"result", result}, {"ok", ok}}});
        ++out.messages;
    }
    ToolCall call(const json& block) {
        std::string id = str(block, "id");
        if (id.empty()) id = "import_" + std::to_string(++generated);
        auto it = block.find("input");
        json input = it != block.end() && it->is_object() ? *it : json::object();
        return {id, str(block, "name"), input};
    }
    // A tool_result's content is a string or a list of blocks; images and other binary blocks are named, not copied.
    std::string result_text(const json& content) {
        if (content.is_string()) return content.get<std::string>();
        std::string text;
        for (const auto& b : content.is_array() ? content : json::array()) {
            if (!text.empty()) text += "\n";
            if (str(b, "type") == "text") text += str(b, "text");
            else {
                text += "[" + (str(b, "type").empty() ? std::string("block") : str(b, "type")) + " omitted]";
                ++out.skipped;
            }
        }
        return text;
    }
};

// claude.ai messages form a tree through parent_message_uuid (a regenerated answer is a sibling); the
// longest branch from the root is the conversation. Exports without the threading fields are taken in order.
std::vector<const json*> conversation_chain(const json& msgs) {
    std::map<std::string, const json*> by;
    std::map<std::string, std::vector<std::string>> kids;
    std::vector<const json*> chain;
    bool threaded = !msgs.empty();
    for (const auto& m : msgs) {
        if (!m.is_object()) continue;
        chain.push_back(&m);
        if (str(m, "uuid").empty() || !m.contains("parent_message_uuid")) threaded = false;
        by[str(m, "uuid")] = &m;
        kids[str(m, "parent_message_uuid")].push_back(str(m, "uuid"));
    }
    if (!threaded) return chain;
    std::map<std::string, size_t> depth;
    std::function<size_t(const std::string&)> deepest = [&](const std::string& u) -> size_t {
        auto it = depth.find(u);
        if (it != depth.end()) return it->second;
        depth[u] = 0;  // a cycle ends here
        size_t best = 0;
        for (const auto& c : kids[u]) best = std::max(best, deepest(c));
        return depth[u] = 1 + best;
    };
    std::string cur;
    for (const json* m : chain) {
        if (!by.count(str(*m, "parent_message_uuid"))) {
            cur = str(*m, "uuid");
            break;
        }
    }
    std::vector<const json*> out;
    while (!cur.empty() && out.size() <= chain.size()) {
        out.push_back(by[cur]);
        std::string next;
        for (const auto& c : kids[cur]) {
            if (next.empty() || deepest(c) > deepest(next)) next = c;
        }
        cur = next;
    }
    return out;
}

void read_claude_ai(const json& conv, ImportedSession& out) {
    out.format = "claude-ai";
    out.title = str(conv, "name");
    out.workspace = fs::current_path().string();
    Builder b{out};
    for (const json* mp : conversation_chain(arr(conv, "chat_messages"))) {
        const json& m = *mp;
        if (str(m, "sender") == "human") {
            std::string text;
            for (const auto& block : arr(m, "content")) {
                if (str(block, "type") == "text") append_text(text, str(block, "text"));
            }
            if (text.empty()) text = str(m, "text");
            for (const auto& a : arr(m, "attachments")) {
                std::string name = str(a, "file_name");
                append_text(text, "[pasted file" + (name.empty() ? "" : ": " + name) + "]\n\n" + str(a, "extracted_content"));
            }
            if (arr(m, "files").size() > arr(m, "attachments").size()) {
                size_t extra = arr(m, "files").size() - arr(m, "attachments").size();
                append_text(text, "[" + std::to_string(extra) + " attached file(s) had no extractable text]");
                out.skipped += extra;
            }
            b.user(text);
        } else if (str(m, "sender") == "assistant") {
            // Results follow their calls inside the same message; the assistant turn is flushed before each.
            std::string text;
            std::vector<ToolCall> calls;
            std::deque<std::string> pending;
            for (const auto& block : arr(m, "content")) {
                std::string type = str(block, "type");
                if (type == "text") append_text(text, str(block, "text"));
                else if (type == "tool_use") {
                    calls.push_back(b.call(block));
                    pending.push_back(calls.back().id);
                } else if (type == "tool_result") {
                    b.assistant(text, calls);
                    text.clear();
                    calls.clear();
                    std::string id = str(block, "tool_use_id");
                    if (id.empty() && !pending.empty()) id = pending.front();
                    if (!pending.empty() && pending.front() == id) pending.pop_front();
                    b.tool(id, str(block, "name"), b.result_text(block.value("content", json())), !flag(block, "is_error"));
                } else ++out.skipped;  // thinking and anything newer
            }
            b.assistant(text, calls);
        } else ++out.skipped;
    }
}

void read_claude_code(const fs::path& source, ImportedSession& out) {
    out.format = "claude-code";
    std::ifstream in(source);
    if (!in) throw std::runtime_error("can't read " + source.string());
    Builder b{out};
    std::string text;  // the assistant turn being assembled from consecutive records
    std::vector<ToolCall> calls;
    bool in_assistant = false;
    auto flush = [&] {
        if (in_assistant) b.assistant(text, calls);
        text.clear();
        calls.clear();
        in_assistant = false;
    };
    int title_rank = 0;  // custom-title beats ai-title beats summary
    auto title = [&](int rank, const std::string& t) {
        if (rank > title_rank && !t.empty()) out.title = t, title_rank = rank;
    };
    for (std::string line; std::getline(in, line);) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        auto j = json::parse(line, nullptr, false);
        if (!j.is_object()) {
            ++out.malformed;
            continue;
        }
        std::string type = str(j, "type");
        if (out.workspace.empty()) out.workspace = str(j, "cwd");
        if (flag(j, "isSidechain")) {
            ++out.skipped;  // a subagent's transcript, interleaved
            continue;
        }
        const json& message = j.contains("message") && j["message"].is_object() ? j["message"] : json::object();
        const json& content = message.contains("content") ? message["content"] : json();
        if (type == "assistant") {
            in_assistant = true;
            if (out.model.empty()) out.model = str(message, "model");
            if (content.is_string()) append_text(text, content.get<std::string>());
            for (const auto& block : content.is_array() ? content : json::array()) {
                std::string bt = str(block, "type");
                if (bt == "text") append_text(text, str(block, "text"));
                else if (bt == "tool_use") calls.push_back(b.call(block));
                else ++out.skipped;
            }
        } else if (type == "user") {
            if (flag(j, "isMeta")) {
                ++out.skipped;  // slash-command echoes and other notes the user did not type
                continue;
            }
            flush();
            if (content.is_string()) {
                b.user(content.get<std::string>());
                continue;
            }
            std::string user_text;
            for (const auto& block : content.is_array() ? content : json::array()) {
                std::string bt = str(block, "type");
                if (bt == "text") append_text(user_text, str(block, "text"));
                else if (bt == "tool_result") b.tool(str(block, "tool_use_id"), "", b.result_text(block.value("content", json())), !flag(block, "is_error"));
                else ++out.skipped;
            }
            if (!user_text.empty()) b.user(user_text);
        } else if (type == "custom-title") title(3, str(j, "customTitle"));
        else if (type == "ai-title") title(2, str(j, "aiTitle"));
        else if (type == "summary") title(1, str(j, "summary"));
        else ++out.skipped;
    }
    flush();
    if (out.workspace.empty()) out.workspace = fs::current_path().string();
}

}  // namespace

std::string detect_import_format(const fs::path& source) {
    std::ifstream in(source);
    if (!in) throw std::runtime_error("can't read " + source.string());
    for (std::string line; std::getline(in, line);) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        if (line[start] == '[') return "claude-ai";
        auto j = json::parse(line, nullptr, false);
        // One record per line with a type is a transcript; a whole conversation on one line, or a
        // pretty-printed one whose first line does not parse alone, is an export.
        if (j.is_object() && j.contains("type") && !j.contains("chat_messages")) return "claude-code";
        return "claude-ai";
    }
    throw std::runtime_error(source.string() + " is empty");
}

ImportedSession read_import(const fs::path& source, const std::string& format, const std::string& conversation) {
    std::string kind = format.empty() || format == "auto" ? detect_import_format(source) : format;
    ImportedSession out;
    if (kind == "claude-code") {
        read_claude_code(source, out);
        return out;
    }
    if (kind != "claude-ai") throw std::runtime_error("unknown import format '" + format + "' (claude-ai, claude-code, auto)");
    std::ifstream in(source);
    if (!in) throw std::runtime_error("can't read " + source.string());
    json doc = json::parse(in, nullptr, false);
    if (doc.is_object()) doc = json::array({doc});
    if (!doc.is_array() || doc.empty()) throw std::runtime_error(source.string() + " is not a claude.ai export (a conversation or a list of them)");
    const json* pick = nullptr;
    if (!conversation.empty()) {
        for (const auto& c : doc) {
            if (str(c, "uuid") == conversation) pick = &c;
        }
        if (!pick) throw std::runtime_error("no conversation with uuid " + conversation + " in " + source.string());
    } else if (doc.size() == 1) {
        pick = &doc[0];
    } else {
        std::string list;
        for (const auto& c : doc) list += "\n  " + str(c, "uuid") + "  " + str(c, "name");
        throw std::runtime_error(source.string() + " holds " + std::to_string(doc.size()) + " conversations; choose one with --conversation UUID:" + list);
    }
    read_claude_ai(*pick, out);
    return out;
}

fs::path write_import(const ImportedSession& session, const fs::path& source, const fs::path& home) {
    SessionLog log("import", home);
    char host[256] = "";
    gethostname(host, sizeof(host) - 1);
    log.write("start", {{"workspace", session.workspace}, {"model", ""}, {"mode", "manual"}, {"host", host}, {"pid", getpid()}});
    log.write("imported_from", {{"path", fs::weakly_canonical(source).string()}, {"format", session.format}, {"model", session.model},
                                {"messages", session.messages}, {"skipped", session.skipped}, {"malformed", session.malformed}});
    if (!session.title.empty()) log.write("title", {{"text", session.title}});
    for (const auto& [type, data] : session.records) log.write(type, data);
    return log.path();
}

}  // namespace maid
