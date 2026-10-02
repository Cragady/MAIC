// Instruction discovery (docs/instructions.md): the classes read all or highest, custom class names, local files,
// the system slot, the reading order, @path imports, on-demand nested files from trusted directories only, the
// global-only `instructions` keys, and the trust hash over every instruction file a directory contributes.
// Everything lives under a throwaway HOME, config and state.
#include "check.hpp"

#include "maic/agent.hpp"
#include "maic/instructions.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/trust.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace maic;
namespace fs = std::filesystem;

namespace {

fs::path g_root;

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// The files in reading order, relative to the test root: "home/dev/p/MAIC.md | ...".
std::string order(const std::vector<InstructionFile>& files) {
    std::string out;
    for (const auto& f : files) out += (out.empty() ? "" : " | ") + f.path.lexically_relative(g_root).string();
    return out;
}

const InstructionFile* find(const std::vector<InstructionFile>& files, const fs::path& p) {
    for (const auto& f : files) {
        if (f.path == p) return &f;
    }
    return nullptr;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    return out;
}

}  // namespace

int main() {
    g_root = fs::weakly_canonical(fs::temp_directory_path()) / ("maic-instructions-test-" + std::to_string(getpid()));
    fs::remove_all(g_root);
    fs::path home = g_root / "home", cfg = home / ".config" / "maic", sys = g_root / "etc-maic";
    fs::create_directories(cfg);
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home / ".config").c_str(), 1);
    setenv("XDG_STATE_HOME", (g_root / "state").c_str(), 1);
    setenv("MAIC_TRIPWIRE_FILE", (g_root / "no-lock").c_str(), 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("MAIC_TESTING", "1", 1);
    setenv("MAIC_SYSTEM_CONFIG_DIR", sys.c_str(), 1);
    load_settings(home / "dev");  // the global defaults into the trust config

    fs::path proj = home / "dev" / "proj";
    fs::create_directories(proj / ".git");  // a project marker; not a git working tree, so any change asks again
    for (const char* name : {"CLAUDE.md", "AGENTS.md", "MAIC.md", "CLAUDE.local.md", "MAIC.local.md"}) write_file(proj / name, std::string(name) + " text\n");
    write_file(cfg / "AGENTS.md", "your agents\n");
    write_file(cfg / "MAIC.md", "your maic\n");

    section("the system slot");
    {
        expect(system_instructions_dir() == sys, "MAIC_SYSTEM_CONFIG_DIR is the system directory with MAIC_TESTING=1");
        unsetenv("MAIC_TESTING");
        expect(system_instructions_dir() == "/etc/maic", "and ignored without it: /etc/maic");
        setenv("MAIC_TESTING", "1", 1);
        expect(load_instructions(home / "dev").size() == 2, "empty by default: only your config directory's files");
        write_file(sys / "MAIC.md", "system maic\n");
        write_file(sys / "CLAUDE.md", "system claude\n");
    }

    section("classes, local files and the reading order");
    {
        expect(load_instructions(proj).size() == 4, "an untrusted project contributes nothing");
        trust_dir(project_dir(proj), Origin::Local);
        std::string all = order(load_instructions(proj));
        expect(all == "etc-maic/CLAUDE.md | etc-maic/MAIC.md | home/.config/maic/AGENTS.md | home/.config/maic/MAIC.md | "
                      "home/dev/proj/CLAUDE.md | home/dev/proj/AGENTS.md | home/dev/proj/MAIC.md | home/dev/proj/CLAUDE.local.md | home/dev/proj/MAIC.local.md",
               "read = all: system, then yours, then the project; classes lowest priority first, local files right after: " + all);
        InstructionOptions highest;
        highest.highest = true;
        std::string top = order(load_instructions(proj, highest));
        expect(top == "etc-maic/MAIC.md | home/.config/maic/MAIC.md | home/dev/proj/MAIC.md | home/dev/proj/MAIC.local.md",
               "read = highest: only the top class present in each directory, and the top local file: " + top);
        InstructionOptions no_local;
        no_local.local_files = false;
        expect(!contains(order(load_instructions(proj, no_local)), "local"), "local_files = false reads no .local.md");
        fs::remove(proj / "MAIC.md");
        expect(!trusted(proj), "removing an instruction file asks again");
        trust_dir(project_dir(proj), Origin::Local);
        top = order(load_instructions(proj, highest));
        expect(contains(top, "home/dev/proj/AGENTS.md | home/dev/proj/MAIC.local.md"), "highest falls back to the next class present: " + top);
        write_file(proj / "MAIC.md", "MAIC.md text\n");
        trust_dir(project_dir(proj), Origin::Local);
    }

    section("custom class names, from the global settings only");
    {
        write_file(cfg / "settings.lua", "return { instructions = { files = { 'CLAUDE.md', 'AGENTS.md', 'MAIC.md', 'RULES.md' } } }\n");
        Settings s = load_settings(proj);
        expect(s.instructions.files.size() == 4 && s.instructions.files[3] == "RULES.md", "instructions.files is read from your global file");
        expect(trusted(proj), "a name added whose file does not exist changes nothing");
        write_file(proj / "RULES.md", "rules text\n");
        TrustStatus st = trust_status(project_dir(proj));
        expect(st.trust == Trust::Changed && st.changed.size() == 1 && st.changed[0] == proj / "RULES.md", "a file of a custom class is hashed: adding it asks again");
        expect(!find(load_instructions(proj, s.instructions), proj / "RULES.md"), "and is not read until then");
        trust_dir(project_dir(proj), Origin::Local);
        write_file(cfg / "settings.lua", "return { instructions = { files = { 'MAIC.md', 'RULES.md', 'CLAUDE.md' } } }\n");
        s = load_settings(proj);
        expect(!trusted(proj), "a class taken out of the list (AGENTS.md) changes what is read, so it asks again");
        trust_dir(project_dir(proj), Origin::Local);
        std::string custom = order(load_instructions(proj, s.instructions));
        expect(contains(custom, "home/dev/proj/MAIC.md | home/dev/proj/RULES.md | home/dev/proj/CLAUDE.md | home/dev/proj/MAIC.local.md | home/dev/proj/CLAUDE.local.md") &&
                   !contains(custom, "proj/AGENTS.md"),
               "the classes in the order given; a name left out is not read: " + custom);
        write_file(proj / "RULES.md", "rules text, edited\n");
        expect(trust_status(project_dir(proj)).trust == Trust::Changed, "an edit to it asks again too");
        write_file(proj / "RULES.md", "rules text\n");
        expect(trust_status(project_dir(proj)).trust == Trust::Trusted, "and putting it back passes");
        write_file(proj / "CLAUDE.local.md", "changed\n");
        expect(trust_status(project_dir(proj)).trust == Trust::Changed, "a local file is hashed");
        fs::remove(cfg / "settings.lua");
        load_settings(proj);
        trust_dir(project_dir(proj), Origin::Local);

        std::string bad;
        write_file(cfg / "settings.lua", "return { instructions = { read = 'some' } }\n");
        try {
            load_settings(proj);
        } catch (const std::exception& e) {
            bad = e.what();
        }
        expect(contains(bad, "instructions.read must be \"all\" or \"highest\""), "read takes all or highest");
        write_file(cfg / "settings.lua", "return { instructions = { files = { 'docs/MAIC.md' } } }\n");
        bad.clear();
        try {
            load_settings(proj);
        } catch (const std::exception& e) {
            bad = e.what();
        }
        expect(contains(bad, "instructions.files holds file names"), "files holds plain names");
        fs::remove(cfg / "settings.lua");
        load_settings(proj);
    }

    section("a project's instructions keys are ignored");
    {
        write_file(proj / ".maic" / "settings.lua",
                   "return { instruction_files = { 'X.md' }, instructions = { files = { 'X.md' }, read = 'highest', local_files = false, imports = { depth = 0 }, extra_dirs = true } }\n");
        trust_dir(project_dir(proj), Origin::Local);
        Settings s = load_settings(proj);
        InstructionOptions d;
        expect(s.instructions.files == d.files && !s.instructions.highest && s.instructions.local_files && s.instructions.import_depth == 4 && !s.instructions.extra_dirs,
               "files, read, local_files, imports and extra_dirs stay your global ones");
        std::string w = joined(s.warnings), f = (proj / ".maic" / "settings.lua").string();
        expect(contains(w, f + ": instructions.files is ignored") && contains(w, f + ": instructions.read is ignored") && contains(w, f + ": instructions.local_files is ignored") &&
                   contains(w, f + ": instructions.imports is ignored") && contains(w, f + ": instructions.extra_dirs is ignored"),
               "each with a warning naming the file:\n" + w);
        expect(contains(w, f + ": instruction_files is ignored: instructions.files in your global settings file replaces it"), "the old instruction_files key is named too");
        fs::remove_all(proj / ".maic");
        trust_dir(project_dir(proj), Origin::Local);
    }

    section("imports");
    {
        fs::path imp = home / "dev" / "imp";
        fs::create_directories(imp / ".git");
        write_file(imp / "MAIC.md",
                   "Follow @docs/style.md please, and (@" + (imp / "docs" / "abs.md").string() + ").\n"
                   "Not `@docs/span.md` in a code span, nor \"@docs/dq.md\" in quotes.\n"
                   "```\n@docs/fenced.md\n```\n"
                   "> @docs/quoted.md\n"
                   "@docs/missing.md and @~/notes/outside.md and @~/.config/maic/shared.md\n"
                   "Write to me@example.com, ask @Micaiah.\n");
        write_file(imp / "docs" / "style.md", "style @one.md\n");
        write_file(imp / "docs" / "one.md", "one @two.md\n");
        write_file(imp / "docs" / "two.md", "two @three.md\n");
        write_file(imp / "docs" / "three.md", "three @four.md\n");
        write_file(imp / "docs" / "four.md", "four\n");
        write_file(imp / "docs" / "abs.md", "absolute\n");
        for (const char* name : {"span.md", "dq.md", "fenced.md", "quoted.md"}) write_file(imp / "docs" / name, "skipped\n");
        write_file(imp / "AGENTS.md", "@docs/cycle-a.md\n");
        write_file(imp / "docs" / "cycle-a.md", "a @cycle-b.md\n");
        write_file(imp / "docs" / "cycle-b.md", "b @cycle-a.md\n");
        write_file(home / "notes" / "outside.md", "outside\n");
        write_file(cfg / "shared.md", "shared\n");

        auto targets = import_targets(imp / "MAIC.md", "x @docs/style.md y `@no.md` \"@no2.md\" @plain\n");
        expect(targets.size() == 1 && targets[0] == imp / "docs" / "style.md", "import_targets: relative to the file; spans, quotes and bare words are not imports");

        trust_dir(project_dir(imp), Origin::Local);
        auto files = load_instructions(imp);
        std::string o = order(files);
        std::string d = "home/dev/imp/docs/";
        expect(contains(o, d + "cycle-b.md | " + d + "cycle-a.md | home/dev/imp/AGENTS.md"), "a cycle is read once each, imports right before the importer: " + o);
        expect(contains(o, d + "three.md | " + d + "two.md | " + d + "one.md | " + d + "style.md | " + d + "abs.md | home/.config/maic/shared.md | home/dev/imp/MAIC.md"),
               "relative and absolute imports, four hops deep, each before the file importing it: " + o);
        expect(!contains(o, "four.md"), "the fifth hop is not followed");
        for (const char* name : {"span.md", "dq.md", "fenced.md", "quoted.md"}) expect(!contains(o, name), std::string("not imported from a skipped place: ") + name);
        const InstructionFile* top = find(files, imp / "MAIC.md");
        const InstructionFile* style = find(files, imp / "docs" / "style.md");
        expect(style && style->imported_by == imp / "MAIC.md", "an imported file names its importer");
        expect(top && contains(top->text, "@" + (imp / "docs" / "missing.md").string() + " was not imported: there is no such file"), "a missing import is noted");
        expect(top && contains(top->text, "@" + (home / "notes" / "outside.md").string() + " was not imported: it is outside the trusted directories and your config directory") &&
                   !contains(o, "outside.md"),
               "an import outside the trusted chain and your config directory is refused and noted, not fatal");
        write_file(home / "notes" / "MAIC.md", "notes\n");
        trust_dir(project_dir(home / "notes"), Origin::Local);
        expect(contains(order(load_instructions(imp)), "home/notes/outside.md"), "it is read once its directory is trusted");
        InstructionOptions none;
        none.import_depth = 0;
        expect(order(load_instructions(imp, none)).find("docs/") == std::string::npos, "imports.depth = 0 imports nothing");

        ProjectDir p = project_dir(imp);
        bool hashed = false;
        for (const auto& f : p.imports) hashed = hashed || f == imp / "docs" / "three.md";
        expect(hashed, "the trust hash covers the files imported inside the directory");
        write_file(imp / "docs" / "style.md", "style, changed @one.md\n");
        expect(trust_status(project_dir(imp)).trust == Trust::Changed, "so a change to an imported file asks again");
    }

    section("on-demand nested files, from trusted directories only");
    {
        write_file(proj / "src" / "lib" / "AGENTS.md", "lib agents\n");
        write_file(proj / "src" / "lib" / "MAIC.md", "lib maic\n");
        write_file(proj / "src" / "lib" / "deep" / "MAIC.md", "deep maic\n");
        write_file(proj / "src" / "lib" / "deep" / "f.txt", "x\n");
        write_file(proj / ".hidden" / "AGENTS.md", "hidden\n");
        write_file(proj / "node_modules" / "pkg" / "AGENTS.md", "vendored\n");
        TrustStatus st = trust_status(project_dir(proj));
        expect(st.trust == Trust::Changed && st.changed.size() == 3, "nested files are part of the hash: three new ones ask again");
        ProjectDir p = project_dir(proj);
        expect(p.nested.size() == 3, "hidden directories and node_modules are not searched");
        expect(nested_allowed(proj).empty(), "until trusted again, nothing nested may be attached");
        trust_dir(p, Origin::Local);

        fs::path ws = proj / "src";
        auto dirs = project_dirs(ws);
        expect(dirs.size() == 1 && dirs[0].dir == proj, "a workspace whose nested files the project root covers is not asked about again");
        std::set<fs::path> seen;
        auto allowed = nested_allowed(ws);
        auto got = nested_instructions(ws, proj / "src" / "lib" / "deep" / "f.txt", InstructionOptions{}, allowed, seen);
        std::string o = order(got);
        expect(o == "home/dev/proj/src/lib/AGENTS.md | home/dev/proj/src/lib/MAIC.md | home/dev/proj/src/lib/deep/MAIC.md",
               "a read in a subdirectory attaches the files from there up to the workspace, outermost first: " + o);
        expect(nested_instructions(ws, proj / "src" / "lib" / "deep" / "f.txt", InstructionOptions{}, allowed, seen).empty(), "each once");
        InstructionOptions highest;
        highest.highest = true;
        std::set<fs::path> fresh;
        expect(order(nested_instructions(ws, proj / "src" / "lib" / "f.txt", highest, allowed, fresh)) == "home/dev/proj/src/lib/MAIC.md", "read = highest applies to them too");
        expect(nested_instructions(ws, proj / "src" / "f.txt", InstructionOptions{}, allowed, fresh).empty(), "a file in the workspace itself attaches nothing");

        write_file(proj / "src" / "lib" / "deep" / "MAIC.md", "deep maic, edited\n");
        expect(trust_status(project_dir(proj)).trust == Trust::Changed, "an edit to a nested file asks again");
        expect(nested_allowed(ws).empty(), "and nothing nested is attached meanwhile");
        trust_dir(project_dir(proj), Origin::Local);

        fs::path loose = home / "dev" / "loose";
        write_file(loose / "sub" / "AGENTS.md", "untrusted\n");
        write_file(loose / "sub" / "f.txt", "x\n");
        auto ld = project_dirs(loose);
        expect(ld.size() == 1 && ld[0].dir == loose && ld[0].nested.size() == 1, "a workspace with only nested files is a project directory, asked about");
        std::set<fs::path> s2;
        expect(nested_instructions(loose, loose / "sub" / "f.txt", InstructionOptions{}, nested_allowed(loose), s2).empty(), "untrusted, its nested file is not attached");
    }

    section("extra directories");
    {
        fs::path extra = home / "dev" / "extra";
        write_file(extra / "MAIC.md", "extra\n");
        InstructionOptions on;
        on.extra_dirs = true;
        expect(!contains(order(load_instructions(proj, InstructionOptions{}, {extra})), "extra"), "extra_dirs = false (default): an extra directory's instructions are not loaded");
        expect(!contains(order(load_instructions(proj, on, {extra})), "extra"), "extra_dirs = true, but untrusted: not loaded");
        trust_dir(project_dir(extra), Origin::Local);
        std::string o = order(load_instructions(proj, on, {extra}));
        expect(contains(o, "home/.config/maic/MAIC.md | home/dev/extra/MAIC.md | home/dev/proj/CLAUDE.md"), "trusted: after yours, before the project's: " + o);
    }

    section("imports from your own files wait for your approval");
    {
        fs::path notes = home / "own-notes", target = notes / "style.md";
        write_file(target, "heron style\n");
        write_file(cfg / "MAIC.md", "your maic, and @" + target.string() + "\n");
        std::vector<PendingImport> pending;
        auto files = load_instructions(home / "dev", InstructionOptions{}, {}, &pending);
        const InstructionFile* own = find(files, cfg / "MAIC.md");
        expect(!find(files, target) && pending.size() == 1 && pending[0].importer == cfg / "MAIC.md" && pending[0].target == target && !pending[0].changed,
               "an import of your config file from outside the trusted directories is not read; it waits for approval");
        expect(own && contains(own->text, "the user has not approved it yet (maic trust imports --approve)"), "and the model is told why");
        std::string err;
        try {
            approve_import(cfg / "MAIC.md", target, Origin::Remote);
        } catch (const std::exception& e) {
            err = e.what();
        }
        expect(!err.empty(), "a remote origin cannot approve one");
        approve_import(cfg / "MAIC.md", target, Origin::Local);
        struct stat st {};
        expect(::stat(import_exceptions_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && import_exceptions_path().parent_path() == trust_path().parent_path(),
               "approved: the pair is kept beside trust.json, 0600");
        pending.clear();
        files = load_instructions(home / "dev", InstructionOptions{}, {}, &pending);
        expect(find(files, target) && pending.empty(), "and read from then on, by every session");

        write_file(proj / "AGENTS.md", "@" + target.string() + "\n");
        trust_dir(project_dir(proj), Origin::Local);
        pending.clear();
        files = load_instructions(proj, InstructionOptions{}, {}, &pending);
        const InstructionFile* agents = find(files, proj / "AGENTS.md");
        expect(agents && contains(agents->text, "was not imported: it is outside the trusted directories and your config directory") && pending.empty(),
               "a project file importing the same target keeps the existing rule: refused, nothing to approve");
        write_file(proj / "AGENTS.md", "AGENTS.md text\n");
        trust_dir(project_dir(proj), Origin::Local);

        write_file(target, "heron style, changed\n");
        pending.clear();
        files = load_instructions(home / "dev", InstructionOptions{}, {}, &pending);
        expect(!find(files, target) && pending.size() == 1 && pending[0].changed, "standard tier: a change outside a git working tree is asked about again");
        write_file(cfg / "settings.lua", "return { trust_strictness = 'relaxed' }\n");
        load_settings(home / "dev");
        expect(import_exception_status(cfg / "MAIC.md", target).trust == Trust::Trusted, "relaxed: the change passes");
        write_file(cfg / "settings.lua", "return { trust_strictness = 'strict' }\n");
        load_settings(home / "dev");
        expect(import_exception_status(cfg / "MAIC.md", target).trust == Trust::Trusted, "and was recorded as the new content");
        write_file(target, "heron style, changed again\n");
        TrustStatus ch = import_exception_status(cfg / "MAIC.md", target);
        expect(ch.trust == Trust::Changed && contains(joined(import_prompt(cfg / "MAIC.md", target, ch)), "changed since you approved it: strict"), "strict: any change asks again, and the question says so");
        fs::remove(cfg / "settings.lua");
        load_settings(home / "dev");

        std::string q = joined(import_prompt(cfg / "MAIC.md", target, ch));
        expect(contains(q, "importing file: " + (cfg / "MAIC.md").string()) && contains(q, "target:         " + target.string()) && contains(q, "size:           27 bytes") &&
                   contains(q, "standing instructions for every agent in every project") && contains(q, "cloud included") && contains(q, "every session pays its tokens") &&
                   contains(q, "it can change future instructions"),
               "the question shows the importer, the target, its size and the four implications:\n" + q);

        expect(touches_trust(Action{Action::Kind::Write, import_exceptions_path(), "", {}, "write_file"}) &&
                   touches_trust(Action{Action::Kind::Shell, {}, "maic trust imports --remove x", {}, "run_shell"}),
               "the exception list is the user's alone: the agent's writes and maic trust commands are trust actions");
        expect(changes_approved_import(Action{Action::Kind::Write, target, "", {}, "write_file"}) &&
                   changes_approved_import(Action{Action::Kind::Shell, {}, "echo x > " + target.string(), {}, "run_shell"}) &&
                   !changes_approved_import(Action{Action::Kind::Shell, {}, "cat " + target.string(), {}, "run_shell"}) &&
                   !changes_approved_import(Action{Action::Kind::Write, notes / "other.md", "", {}, "write_file"}),
               "an approved target is self-protected: writes to it are caught, reads are not");

        std::string listed = trust_imports_command({});
        expect(contains(listed, (cfg / "MAIC.md").string() + " -> " + target.string()) && contains(listed, "changed: asked again"), "maic trust imports lists it: " + listed);
        expect(contains(trust_command("trust", {"imports", "--remove", target.string()}, home), "removed " + (cfg / "MAIC.md").string() + " -> " + target.string()),
               "--remove PATH forgets it");
        expect(import_exception_targets().empty() && import_exception_status(cfg / "MAIC.md", target).trust == Trust::Unknown, "and it waits for approval again");
        write_file(cfg / "MAIC.md", "your maic\n");
    }

    fs::remove_all(g_root);
    return finish();
}
