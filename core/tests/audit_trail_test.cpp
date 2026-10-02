// The audit trail's start-up check and its systemd units (docs/audit-trail.md): when an entry point holds for an
// audit, from a synthetic trail under a throwaway XDG_STATE_HOME and XDG_CONFIG_HOME. The writer itself is in
// agent_test, the settings in trust_test, the commands in cli_smoke and the audit in tools/audit.
#include "check.hpp"

#include "maic/audit_trail.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace maic;
namespace fs = std::filesystem;

namespace {

std::time_t at(const char* text) {
    std::tm tm{};
    strptime(text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return timegm(&tm);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    fs::path root = fs::temp_directory_path() / ("maic-audit-trail-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);

    section("durations");
    {
        expect(parse_duration("90") == 90 && parse_duration("30m") == 1800 && parse_duration("12h") == 43200 && parse_duration("1d") == 86400 &&
                   parse_duration("2w") == 14 * 86400,
               "seconds, minutes, hours, days, weeks");
        int refused = 0;
        for (const char* bad : {"", "0", "1y", "d", "-1d", "1.5d", "1 d"}) {
            try {
                parse_duration(bad);
            } catch (const std::exception&) {
                ++refused;
            }
        }
        expect(refused == 7, "anything else is refused");
    }

    section("the due check");
    {
        AuditSettings s;
        expect(!audit_due(s, at("2026-10-05T00:00:00Z")).enforce(), "no trail: nothing to hold for");
        fs::create_directories(audit_trail_dir());
        std::ofstream(audit_trail_dir() / "20261001.jsonl") << "{\"id\":1}\n";
        AuditDue d = audit_due(s, at("2026-10-01T12:00:00Z"));
        expect(!d.due && !d.enforce() && d.live_bytes == 9, "never audited, its first container under a day old: not due");
        d = audit_due(s, at("2026-10-02T00:00:01Z"));
        expect(d.due && !d.scheduled && d.enforce() && d.why.find("no scheduler ran it") != std::string::npos,
               "a day on and no scheduler installed: the data-driven check holds: " + d.why);
        fs::create_directories(systemd_user_dir());
        std::ofstream(systemd_user_dir() / "maic-leak-audit.timer") << "[Timer]\n";
        d = audit_due(s, at("2026-10-02T00:00:01Z"));
        expect(d.due && d.scheduled && !d.enforce(), "with the timer installed, due alone is the scheduler's to run");
        d = audit_due(s, at("2026-10-04T00:00:01Z"));
        expect(d.overdue && d.enforce() && d.why.find("never been audited") != std::string::npos, "grace (3) x every past it, even the timer's start holds: " + d.why);
        std::ofstream(audit_trail_dir() / "index.json") << R"({"last_audit": "2026-10-03T06:00:00Z", "next_audit_due": "2026-10-04T06:00:00Z"})";
        d = audit_due(s, at("2026-10-04T00:00:01Z"));
        expect(!d.due && !d.overdue && !d.enforce(), "a scheduler ran the audit: next_audit_due is ahead, nothing happens");
        fs::remove(systemd_user_dir() / "maic-leak-audit.timer");
        d = audit_due(s, at("2026-10-04T06:00:01Z"));
        expect(d.due && d.enforce() && d.why.find("2026-10-04 06:00 UTC") != std::string::npos, "past next_audit_due with no scheduler: " + d.why);
        d = audit_due(s, at("2026-10-06T06:00:01Z"));
        expect(d.overdue && d.why.find("the last audit was 2026-10-03 06:00 UTC") != std::string::npos, "overdue names the last audit: " + d.why);
        s.live_mb = 1;
        std::ofstream(audit_trail_dir() / "20261003.2.jsonl") << std::string((1 << 20) + 10, 'x');
        d = audit_due(s, at("2026-10-03T07:00:00Z"));
        expect(d.over_size && d.enforce() && d.why.find("past live_mb (1 MB)") != std::string::npos, "past live_mb, before it is due: " + d.why);
        fs::remove_all(audit_trail_dir());
    }

    section("the systemd units");
    {
        std::string service = render_unit(read_file(fs::path(MAIC_CONTRIB) / "systemd" / "maic-leak-audit.service"), "/opt/maic/bin/maic-leak-audit", "/opt/maic/bin/maic", "12h");
        std::string timer = render_unit(read_file(fs::path(MAIC_CONTRIB) / "systemd" / "maic-leak-audit.timer"), "/opt/maic/bin/maic-leak-audit", "/opt/maic/bin/maic", "12h");
        expect(service.find("\nExecStart=/opt/maic/bin/maic-leak-audit\n") != std::string::npos && service.find("\nEnvironment=MAIC_BIN=/opt/maic/bin/maic\n") != std::string::npos &&
                   service.find("Type=oneshot") != std::string::npos && service.find('@') == std::string::npos,
               "the service runs maic-leak-audit, with MAIC_BIN for its maic");
        expect(timer.find("\nOnUnitActiveSec=12h\n") != std::string::npos && timer.find("WantedBy=timers.target") != std::string::npos && timer.find('@') == std::string::npos,
               "the timer repeats every `every`");
    }

    fs::remove_all(root);
    return failures();
}
