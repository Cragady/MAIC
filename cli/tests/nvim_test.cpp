// maic.nvim from MAIC's side, against a real headless nvim (`--listen` on a socket of the test's own, the plugin
// on its runtimepath): the ancestry check, maic_send and maic_command, :e and the diff in the host, the theme
// following ColorScheme, User autocmds, the diagnostics tool judged as a read, maic.nvim in the Lua tools and in
// the user's Lua; then the plugin's own Lua tests (maic.nvim/tests/maic_test.lua). Skipped (77) without nvim.
#include "check.hpp"
#include "nvim_host.hpp"

#include "maic/agent.hpp"
#include "maic/http.hpp"
#include "maic/lua.hpp"
#include "maic/lua_tools.hpp"
#include "maic/nvim_setup.hpp"
#include "maic/theme.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace maic;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool on_path(const std::string& program) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "";
    for (size_t start = 0; start <= dirs.size();) {
        size_t colon = dirs.find(':', start);
        std::string dir = dirs.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty() && access((fs::path(dir) / program).c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

pid_t spawn(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    return pid;
}

int run(const std::vector<std::string>& argv) {
    std::cout.flush();
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

// A command's exit code and its stdout and stderr together (the arguments hold no spaces or quotes).
int capture(const std::vector<std::string>& argv, std::string& out) {
    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
    FILE* p = popen((line + " 2>&1").c_str(), "r");
    out.clear();
    char buf[4096];
    for (size_t n; p && (n = fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
    int status = p ? pclose(p) : -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <class F>
bool eventually(F f, int ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

// A listening Unix socket owned by this process, for the ancestry check's own cases.
int listen_at(const fs::path& path) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    listen(s, 4);
    return s;
}

// What connect_host_socket says in a child process of this one: this process is then its ancestor.
std::string verdict_in_child(const fs::path& path) {
    int fds[2];
    if (pipe(fds) != 0) return "pipe failed";
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        int fd = -1;
        std::string why = connect_host_socket(path, fd);
        [[maybe_unused]] ssize_t w = write(fds[1], why.data(), why.size());
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buf[512];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    waitpid(pid, nullptr, 0);
    return out;
}

// The model for the agent check: first a `diagnostics` call with `args`, then a plain reply.
struct FakeModel {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::mutex mu;
    std::vector<json> requests;
    json args;

    static std::string event(const json& j) { return "data: " + j.dump() + "\n\n"; }

    FakeModel() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            bool first;
            {
                std::lock_guard lock(mu);
                requests.push_back(body);
                first = requests.size() % 2 == 1;
            }
            std::string out;
            if (first) {
                json tc = {{"index", 0}, {"id", "call_1"}, {"type", "function"}, {"function", {{"name", "diagnostics"}, {"arguments", args.dump()}}}};
                out += event({{"choices", {{{"index", 0}, {"delta", {{"content", ""}, {"tool_calls", {tc}}}}}}}});
                out += event({{"choices", {{{"index", 0}, {"delta", json::object()}, {"finish_reason", "tool_calls"}}}}});
            } else {
                out += event({{"choices", {{{"index", 0}, {"delta", {{"content", "done"}}}}}}});
                out += event({{"choices", {{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}}}});
            }
            out += "data: [DONE]\n\n";
            res.set_content(out, "text/event-stream");
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeModel() {
        srv.stop();
        thread.join();
    }
    Provider provider() const { return {"fake", "openai", "http://127.0.0.1:" + std::to_string(port) + "/v1", "", "", {{"context_window", 16384}}}; }
};

// A model whose reply never ends: one chunk, then keepalives until the agent hangs up (or 10 s).
struct HeldModel {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::atomic<bool> streaming{false};

    HeldModel() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider("text/event-stream", [this](size_t, httplib::DataSink& sink) {
                std::string first = FakeModel::event({{"choices", {{{"index", 0}, {"delta", {{"content", "thinking"}}}}}}});
                if (!sink.write(first.data(), first.size())) return false;
                streaming = true;
                std::string keepalive = FakeModel::event({{"choices", {{{"index", 0}, {"delta", json::object()}}}}});
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (!sink.write(keepalive.data(), keepalive.size())) return false;
                }
                sink.done();
                return true;
            });
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~HeldModel() {
        srv.stop();
        thread.join();
    }
    Provider provider() const { return {"fake", "openai", "http://127.0.0.1:" + std::to_string(port) + "/v1", "", "", {{"context_window", 16384}}}; }
};

struct Recorder : AgentEvents {
    std::vector<std::string> results;
    std::vector<ApprovalRequest> asked;
    std::vector<std::string> notices;
    void on_text(std::string_view, bool) override {}
    void on_tool_call(const std::string&) override {}
    void on_tool_result(const std::string& t, bool) override { results.push_back(t); }
    void on_notice(const std::string& n) override { notices.push_back(n); }
    ApprovalAnswer ask(const ApprovalRequest& r) override {
        asked.push_back(r);
        return {Approval::No, ""};
    }
};

bool offers(const json& body, const std::string& name) {
    for (const auto& t : body.value("tools", json::array())) {
        if (t["function"]["name"] == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    if (!on_path("nvim")) {
        std::cout << "nvim is not on PATH: the maic.nvim tests are skipped\n";
        return 77;
    }
    fs::path tmp = fs::temp_directory_path() / ("maic-nvim-test-" + std::to_string(getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "ws");
    fs::path ws = fs::canonical(tmp / "ws");
    setenv("XDG_STATE_HOME", (tmp / "state").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), 1);
    setenv("MAIC_TRIPWIRE_FILE", (tmp / "tripwire.none").c_str(), 1);
    fs::create_directories(tmp / "run");
    fs::permissions(tmp / "run", fs::perms::owner_all);
    setenv("XDG_RUNTIME_DIR", (tmp / "run").c_str(), 1);
    unsetenv("MAIC_TESTING");
    unsetenv("MAIC_NVIM_TRUST_SOCKET");

    fs::path sock = tmp / "nvim.sock";
    pid_t nvim = spawn({"nvim", "--headless", "-u", "NONE", "-i", "NONE", "-n", "--listen", sock.string(), "--cmd", "set rtp+=" MAIC_PLUGIN_DIR});
    if (!eventually([&] { return fs::exists(sock); }, 10000)) {
        std::cout << "nvim did not open its socket\n";
        kill(-nvim, SIGKILL);
        return 1;
    }

    section("the ancestry check");
    {
        int fd = -1;
        std::string why = connect_host_socket(sock, fd);
        expect(fd < 0 && why.find("not one of this MAIC's parent processes") != std::string::npos, "a headless nvim that is not an ancestor is refused: " + why);
        setenv("MAIC_NVIM_TRUST_SOCKET", "1", 1);
        why = connect_host_socket(sock, fd);
        expect(fd < 0 && !why.empty(), "MAIC_NVIM_TRUST_SOCKET alone is ignored (it needs MAIC_TESTING=1)");
        unsetenv("MAIC_NVIM_TRUST_SOCKET");
        std::ofstream(tmp / "plain") << "x";
        expect(connect_host_socket(tmp / "plain", fd) == "not a socket", "a file that is not a socket is refused");
        expect(!connect_host_socket("127.0.0.1:6666", fd).empty(), "a TCP address is refused");

        int own = listen_at(tmp / "ancestor.sock");
        std::string child = verdict_in_child(tmp / "ancestor.sock");
        expect(child.empty(), "a socket whose listener is an ancestor is accepted" + (child.empty() ? "" : ": " + child));
        close(own);
        int wrong = listen_at(tmp / "nvim.1.0");
        child = verdict_in_child(tmp / "nvim.1.0");
        expect(child.find("its name says nvim 1") != std::string::npos, "a default socket name naming another pid is refused: " + child);
        close(wrong);

        std::string refused;
        setenv("NVIM", sock.c_str(), 1);
        expect(!HostNvim::from_env(refused) && !refused.empty(), "HostNvim::from_env refuses it too and says why");
        unsetenv("NVIM");
        expect(!HostNvim::from_env(refused) && refused.empty(), "no $NVIM: no host and nothing to report");
    }

    setenv("MAIC_TESTING", "1", 1);
    setenv("MAIC_NVIM_TRUST_SOCKET", "1", 1);
    setenv("NVIM", sock.c_str(), 1);
    std::string why;
    std::shared_ptr<HostNvim> host = HostNvim::from_env(why);
    expect(host && host->connected(), "with MAIC_TESTING=1 and MAIC_NVIM_TRUST_SOCKET=1 MAIC connects" + (why.empty() ? "" : ": " + why));
    // A second client to look at the host with, renamed so it is not taken for MAIC.
    std::shared_ptr<HostNvim> other = HostNvim::connect(sock, why);
    if (!host || !other) {
        kill(-nvim, SIGKILL);
        return finish();
    }
    other->request("nvim_set_client_info", {msgpack::Value::str("tester"), to_msgpack(json::object()), msgpack::Value::str("remote"), to_msgpack(json::object()), to_msgpack(json::object())});

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> sent, commands;
    int colorschemes = 0;
    HostNvim::Handlers h;
    h.send = [&](const std::string& t) {
        std::lock_guard lock(mu);
        sent.push_back(t);
        cv.notify_all();
    };
    h.command = [&](const std::string& c) {
        std::lock_guard lock(mu);
        commands.push_back(c);
        cv.notify_all();
    };
    h.colorscheme = [&] {
        std::lock_guard lock(mu);
        ++colorschemes;
        cv.notify_all();
    };
    host->set_handlers(h);
    auto wait_for = [&](auto pred) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(5), pred);
    };

    section("maic_send and maic_command from the plugin");
    {
        json how = other->exec_lua("return require('maic').send_text(...)", json::array({"hello from nvim"}));
        expect(how == "rpc", "the plugin finds MAIC's channel by its client name");
        expect(wait_for([&] { return !sent.empty(); }) && sent[0] == "hello from nvim", "maic_send arrives as text for the input");
        other->exec_lua("require('maic').command(...)", json::array({":theme mono"}));
        expect(wait_for([&] { return !commands.empty(); }) && commands[0] == ":theme mono", "maic_command arrives as a command line");
    }

    section(":MaicInterrupt cancels a running turn");
    {
        HeldModel model;
        Agent agent(ws, "fake/m");
        agent.providers = {model.provider()};
        std::atomic<bool> cancel{false};
        h.interrupt = [&] { cancel = true; };  // the TUI's handler does the first Ctrl-C; this one sets the same flag
        host->set_handlers(h);
        Recorder r;
        std::thread turn([&] { agent.submit("a long one", Origin::Local, r, cancel); });
        expect(eventually([&] { return model.streaming.load(); }), "the fake turn is running");
        auto t0 = std::chrono::steady_clock::now();
        other->exec_lua("vim.cmd('runtime plugin/maic.lua') vim.cmd('MaicInterrupt')", json::array());
        turn.join();
        bool noticed = false;
        for (const auto& n : r.notices) noticed = noticed || n.find("interrupted") != std::string::npos;
        expect(cancel.load() && noticed && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5),
               "rpcnotify(chan, \"maic_interrupt\") from :MaicInterrupt cancels the turn at once");
        expect(other->exec_lua("return require('maic').interrupt()", json::array()) == "rpc", "require('maic').interrupt() goes over MAIC's channel when it is connected");
        h.interrupt = nullptr;
        host->set_handlers(h);
    }

    section(":e and the diff open in the host");
    fs::path file = ws / "src.c";
    std::ofstream(file) << "int main(void) {\n    return 0\n}\n";
    {
        host_open(*host, file);
        json name = other->exec_lua("return vim.uv.fs_realpath(vim.api.nvim_buf_get_name(0))", json::array());
        expect(name == file.string(), ":e FILE drops the file into the host's window: " + name.dump());
        host_diff(*host, file, "int main(void) {\n    return 0;\n}\n");
        json state = other->exec_lua("local b = vim.api.nvim_get_current_buf() return { tabs = vim.fn.tabpagenr('$'), diff = vim.wo.diff, ro = vim.bo[b].readonly, "
                                     "line = vim.api.nvim_buf_get_lines(b, 1, 2, false)[1], wins = #vim.api.nvim_tabpage_list_wins(0) }",
                                     json::array());
        expect(state["tabs"] == 2 && state["wins"] == 2 && state["diff"] == true && state["ro"] == true && state["line"] == "    return 0;",
               "the diff is a new tab: the file against a read-only scratch buffer with the proposed content, both in diff mode: " + state.dump());
        other->exec_lua("vim.cmd('tabclose')", json::array());
    }

    section("the theme follows ColorScheme");
    {
        host_watch_colorscheme(*host, host->channel());
        Settings before, after;
        Theme first = host_theme(*host);
        apply_theme(before, first);
        other->exec_lua("vim.cmd.colorscheme('habamax')", json::array());
        expect(wait_for([&] { return colorschemes > 0; }), "the host's ColorScheme reaches MAIC as maic_colorscheme");
        Theme second = host_theme(*host);
        apply_theme(after, second);
        expect(second.name == "nvim:habamax", "the theme is named nvim:<colors_name>: " + second.name);
        expect(before.style("error").fg != after.style("error").fg, "a role's colour changes with the colorscheme (error: " + before.style("error").fg.value_or("none") + " -> " +
                                                                        after.style("error").fg.value_or("none") + ")");
    }

    section("User autocmds fire in the host");
    {
        other->exec_lua("vim.api.nvim_create_autocmd('User', { pattern = 'MaicToolCall', callback = function(ev) vim.g.maic_seen = ev.data.tool .. ' ' .. ev.data.path .. ' ' .. ev.data.session end })",
                        json::array());
        host_fire(*host, "MaicToolCall", {{"tool", "read_file"}, {"path", "/x"}, {"session", "s1"}});
        expect(eventually([&] { return other->exec_lua("return vim.g.maic_seen", json::array()) == "read_file /x s1"; }), "MaicToolCall runs the user's autocmd with its data");
    }

    section("diagnostics: the tool, the Lua tools, the user's Lua");
    {
        other->exec_lua("local path = ... vim.cmd.edit(vim.fn.fnameescape(path)) local ns = vim.api.nvim_create_namespace('maic_test') "
                        "vim.diagnostic.set(ns, vim.api.nvim_get_current_buf(), { { lnum = 1, col = 12, severity = vim.diagnostic.severity.ERROR, message = 'expected ;', source = 'clangd' } })",
                        json::array({file.string()}));
        json d = host_diagnostics(*host, file);
        expect(d.size() == 1 && d[0]["line"] == 2 && d[0]["col"] == 13 && d[0]["severity"] == "error", "host_diagnostics reads vim.diagnostic: " + d.dump());
        expect(format_diagnostics(d, ws, false) == "src.c:2:13: error: expected ; [clangd]\n", "formatted as path:line:col: severity: message");
        expect(format_diagnostics(host_diagnostics(*host, ""), ws, true) == "src.c:2:13: error: expected ; [clangd]\n", "the workspace-wide call finds it too");

        {
            FakeModel model;
            model.args = {{"path", "src.c"}};
            Agent agent(ws, "fake/m");
            agent.providers = {model.provider()};
            agent.mode = Mode::Plan;
            Recorder r;
            std::atomic<bool> no{false};
            agent.submit("check it", Origin::Local, r, no);
            expect(!model.requests.empty() && !offers(model.requests[0], "diagnostics"), "without a host there is no diagnostics tool");

            agent.set_nvim_host(host);
            r = Recorder{};
            agent.submit("check it", Origin::Local, r, no);
            expect(offers(model.requests[2], "diagnostics"), "with a host the model is offered diagnostics");
            expect(r.asked.empty() && !r.results.empty() && r.results[0].find("src.c:2:13: error: expected ;") != std::string::npos,
                   "a read in the workspace runs in plan mode without asking and returns the line: " + (r.results.empty() ? "" : r.results[0]));
            // The built-in plan and explore agents list their tools, and diagnostics is on both lists.
            for (const char* name : {"plan", "explore"}) {
                FakeModel sub_model;
                sub_model.args = {{"path", "src.c"}};
                Agent sub(ws, "fake/m");
                sub.providers = {sub_model.provider()};
                sub.set_agent_def(*find_agent_def(default_agent_defs(), name));
                sub.set_nvim_host(host);
                Recorder sr;
                sub.submit("check it", Origin::Local, sr, no);
                expect(!sub_model.requests.empty() && offers(sub_model.requests[0], "diagnostics") && sr.asked.empty() && !sr.results.empty() &&
                           sr.results[0].find("src.c:2:13: error: expected ;") != std::string::npos,
                       std::string("the ") + name + " agent is offered diagnostics while a host is connected and runs it as a read");
            }

            model.args = {{"path", "/etc/hostname"}};
            r = Recorder{};
            agent.submit("and that", Origin::Local, r, no);
            expect(r.asked.size() == 1 && r.asked[0].tool == "diagnostics" && r.asked[0].reason == "reads outside the workspace" && r.asked[0].path == "/etc/hostname",
                   "outside the workspace it is asked about as a read");
            expect(!r.results.empty() && r.results[0].rfind("DENIED", 0) == 0, "and a no is a denial");

            model.args = {{"path", "~/.ssh/id_ed25519"}};
            r = Recorder{};
            agent.submit("and keys", Origin::Local, r, no);
            expect(r.asked.empty() && !r.results.empty() && r.results[0].find("never read") != std::string::npos, "a key is never read, through nvim or not");
        }

        LuaTool tool{"diag", "test", json::object(), ws / "diag.lua",
                     "return { name = 'diag', description = 'x', run = function() "
                     "local d = maic.nvim.diagnostics('src.c') local b = maic.nvim.buffers() "
                     "return d[1].message .. '|' .. #b .. '|' .. tostring(maic.nvim.exec) end }"};
        std::vector<Action> seen;
        Authorise gate = [&](const Action& a, const std::string&, const std::string&) {
            seen.push_back(a);
            return Decision{Verdict::Allow, ""};
        };
        Harness harness(ws);
        std::atomic<bool> no{false};
        ToolResult tr = run_lua_tool(tool, json::object(), harness, gate, no, std::chrono::seconds(10), host.get());
        expect(tr.ok && tr.text == "expected ;|1|nil", "a Lua tool reads diagnostics and buffers, and has no exec: " + tr.text);
        expect(seen.size() == 2 && seen[0].kind == Action::Kind::Read && seen[0].path == file && seen[0].tool == "diagnostics" && seen[1].kind == Action::Kind::Read && seen[1].path == ws,
               "each is authorised as a read (the file, then the workspace)");
        tr = run_lua_tool(tool, json::object(), harness, gate, no);
        expect(!tr.ok, "without a host a Lua tool has no maic.nvim");

        set_lua_nvim_host(host);
        Lua lua(ws);
        Lua::Result lr = lua.run("return maic.nvim.exec('return 1 + ...', 41), maic.nvim.diagnostics('src.c')[1].message, maic.nvim.current().path ~= nil, #maic.nvim.buffers()");
        expect(lr.ok && lr.output == "42\nexpected ;\ntrue\n1\n", "the user's Lua has maic.nvim.exec, diagnostics, current and buffers: " + lr.output);
        set_lua_nvim_host(nullptr);
        Lua plain(ws);
        expect(plain.run("return maic.nvim").output == "nil\n", "without a host there is no maic.nvim");
    }

    section("a real maic in an nvim terminal");
    {
        // nvim was started before the test variables were set, so this maic has only the real check: nvim is its parent.
        json pid = other->exec_lua("vim.cmd('tabnew') local job = vim.fn.jobstart({ ... }, { term = true }) vim.g.maic_job = job return vim.fn.jobpid(job)",
                                   json::array({MAIC_BINARY, "--no-record", "--no-instructions"}));
        auto client = [&] {
            json chans = other->exec_lua("local out = {} for _, c in ipairs(vim.api.nvim_list_chans()) do if c.client and c.client.name == 'maic' then "
                                         "out[#out + 1] = c.client.attributes.pid end end return out",
                                         json::array());
            for (const auto& p : chans) {
                if (p.is_string() && pid.is_number() && p.get<std::string>() == std::to_string(pid.get<long>())) return true;
            }
            return false;
        };
        expect(eventually(client, 15000), "maic started in a terminal of this nvim passes the ancestry check and connects as client \"maic\" with its pid");
        other->exec_lua("vim.fn.jobstop(vim.g.maic_job)", json::array());
        expect(eventually([&] { return !client(); }, 10000), "and disconnects when it exits");

        // --bare and MAIC_BARE=1: the same maic, the same host, no connection. Ready is the welcome on its screen.
        auto bare = [&](const json& argv, const json& env) {
            pid = other->exec_lua("local argv, env = ... vim.cmd('enew!') local job = vim.fn.jobstart(argv, { term = true, env = env }) vim.g.maic_job = job vim.g.maic_buf = vim.api.nvim_get_current_buf() "
                                  "return vim.fn.jobpid(job)",
                                  json::array({argv, env}));
            bool ready = eventually([&] {
                return other->exec_lua("return table.concat(vim.api.nvim_buf_get_lines(vim.g.maic_buf, 0, -1, false), '\\n'):find('harness armed', 1, true) ~= nil", json::array()) == true;
            }, 15000);
            bool connected = eventually(client, 1500);
            other->exec_lua("vim.fn.jobstop(vim.g.maic_job) vim.cmd('enew!')", json::array());
            return ready && !connected;
        };
        expect(bare(json::array({MAIC_BINARY, "--no-record", "--no-instructions", "--bare"}), json::object()), "maic --bare inside the host does not connect");
        expect(bare(json::array({MAIC_BINARY, "--no-record", "--no-instructions"}), json{{"MAIC_BARE", "1"}}), "nor does maic with MAIC_BARE=1");
    }

    section("Esc in MAIC's terminal");
    {
        // A user's global tnoremap <Esc> <C-\><C-n>: MAIC's terminal still gets Esc, nvim's own key leaves it, and
        // every other terminal keeps the user's mapping. Keys go in through nvim_input, the real input path.
        fs::path typed = tmp / "typed";
        auto mode = [&] { return other->exec_lua("return vim.api.nvim_get_mode().mode", json::array()); };
        other->exec_lua("local out = ... vim.keymap.set('t', '<Esc>', '<C-\\\\><C-n>') "
                        "require('maic').setup({ keymaps = false, open = 'split', cmd = { 'sh', '-c', 'stty raw -echo; exec cat > ' .. out } }) "
                        "vim.cmd('tabnew') vim.cmd('Maic')",
                        json::array({typed.string()}));
        expect(eventually([&] { return fs::exists(typed) && mode() == "t"; }), "MAIC's terminal starts in terminal mode");
        other->request("nvim_input", {msgpack::Value::str("ab<Esc>c")});
        bool reached = eventually([&] { return read_file(typed) == "ab\x1b" "c"; });
        expect(reached, "Esc reaches MAIC as Esc: " + json(read_file(typed)).dump());
        expect(mode() == "t", "and MAIC's terminal stays in terminal mode");
        other->request("nvim_input", {msgpack::Value::str("<C-\\><C-n>")});
        expect(eventually([&] { return mode() == "nt"; }), "<C-\\><C-n> (terminal_escape) leaves it");
        other->exec_lua("vim.cmd('tabnew') vim.g.plain_job = vim.fn.jobstart({ 'sh', '-c', 'sleep 30' }, { term = true }) vim.cmd('startinsert')", json::array());
        expect(eventually([&] { return mode() == "t"; }), "another terminal in terminal mode");
        other->request("nvim_input", {msgpack::Value::str("<Esc>")});
        expect(eventually([&] { return mode() == "nt"; }), "the user's global <Esc> still leaves terminal mode everywhere else");
        other->exec_lua("vim.fn.jobstop(vim.g.plain_job) for _, c in ipairs(vim.api.nvim_list_chans()) do if c.mode == 'terminal' then pcall(vim.fn.jobstop, c.id) end end "
                        "vim.cmd('silent! tabonly!') vim.keymap.del('t', '<Esc>')",
                        json::array());
    }

    section("the host going away");
    {
        bool closed = false;
        h.closed = [&] {
            std::lock_guard lock(mu);
            closed = true;
            cv.notify_all();
        };
        host->set_handlers(h);
        other->notify("nvim_command", {msgpack::Value::str("qa!")});
        expect(wait_for([&] { return closed; }) && !host->connected(), "the closed handler runs and the host reports itself gone");
        bool threw = false;
        try {
            host->exec_lua("return 1", json::array());
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a request after that fails instead of waiting");
        host->set_handlers({});
    }
    int status = 0;
    if (waitpid(nvim, &status, WNOHANG) == 0) {
        kill(-nvim, SIGKILL);
        waitpid(nvim, &status, 0);
    }

    section("maic nvim keymaps");
    {
        // The real binary runs the same check as :checkhealth maic in a headless nvim; -u points it at a config.
        fs::path cfg = tmp / "init.lua", clean = tmp / "clean.lua";
        std::string rtp = "vim.g.mapleader = ' '\nvim.opt.rtp:prepend('" MAIC_PLUGIN_DIR "')\n";
        std::ofstream(clean) << rtp;
        std::ofstream(cfg) << rtp << "vim.keymap.set('t', '<C-w>', '<C-\\\\><C-n><C-w>', { desc = 'window from a terminal' })\n"
                                     "vim.g.llama_config = { endpoint_fim = 'http://127.0.0.1:8084/infill' }\n";
        std::string out;
        int rc = capture({MAIC_BINARY, "nvim", "keymaps", "-u", cfg.string()}, out);
        expect(rc == 1 && out.find("MAIC never gets <C-w>") != std::string::npos && out.find("fix: terminal_passthrough = { [\"<C-w>\"] = true }") != std::string::npos,
               "a terminal-mode <C-w> in the config is a collision, exit 1, with the fix: " + out);
        expect(out.find("<leader>llf (insert), keymap_fim_trigger") != std::string::npos && out.find("typing <Space> in insert mode waits") != std::string::npos,
               "llama.vim's default insert-mode trigger under a Space leader is reported");
        rc = capture({MAIC_BINARY, "nvim", "keymaps", "-u", clean.string()}, out);
        expect(rc == 0 && out.find("no collisions") != std::string::npos, "a plain config: no collisions, exit 0: " + out);
        rc = capture({MAIC_BINARY, "nvim", "keymaps", "--all", "-u", clean.string()}, out);
        expect(rc == 0 && out.find("ok     <leader>mm (normal): MAIC: open or focus") != std::string::npos, "--all lists every key");
        rc = capture({MAIC_BINARY, "nvim"}, out);
        expect(rc == 2 && out.find("usage: maic nvim keymaps") != std::string::npos && out.find("maic nvim setup llama-vim") != std::string::npos, "maic nvim alone is a usage error, exit 2");
    }

    section("maic nvim setup llama-vim");
    {
        // A real headless nvim with -u pointing at an init that defines a stand-in for lazy.nvim: the modules the
        // setup asks about, filled from a spec the way lazy.nvim fills them, its imports read from <config>/lua.
        fs::path root = tmp / "setup", cfg = tmp / "config" / "nvim", plugins = cfg / "lua" / "plugins", ours = plugins / "maic-llama-vim.lua";
        fs::create_directories(plugins);
        fs::create_directories(root);
        std::ofstream(root / "fake_lazy.lua") << R"lua(return function(spec, setup)
  local config, plugins = vim.fn.stdpath("config"), {}
  local function add(p)
    if type(p) ~= "table" or type(p[1]) ~= "string" then return end
    local name = p[1]:match("[^/]+$")
    plugins[name] = plugins[name] or { name = name, url = "https://github.com/" .. p[1] .. ".git", _ = { frags = {} } }
    table.insert(plugins[name]._.frags, #plugins[name]._.frags + 1)
    for k, v in pairs(p) do if type(k) == "string" then plugins[name][k] = v end end
  end
  local function walk(s)
    if type(s) ~= "table" then return end
    if s.import then
      local dir = config .. "/lua/" .. (s.import:gsub("%.", "/"))
      for name, kind in vim.fs.dir(dir) do
        if kind == "file" and name:match("%.lua$") then
          local mod = dofile(dir .. "/" .. name)
          if type(mod[1]) == "string" then add(mod) else for _, p in ipairs(mod) do add(p) end end
        end
      end
    end
    add(s)
    for _, x in ipairs(s) do walk(x) end
  end
  package.loaded["lazy"] = { setup = function() end }
  if not setup then
    package.loaded["lazy.core.config"] = { options = {}, plugins = {} }
    return
  end
  walk(spec)
  package.loaded["lazy.core.config"] = { options = { spec = spec }, spec = { plugins = plugins, disabled = {} }, plugins = plugins }
end
)lua";
        std::string fake = "dofile('" + (root / "fake_lazy.lua").string() + "')";
        std::ofstream(root / "import.lua") << fake << "({ { import = 'plugins' } }, true)\n";
        std::ofstream(root / "noimport.lua") << fake << "({ { 'folke/tokyonight.nvim' } }, true)\n";
        std::ofstream(root / "nosetup.lua") << fake << "(nil, false)\n";
        std::ofstream(root / "none.lua") << "vim.g.no_lazy_here = 1\n";
        std::ofstream(plugins / "other.lua") << "return { 'folke/tokyonight.nvim' }\n";
        std::string env_data = "XDG_DATA_HOME=" + (root / "data").string();
        auto setup = [&](const std::string& init, std::vector<std::string> extra, std::string& out) {
            std::vector<std::string> argv = {"env", "-u", "NVIM_APPNAME", env_data, MAIC_BINARY, "nvim", "setup", "llama-vim"};
            if (!init.empty()) argv.insert(argv.end(), {"-u", (root / init).string()});
            argv.insert(argv.end(), extra.begin(), extra.end());
            argv.push_back("</dev/null");
            return capture(argv, out);
        };
        auto has = [](const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; };
        std::string out;

        std::string docs = read_file(fs::path(MAIC_PLUGIN_DIR).parent_path() / "docs" / "models.md");
        expect(has(docs, "```lua\n" + llama_vim_spec() + "```"), "the spec MAIC writes is the one docs/models.md shows");

        int rc = setup("none.lua", {"--yes"}, out);
        expect(rc == 0 && has(out, "lazy.nvim is not installed") && has(out, "require(\"lazy\") fails") && has(out, "Nothing was written") && has(out, "'ggml-org/llama.vim'"),
               "no lazy.nvim: a noop, exit 0, saying why, with the spec to paste: " + out);
        rc = setup("", {"--yes"}, out);
        expect(rc == 0 && has(out, "lazy.nvim is not installed: there is no ") && has(out, "Nothing was written"), "without -u and no lazy.nvim directory: the same, and nvim is not started: " + out);
        rc = capture({"env", "PATH=/nonexistent", env_data, MAIC_BINARY, "nvim", "setup", "llama-vim", "--yes", "</dev/null"}, out);
        expect(rc == 0 && has(out, "nvim is not on PATH") && has(out, "Nothing was written"), "no nvim on PATH: a noop, exit 0: " + out);
        rc = setup("nosetup.lua", {"--yes"}, out);
        expect(rc == 0 && has(out, "does not call require(\"lazy\").setup(...)") && has(out, "Nothing was written"), "lazy.nvim installed but never set up: a noop: " + out);
        rc = setup("noimport.lua", {"--yes"}, out);
        expect(rc == 0 && has(out, "imports no directory") && has(out, "{ import = \"plugins\" }") && has(out, (plugins / "llama-vim.lua").string()) && has(out, "return {"),
               "no import directory: where to add the import and the file, exit 0: " + out);
        expect(!fs::exists(ours) && !fs::exists(plugins / "llama-vim.lua"), "and nothing was written");

        rc = setup("import.lua", {"--dry-run"}, out);
        expect(rc == 0 && has(out, "Write " + ours.string() + " (a new file)") && has(out, "{ import = \"plugins\" }") && has(out, "--dry-run: nothing was written"),
               "--dry-run shows the file, its content and the import it relies on: " + out);
        expect(!fs::exists(ours), "and writes nothing");
        rc = setup("import.lua", {}, out);
        expect(rc == 2 && has(out, "off a terminal it needs --yes") && !fs::exists(ours), "off a terminal without --yes: refused, exit 2, nothing written: " + out);

        rc = setup("import.lua", {"--yes"}, out);
        std::string written = read_file(ours);
        expect(rc == 0 && has(out, "wrote " + ours.string()) && has(out, ":Lazy sync") && has(out, "maic lazy-lock record"), "--yes writes it and says what is next: " + out);
        expect(written.rfind("-- Written by MAIC (maic nvim setup llama-vim) on ", 0) == 0 && has(written, "-- Undo with: maic nvim setup llama-vim --remove\n") &&
                   has(written, "docs/models.md#code-completion") && has(written, "\nreturn {\n    'ggml-org/llama.vim',\n") &&
                   has(written, "endpoint_fim = 'http://127.0.0.1:8084/infill'") && has(written, "model_fim = 'current'") && has(written, "keymap_fim_trigger = '<M-f>'") &&
                   has(written, "keymap_fim_accept_word = '<M-]>'") && has(written, "keymap_inst_accept = ''") && has(written, "keymap_inst_cancel = ''") && !has(written, "(below)"),
               "the file: MAIC's header, then the spec from docs/models.md: " + written);
        fs::path load = root / "load.lua";
        std::ofstream(load) << "local s = dofile(arg[1])\nassert(s[1] == 'ggml-org/llama.vim')\ns.init()\nassert(vim.g.llama_config.model_fim == 'current' and vim.g.llama_config.keymap_inst_cancel == '')\n";
        expect(run({"nvim", "--headless", "-u", "NONE", "-i", "NONE", "-n", "-l", load.string(), ours.string()}) == 0, "it loads as a spec whose init sets g:llama_config");
        expect(read_file(plugins / "other.lua") == "return { 'folke/tokyonight.nvim' }\n", "no other file is touched");

        rc = setup("import.lua", {"--yes"}, out);
        expect(rc == 0 && has(out, "already holds this spec") && read_file(ours) == written, "a second run changes nothing: " + out);
        std::string edited = written;
        edited.replace(edited.find("'<M-f>'"), 7, "'<M-g>'");
        std::ofstream(ours) << edited;
        rc = setup("import.lua", {"--yes"}, out);
        expect(rc == 0 && has(out, "Update " + ours.string() + ", which MAIC wrote") && has(out, "- ") && has(out, "<M-g>") && has(out, "+ ") && has(out, "updated " + ours.string()) &&
                   read_file(ours) == written,
               "MAIC's own file changed by hand: updated in place, with a diff: " + out);

        std::ofstream(plugins / "mine.lua") << "return { 'ggml-org/llama.vim', opts = {} }\n";
        rc = setup("import.lua", {"--yes"}, out);
        expect(rc == 1 && has(out, "is already in your lazy.nvim spec") && has(out, (plugins / "mine.lua").string()) && has(out, "merge them into that spec by hand") && read_file(ours) == written,
               "llama.vim also specified elsewhere: refused, exit 1, naming the file: " + out);
        fs::remove(plugins / "mine.lua");

        rc = setup("", {"--remove", "--dry-run"}, out);
        expect(rc == 0 && has(out, "Remove " + ours.string()) && has(out, "--dry-run: nothing was written") && fs::exists(ours), "--remove --dry-run removes nothing: " + out);
        rc = setup("", {"--remove"}, out);
        expect(rc == 2 && has(out, "--remove --yes") && fs::exists(ours), "--remove off a terminal without --yes: refused: " + out);
        rc = setup("", {"--remove", "--yes"}, out);
        expect(rc == 0 && has(out, "removed " + ours.string()) && !fs::exists(ours) && fs::exists(plugins / "other.lua"), "--remove --yes deletes MAIC's file and nothing else: " + out);
        rc = setup("", {"--remove", "--yes"}, out);
        expect(rc == 0 && has(out, "nothing of MAIC's to remove"), "--remove with nothing there: exit 0: " + out);

        std::ofstream(ours) << "return {}\n";
        rc = setup("import.lua", {"--yes"}, out);
        expect(rc == 1 && has(out, "MAIC did not write it") && read_file(ours) == "return {}\n", "a file of that name MAIC did not write: refused, left alone: " + out);
        rc = setup("", {"--remove", "--yes"}, out);
        expect(rc == 1 && has(out, "will not remove it") && fs::exists(ours), "and --remove refuses it too: " + out);
        fs::remove(ours);

        std::ofstream(plugins / "mine.lua") << "return { { 'ggml-org/llama.vim' } }\n";
        rc = setup("import.lua", {"--yes"}, out);
        expect(rc == 1 && has(out, (plugins / "mine.lua").string()) && !fs::exists(ours), "llama.vim in the spec and no file of MAIC's: refused, nothing written: " + out);
        fs::remove(plugins / "mine.lua");
    }

    section("the plugin's own Lua tests");
    {
        int rc = run({"nvim", "--headless", "-u", "NONE", "-i", "NONE", "-n", "-l", MAIC_PLUGIN_DIR "/tests/maic_test.lua"});
        expect(rc == 0, "maic.nvim/tests/maic_test.lua passes (exit " + std::to_string(rc) + ")");
    }

    fs::remove_all(tmp);
    return finish();
}
