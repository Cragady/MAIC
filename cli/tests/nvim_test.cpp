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
#include "maic/theme.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
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

struct Recorder : AgentEvents {
    std::vector<std::string> results;
    std::vector<ApprovalRequest> asked;
    void on_text(std::string_view, bool) override {}
    void on_tool_call(const std::string&) override {}
    void on_tool_result(const std::string& t, bool) override { results.push_back(t); }
    void on_notice(const std::string&) override {}
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

    section("the plugin's own Lua tests");
    {
        int rc = run({"nvim", "--headless", "-u", "NONE", "-i", "NONE", "-n", "-l", MAIC_PLUGIN_DIR "/tests/maic_test.lua"});
        expect(rc == 0, "maic.nvim/tests/maic_test.lua passes (exit " + std::to_string(rc) + ")");
    }

    fs::remove_all(tmp);
    return finish();
}
