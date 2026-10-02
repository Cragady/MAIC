#include "audit_trail.hpp"

#include "maic/audit_trail.hpp"
#include "maic/llm.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>

extern char** environ;

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

std::string find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "";
    for (size_t start = 0; start <= dirs.size();) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        fs::path candidate = fs::path(dirs.substr(start, end - start).empty() ? "." : dirs.substr(start, end - start)) / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) return candidate.string();
        start = end + 1;
    }
    return "";
}

std::string self_exe() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "" : exe.string();
}

// Runs argv with stdin on /dev/null and stderr inherited; stdout into `out` when given. The exit code, or -1 when
// it could not start.
int run(const std::vector<std::string>& argv, std::string* out) {
    int fds[2] = {-1, -1};
    if (out && pipe2(fds, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    std::vector<char*> av;
    std::vector<std::string> copy = argv;
    for (auto& a : copy) av.push_back(a.data());
    av.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out) close(fds[1]);
    if (rc != 0) {
        if (out) close(fds[0]);
        return -1;
    }
    if (out) {
        char buf[4096];
        for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) != 0;) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) break;
            out->append(buf, static_cast<size_t>(n));
        }
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

namespace {

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string size_text(uintmax_t bytes) {
    char buf[32];
    if (bytes < (1u << 20)) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    return buf;
}

// maic-leak-audit: installed beside this binary (a release), else on PATH, else the source tree's (a dev build).
std::string leak_audit_path() {
    std::error_code ec;
    if (std::string exe = self_exe(); !exe.empty() && fs::is_regular_file(fs::path(exe).parent_path() / "maic-leak-audit", ec)) {
        return (fs::path(exe).parent_path() / "maic-leak-audit").string();
    }
    if (std::string found = find_on_path("maic-leak-audit"); !found.empty()) return found;
    fs::path tree = root_dir() / "tools" / "audit" / "leak_audit.py";
    return fs::is_regular_file(tree, ec) ? tree.string() : "";
}


std::string first_line(std::string text) {
    if (auto nl = text.find('\n'); nl != std::string::npos) text.resize(nl);
    return text;
}

json read_index() {
    std::ifstream in(audit_trail_dir() / "index.json");
    json index = json::parse(in, nullptr, false);
    return index.is_object() ? index : json::object();
}

struct Containers {
    size_t files = 0;
    uintmax_t bytes = 0;
};

Containers containers() {
    static const std::regex re(R"(\d{8}(\.\d+)?\.jsonl)");
    Containers c;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec)) {
        if (!std::regex_match(e.path().filename().string(), re)) continue;
        ++c.files;
        c.bytes += e.file_size(ec);
    }
    return c;
}

long long last_id_given() {
    std::ifstream in(audit_trail_dir() / "seq");
    long long id = 0;
    in >> id;
    return id;
}

// Archive chunks by name with the UTC time in it: audit-chunk-<YYYYMMDDTHHMMSSZ>-<n>.tar.gz.
struct Chunk {
    fs::path path;
    std::time_t time = 0;
    uintmax_t bytes = 0;
};

std::vector<Chunk> chunks(const std::string& archive) {
    static const std::regex re(R"(audit-chunk-(\d{8}T\d{6}Z)-\d+\.tar\.gz)");
    std::vector<Chunk> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(archive, ec)) {
        std::smatch m;
        std::string name = e.path().filename().string();
        if (!std::regex_match(name, m, re)) continue;
        std::tm tm{};
        strptime(m[1].str().c_str(), "%Y%m%dT%H%M%SZ", &tm);
        out.push_back({e.path(), timegm(&tm), e.file_size(ec)});
    }
    std::sort(out.begin(), out.end(), [](const Chunk& a, const Chunk& b) { return a.path < b.path; });
    return out;
}

int status(const Settings& settings, bool as_json) {
    const AuditSettings& a = settings.audit;
    json index = read_index();
    Containers c = containers();
    std::map<std::string, long long> by_state = {{"live", 0}, {"stale live", 0}, {"archival", 0}};
    long long archived = 0;
    for (const auto& r : index.value("ranges", json::array())) {
        if (r.is_object()) by_state[r.value("state", "live")] += r.value("count", 0LL);
    }
    for (const auto& r : index.value("archived", json::array())) {
        if (r.is_object()) archived += r.value("count", 0LL);
    }
    long long given = last_id_given();
    long long unaudited = std::max(0LL, given - index.value("last_id", 0LL));
    std::vector<Chunk> found = a.archive == "off" ? std::vector<Chunk>{} : chunks(a.archive);
    uintmax_t chunk_bytes = 0;
    for (const auto& ch : found) chunk_bytes += ch.bytes;
    AuditDue due = audit_due(a, std::time(nullptr));
    std::string last = index.value("last_audit", ""), next = index.value("next_audit_due", "");
    if (as_json) {
        json out = {{"on", a.enabled}, {"every", a.every}, {"every_seconds", a.every_seconds}, {"grace", a.grace}, {"live_window", a.live_window},
                    {"live_window_seconds", a.live_window_seconds}, {"stale_days", a.stale_days}, {"order", a.order}, {"enforce", a.enforce},
                    {"start_services", a.start_services}, {"judge", a.judge}, {"judge_thinking", a.judge_thinking}, {"judge_max_tokens", a.judge_max_tokens},
                    {"file_mb", a.file_mb}, {"live_mb", a.live_mb}, {"chunk_mb", a.chunk_mb}, {"archive", a.archive}, {"dir", audit_trail_dir().string()},
                    {"files", c.files}, {"live_bytes", c.bytes}, {"entries", by_state}, {"not_yet_audited", unaudited}, {"archived_entries", archived},
                    {"chunks", found.size()}, {"chunk_bytes", chunk_bytes}, {"last_audit", last}, {"next_audit_due", next}, {"scheduled", due.scheduled},
                    {"hold", due.enforce()}};
        std::cout << out.dump() << "\n";
        return 0;
    }
    std::cout << "audit trail: " << (a.enabled ? "on" : "off") << " (" << audit_settings_path().string()
              << (fs::exists(audit_settings_path()) ? "" : ", not written yet: maic audit-trail init") << "; off by default)\n"
              << "  " << audit_trail_dir().string() << ": " << c.files << (c.files == 1 ? " file, " : " files, ") << size_text(c.bytes)
              << " (live_mb " << a.live_mb << ")\n"
              << "  entries by the last audit: " << by_state["live"] << " live, " << by_state["stale live"] << " stale live, " << by_state["archival"]
              << " archival; " << unaudited << " not yet audited (last id " << given << ")\n"
              << "  last audit: " << (last.empty() ? "never" : last) << "; next due: " << (next.empty() ? "a day after the first entry" : next) << " (every "
              << a.every << ")\n"
              << "  schedule: "
              << (due.scheduled ? "the systemd timer (maic audit-trail schedule remove takes it away)"
                                : "none; the due check at each start stands in (maic audit-trail schedule install)")
              << "\n";
    if (a.enabled && due.enforce()) std::cout << "  the next start holds for an audit (" << a.enforce << "): " << due.why << "\n";
    if (a.archive == "off") {
        std::cout << "  archive: off (an entry that gets the retirement signal is deleted)\n";
    } else {
        std::cout << "  archive: " << a.archive << ", " << found.size() << (found.size() == 1 ? " chunk, " : " chunks, ") << size_text(chunk_bytes) << ", "
                  << archived << " entries; maic audit-trail offsite DEST prints how to move old chunks on\n";
    }
    return 0;
}

int purge() {
    Containers c = containers();
    if (c.files == 0 && !fs::exists(audit_trail_dir() / "index.json")) {
        std::cout << "no audit trail to purge\n";
        return 0;
    }
    if (!isatty(STDIN_FILENO)) {
        std::cerr << "maic: purging the audit trail asks at a terminal; run maic audit-trail purge in one. Nothing was deleted.\n";
        return 2;
    }
    std::cout << "delete the live audit trail (" << c.files << (c.files == 1 ? " file, " : " files, ") << size_text(c.bytes)
              << "; what it holds can no longer be audited; the archive is not touched)? [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || (line != "y" && line != "Y" && line != "yes")) {
        std::cout << "nothing was deleted\n";
        return 0;
    }
    static const std::regex re(R"(\d{8}(\.\d+)?\.jsonl|index\.json)");
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec)) {
        if (std::regex_match(e.path().filename().string(), re)) fs::remove(e.path());
    }
    std::cout << "deleted " << c.files << (c.files == 1 ? " file" : " files") << " of audit trail and its index (ids go on from " << last_id_given() << ")\n";
    return 0;
}

// Prints, and never runs, the commands that would move dusty chunks from the archive to DEST.
int offsite(const AuditSettings& a, const std::vector<std::string>& args) {
    std::string dest, older = "90d";
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--older-than" && i + 1 < args.size()) older = args[++i];
        else if (dest.empty() && args[i].rfind("--", 0) != 0) dest = args[i];
        else throw std::runtime_error("maic audit-trail offsite DEST [--older-than 90d]");
    }
    if (dest.empty()) throw std::runtime_error("maic audit-trail offsite DEST [--older-than 90d]");
    if (a.archive == "off") throw std::runtime_error("archive is \"off\" in " + audit_settings_path().string() + ": there are no chunks to move (docs/audit-trail.md)");
    long age = parse_duration(older);
    std::time_t now = std::time(nullptr);
    std::vector<Chunk> picked;
    uintmax_t bytes = 0;
    for (const auto& ch : chunks(a.archive)) {
        if (now - ch.time < age) continue;
        picked.push_back(ch);
        bytes += ch.bytes;
    }
    if (picked.empty()) {
        std::cout << "no chunk in " << a.archive << " is older than " << older << "; nothing to move\n";
        return 0;
    }
    std::vector<std::string> files, names, sums;
    std::cout << "off-site: " << picked.size() << (picked.size() == 1 ? " chunk" : " chunks") << " older than " << older << " in " << a.archive << ", "
              << size_text(bytes) << " in all:\n";
    for (const auto& ch : picked) {
        char day[16];
        std::tm tm{};
        gmtime_r(&ch.time, &tm);
        std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);
        std::cout << "  " << ch.path.filename().string() << "  " << size_text(ch.bytes) << "  " << day << "\n";
        std::string sum = ch.path.string() + ".sha256";
        files.push_back(quote(ch.path.string()));
        files.push_back(quote(sum));
        names.push_back(ch.path.filename().string());
        sums.push_back(quote(ch.path.filename().string() + ".sha256"));
    }
    auto joined = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& s : v) out += (out.empty() ? "" : " ") + s;
        return out;
    };
    std::string q = quote(dest), check = "(cd " + q + " && sha256sum -c " + joined(sums) + ")", remove = "rm " + joined(files);
    std::cout << "\nMAIC runs none of this: these are commands for you to check and run. Each chunk has a .sha256 beside it, and\n"
                 "manifest.json inside it lists the SHA-256 of every member; delete nothing until the copy checks.\n";
    bool any = false;
    if (!find_on_path("rsync").empty()) {
        any = true;
        std::cout << "\nrsync (a local disk, a mounted share or host:path over ssh; it removes each source only after its copy):\n"
                  << "  rsync -a --checksum --remove-source-files " << joined(files) << " " << quote(dest + (dest.back() == '/' ? "" : "/")) << "\n"
                  << "  then, where DEST is: " << check << "\n";
    }
    if (!find_on_path("rclone").empty()) {
        std::string includes;
        for (const auto& n : names) includes += " --include " + quote(n) + " --include " + quote(n + ".sha256");
        any = true;
        std::cout << "\nrclone (DEST as an rclone remote, e.g. b2:bucket/maic-audit; it compares checksums where both sides have them):\n"
                  << "  rclone move --checksum" << includes << " " << quote(a.archive) << " " << q << "\n"
                  << "  then compare rclone hashsum sha256 " << q << " with the .sha256 files (rclone move deletes each source once it is across)\n";
    }
    if (!find_on_path("restic").empty()) {
        any = true;
        std::cout << "\nrestic (DEST a restic repository: encrypted, deduplicated, versioned; restic -r " << q << " init the first time):\n"
                  << "  restic -r " << q << " backup " << joined(files) << "\n"
                  << "  restic -r " << q << " check --read-data\n"
                  << "  then, with the snapshot listed by restic -r " << q << " snapshots: " << remove << "\n";
    }
    if (!find_on_path("kopia").empty()) {
        any = true;
        std::cout << "\nkopia (DEST a kopia repository: encrypted, deduplicated; kopia repository create filesystem --path " << q << " the first time):\n"
                  << "  kopia repository connect filesystem --path " << q << "\n"
                  << "  kopia snapshot create " << quote(a.archive) << "\n"
                  << "  kopia snapshot verify --verify-files-percent=100\n"
                  << "  then: " << remove << "\n"
                  << "  (the snapshot holds the whole archive directory; only the chunks above are removed)\n";
    }
    if (!find_on_path("syncthing").empty()) {
        any = true;
        std::cout << "\nsyncthing (another machine keeps the copy; it runs as your service, nothing here starts it):\n"
                  << "  1. in its web UI share " << a.archive << " with the device that holds DEST, Folder Type \"Send Only\" here\n"
                  << "  2. on that device put the folder at DEST, Folder Type \"Receive Only\", and set ignoreDelete under Advanced so\n"
                  << "     deletions here never reach it\n"
                  << "  3. once it shows Up to Date there, check there: " << check << "\n"
                  << "  4. then here: " << remove << "\n";
    }
    if (!find_on_path("cp").empty() && !find_on_path("sha256sum").empty()) {
        std::cout << "\n" << (any ? "or with " : "") << "coreutils (cp, sha256sum, rm; DEST a mounted directory), one chunk at a time:\n";
        for (const auto& ch : picked) {
            std::string c = quote(ch.path.string()), s = quote(ch.path.string() + ".sha256");
            std::cout << "  cp -a " << c << " " << s << " " << q << " && (cd " << q << " && sha256sum -c " << quote(ch.path.filename().string() + ".sha256")
                      << ") && rm " << c << " " << s << "\n";
        }
    }
    std::cout << "\nby hand, if none of these fits:\n"
              << "  1. copy each chunk above and its .sha256 to " << dest << "\n"
              << "  2. there, check each copy: its SHA-256 must be the first word of its .sha256\n"
              << "  3. only when every copy checks, delete the originals from " << a.archive << "\n";
    return 0;
}

int schedule(const AuditSettings& a, const std::vector<std::string>& args) {
    if (args.size() != 2 || (args[1] != "install" && args[1] != "remove")) throw std::runtime_error("maic audit-trail schedule install|remove");
    fs::path dir = systemd_user_dir();
    fs::path service = dir / "maic-leak-audit.service", timer = dir / "maic-leak-audit.timer";
    std::string systemctl = find_on_path("systemctl");
    auto ctl = [&](std::vector<std::string> argv) {
        argv.insert(argv.begin(), {systemctl, "--user"});
        std::string shown;
        for (size_t i = 1; i < argv.size(); ++i) shown += (i > 1 ? " " : "") + argv[i];
        int rc = run(argv, nullptr);
        std::cout << "  systemctl " << shown << (rc == 0 ? "" : "  (failed, exit " + std::to_string(rc) + ")") << "\n";
        return rc == 0;
    };
    if (args[1] == "remove") {
        if (!fs::exists(service) && !fs::exists(timer)) {
            std::cout << "no audit schedule in " << dir.string() << "\n";
            return 0;
        }
        if (!systemctl.empty()) ctl({"disable", "--now", "maic-leak-audit.timer"});
        fs::remove(timer);
        fs::remove(service);
        if (!systemctl.empty()) ctl({"daemon-reload"});
        std::cout << "removed the audit schedule from " << dir.string() << "; the due check at each start stands in\n";
        return 0;
    }
    std::string exec = leak_audit_path();
    if (exec.empty()) throw std::runtime_error("maic-leak-audit is not installed beside maic or on PATH; nothing was written");
    fs::path templates = root_dir() / "contrib" / "systemd";
    auto read = [&](const char* name) {
        std::ifstream in(templates / name);
        if (!in) throw std::runtime_error("no " + (templates / name).string() + "; nothing was written");
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    std::string service_text = render_unit(read("maic-leak-audit.service"), exec, self_exe(), a.every);
    std::string timer_text = render_unit(read("maic-leak-audit.timer"), exec, self_exe(), a.every);
    fs::create_directories(dir);
    std::ofstream(service) << service_text;
    std::ofstream(timer) << timer_text;
    std::cout << "wrote " << service.string() << " and " << timer.filename().string() << ": " << exec << " every " << a.every << "\n";
    if (systemctl.empty()) {
        std::cout << "no systemctl here: the units are written but not enabled; the due check at each start stands in\n";
        return 0;
    }
    bool ok = ctl({"daemon-reload"}) && ctl({"enable", "--now", "maic-leak-audit.timer"});
    if (!ok) std::cout << "the timer is not running; the due check at each start stands in until it is\n";
    if (!a.enabled) std::cout << "the trail is off (enabled = false): the timer audits session transcripts only until you turn it on\n";
    return ok ? 0 : 1;
}

// judge-and-hold with start_services: the judge's local service, started when it is stopped (a port another server
// holds is left for the audit to try). Anything that keeps it from starting is said and left to the audit, which then
// refuses and the scan stands in.
void start_judge_service(const Settings& settings, const std::string& judge) {
    std::string model = judge;
    if (auto p = find_preset(settings.presets, model)) model = p->model;
    std::string provider;
    try {
        provider = resolve_model(settings.providers, resolve_model_alias(model)).first.name;
    } catch (const std::exception&) {
        return;
    }
    for (const auto& def : load_services(root_dir() / "services")) {
        if (def.name != provider || service_status(def).state != ServiceState::Stopped) continue;
        bool said = false;
        try {
            require_armed("start services");
            std::cerr << "audit trail: starting " << def.name << " for the judge..." << std::flush;
            said = true;
            bool ready = start_service(def);
            std::cerr << (ready ? " ready\n" : " still starting\n");
        } catch (const std::exception& e) {
            std::cerr << (said ? " failed\n" : "") << "audit trail: " << def.name << " did not start: " << e.what() << "\n";
        }
    }
}

}  // namespace

int cmd_audit_trail(const std::vector<std::string>& args) {
    static const char* usage = "maic audit-trail init | status [--json] | purge | offsite DEST [--older-than 90d] | schedule install|remove";
    std::string sub = args.empty() ? "status" : args[0];
    if (sub == "init") {
        if (args.size() != 1) throw std::runtime_error(usage);
        bool wrote = write_default_audit_settings();
        std::cout << (wrote ? "wrote " : "already there, left as it is: ") << audit_settings_path().string() << (wrote ? " (enabled = false; docs/audit-trail.md)" : "") << "\n";
        return 0;
    }
    Settings settings = load_settings();
    if (sub == "status" && (args.size() <= 1 || (args.size() == 2 && args[1] == "--json"))) return status(settings, args.size() == 2);
    if (sub == "purge" && args.size() == 1) return purge();
    if (sub == "offsite") return offsite(settings.audit, args);
    if (sub == "schedule") return schedule(settings.audit, args);
    throw std::runtime_error(usage);
}

void audit_gate(const Settings& settings) {
    const AuditSettings& a = settings.audit;
    if (!a.enabled) return;
    AuditDue due = audit_due(a, std::time(nullptr));
    if (!due.enforce()) return;
    auto say = [](const std::string& text) { std::cerr << "audit trail: " << text << "\n" << std::flush; };
    if (a.enforce == "notify") {
        say(due.why + "; run maic-leak-audit (maic audit-trail schedule install runs it for you)");
        return;
    }
    std::string exec = leak_audit_path();
    if (exec.empty()) {
        say(due.why + "; maic-leak-audit is not installed, so nothing was audited");
        return;
    }
    if (std::string exe = self_exe(); !exe.empty()) setenv("MAIC_BIN", exe.c_str(), 1);
    std::string line;
    if (a.enforce == "judge-and-hold") {
        say(due.why + "; auditing with the local judge (" + a.judge + ") before the session opens, everything else on hold...");
        if (a.start_services) start_judge_service(settings, a.judge);
        int rc = run({exec, "--model", a.judge}, &line);
        say(first_line(line));
        if (rc == 0) return;
        say("the judge could not run, so the phase-1 scan stands in: its entries are marked scanned, pending judgement, and the next start tries the judge again");
    } else {
        say(due.why + "; scanning (phase 1) and continuing");
    }
    line.clear();
    run({exec, "--scan"}, &line);
    say(first_line(line));
}

}  // namespace maic
