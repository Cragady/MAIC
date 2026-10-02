// Streamed command output (run_sandboxed's taps): chunks in order with their offsets, batching, stdout and stderr
// apart, the model's result unchanged, and a slow consumer that never slows the command.
#include "check.hpp"

#include "maic/full_output.hpp"
#include "maic/sandbox.hpp"
#include "maic/vendor.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace maic;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Chunk {
    OutputStream stream;
    std::string data;
    size_t offset;
    Clock::time_point at;
};

// Records every chunk; `delay` makes it a slow consumer.
struct Collector {
    std::mutex mu;
    std::vector<Chunk> chunks;
    std::chrono::milliseconds delay{0};
    OutputTaps taps() {
        OutputTaps t;
        t.on_output = [this](OutputStream s, std::string_view bytes, size_t offset) {
            {
                std::lock_guard lock(mu);
                chunks.push_back({s, std::string(bytes), offset, Clock::now()});
            }
            if (delay.count()) std::this_thread::sleep_for(delay);
        };
        return t;
    }
    std::string joined(OutputStream s) {
        std::string all;
        for (const auto& c : chunks) {
            if (c.stream == s) all += c.data;
        }
        return all;
    }
    // Every chunk of `s` starts where the one before it ended (nothing dropped).
    bool contiguous(OutputStream s) {
        size_t next = 0;
        for (const auto& c : chunks) {
            if (c.stream != s) continue;
            if (c.offset != next) return false;
            next += c.data.size();
        }
        return true;
    }
};

// What the model is given of `full`, written out from the rule rather than from the code under test.
std::string capped(const std::string& full) {
    if (full.size() <= kOutputHeadBytes + kOutputTailBytes) return full;
    return full.substr(0, kOutputHeadBytes) + "\n... [" + std::to_string(full.size() - kOutputHeadBytes - kOutputTailBytes) + " bytes omitted] ...\n" +
           full.substr(full.size() - kOutputTailBytes);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

unsigned mode_of(const fs::path& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? st.st_mode & 0777 : 0;
}

std::string numbers(size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i <= to; ++i) s += std::to_string(i) + "\n";
    return s;
}

}  // namespace

int main() {
    fs::path ws = fs::temp_directory_path() / ("maic-tool-output-test-" + std::to_string(getpid()));
    fs::remove_all(ws);
    fs::create_directories(ws);
    std::atomic<bool> no{false};
    auto secs = std::chrono::seconds(60);

    section("a command that prints over time");
    {
        Collector c;
        auto t0 = Clock::now();
        auto r = run_sandboxed("for i in 1 2 3 4; do echo line$i; sleep 0.3; done", ws, false, secs, no, {}, c.taps());
        auto t1 = Clock::now();
        expect(r.exit_code == 0 && r.output == "line1\nline2\nline3\nline4\n", "the result is the whole output");
        expect(c.chunks.size() == 4 && c.joined(OutputStream::Stdout) == r.output && c.contiguous(OutputStream::Stdout),
               "each line is a chunk of its own, in order, each offset where the last one ended: " + std::to_string(c.chunks.size()) + " chunks");
        bool live = c.chunks.size() == 4 && c.chunks[0].at - t0 < std::chrono::milliseconds(800) && t1 - c.chunks[0].at > std::chrono::milliseconds(700);
        for (size_t i = 1; live && i < c.chunks.size(); ++i) live = c.chunks[i].at - c.chunks[i - 1].at > std::chrono::milliseconds(150);
        expect(live, "the chunks arrive while the command runs, not at its end");
        expect(r.dropped_chunks == 0 && r.output_bytes == r.output.size(), "nothing was dropped");
    }

    section("batching: 100 ms or 16 KiB, whichever comes first");
    {
        Collector c;
        auto r = run_sandboxed("for i in $(seq 1 50); do echo $i; done; sleep 0.5; echo end", ws, false, secs, no, {}, c.taps());
        expect(r.exit_code == 0 && c.joined(OutputStream::Stdout) == numbers(1, 50) + "end\n" && c.contiguous(OutputStream::Stdout), "the stream is the output, in order");
        expect(c.chunks.size() == 2 && c.chunks[0].data == numbers(1, 50) && c.chunks[1].data == "end\n",
               "fifty quick lines are one chunk and the line after a pause another: " + std::to_string(c.chunks.size()) + " chunks");

        Collector big;
        r = run_sandboxed("head -c 100000 /dev/zero | tr '\\0' a", ws, false, secs, no, {}, big.taps());
        size_t largest = 0;
        for (const auto& ch : big.chunks) largest = std::max(largest, ch.data.size());
        expect(big.chunks.size() == 7 && largest == 16 * 1024 && big.joined(OutputStream::Stdout) == std::string(100000, 'a') && big.contiguous(OutputStream::Stdout),
               "100000 bytes at once are cut at 16 KiB: " + std::to_string(big.chunks.size()) + " chunks, the largest " + std::to_string(largest));

        Collector utf;
        r = run_sandboxed("printf '\xe2\x82\xac%.0s' $(seq 1 6000)", ws, false, secs, no, {}, utf.taps());
        bool whole = !utf.chunks.empty();
        for (const auto& ch : utf.chunks) whole = whole && ch.data.size() % 3 == 0;
        expect(whole && utf.chunks.size() >= 2 && utf.joined(OutputStream::Stdout).size() == 18000, "a chunk never ends inside a UTF-8 character");
    }

    section("stdout and stderr apart");
    {
        Collector c;
        auto r = run_sandboxed_argv({"/bin/sh", "-c", "echo out1; echo err1 >&2; sleep 0.2; echo out2; echo err2 >&2"}, "", ws, true, secs, no, {}, c.taps());
        expect(r.exit_code == 0 && r.output == "out1\nout2\n" && r.error == "err1\nerr2\n", "the result keeps them apart");
        expect(c.joined(OutputStream::Stdout) == "out1\nout2\n" && c.joined(OutputStream::Stderr) == "err1\nerr2\n", "so does the stream");
        expect(c.contiguous(OutputStream::Stdout) && c.contiguous(OutputStream::Stderr), "each stream has its own offsets from 0");

        Collector shell;
        r = run_sandboxed("echo out; echo err >&2", ws, false, secs, no, {}, shell.taps());
        expect(shell.joined(OutputStream::Stdout) == "out\nerr\n" && shell.joined(OutputStream::Stderr).empty(), "run_sandboxed streams the interleaved output as stdout");
    }

    section("the model's result is unchanged");
    {
        std::string full = numbers(1, 200000).substr(0, 1024 * 1024);
        std::string command = "seq 1 200000 | head -c 1048576";
        auto plain = run_sandboxed(command, ws, false, secs, no);
        Collector c;
        auto streamed = run_sandboxed(command, ws, false, secs, no, {}, c.taps());
        expect(plain.output == capped(full), "1 MiB without a tap: 24 KiB of head, 8 KiB of tail and the true omitted count");
        expect(streamed.output == plain.output && streamed.exit_code == plain.exit_code && streamed.output_bytes == full.size(), "the same with a tap, byte for byte");
        expect(c.joined(OutputStream::Stdout) == full && c.contiguous(OutputStream::Stdout), "and the stream is the whole 1 MiB");
        std::string mid = numbers(1, 20000);  // 108894 bytes: over the cap, under the point where the middle is cut
        auto r = run_sandboxed("seq 1 20000", ws, false, secs, no, {}, c.taps());
        expect(r.output == capped(mid), "an output just over the cap is trimmed as before");
        auto small = run_sandboxed("seq 1 100", ws, false, secs, no, {}, c.taps());
        expect(small.output == numbers(1, 100), "a short output is whole");
        std::string seen;
        OutputTaps reads;
        reads.on_read = [&](OutputStream, std::string_view bytes, size_t offset) {
            if (offset == seen.size()) seen += bytes;
        };
        auto read_all = run_sandboxed(command, ws, false, secs, no, {}, reads);
        expect(read_all.output == plain.output && seen == full, "on_read sees every byte, in order, and changes nothing");
    }

    section("a slow consumer never slows the command");
    {
        std::string command = "head -c 8388608 /dev/zero | tr '\\0' x";
        auto t0 = Clock::now();
        auto plain = run_sandboxed(command, ws, false, secs, no);
        auto plain_time = Clock::now() - t0;
        Collector c;
        c.delay = std::chrono::milliseconds(50);
        t0 = Clock::now();
        auto slow = run_sandboxed(command, ws, false, secs, no, {}, c.taps());
        auto slow_time = Clock::now() - t0;
        auto ms = [](Clock::duration d) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()) + " ms"; };
        expect(slow_time < plain_time + std::chrono::milliseconds(700),
               "8 MiB to a consumer that takes 50 ms a chunk: " + ms(slow_time) + " against " + ms(plain_time) + " with none");
        expect(slow.output == plain.output && slow.output_bytes == 8388608, "the result is the same");
        size_t delivered = 0, next = 0;
        bool ordered = true, content = true;
        for (const auto& ch : c.chunks) {
            ordered = ordered && ch.offset >= next;
            next = ch.offset + ch.data.size();
            delivered += ch.data.size();
            content = content && ch.data.find_first_not_of('x') == std::string::npos;
        }
        expect(slow.dropped_chunks > 0 && delivered + slow.dropped_bytes == 8388608,
               "the chunks it could not take were dropped and counted: " + std::to_string(slow.dropped_chunks) + " chunks, " + std::to_string(slow.dropped_bytes) + " bytes");
        expect(ordered && content && delivered <= 2 * 1024 * 1024 + 512 * 1024, "what it got is in order, offsets leaving the gaps, and no more than the backlog allows");
    }

    section("a program gets the default SIGPIPE whatever MAIC ignores");
    {
        signal(SIGPIPE, SIG_IGN);  // as a process with an httplib server does
        auto r = run_sandboxed("seq 1 1000000 | head -c 6", ws, false, secs, no);
        expect(r.output == "1\n2\n3\n", "a writer to a closed pipe ends quietly instead of printing a write error: " + r.output);
        signal(SIGPIPE, SIG_DFL);
    }

    section("cancel and timeout still work with a tap");
    {
        Collector c;
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            cancel = true;
        });
        auto t0 = Clock::now();
        auto r = run_sandboxed("echo started; sleep 30", ws, false, secs, cancel, {}, c.taps());
        canceller.join();
        expect(r.cancelled && Clock::now() - t0 < std::chrono::seconds(3) && c.joined(OutputStream::Stdout) == "started\n", "cancel stops it, and what it printed was streamed");
        Collector t;
        r = run_sandboxed("echo begun; sleep 30", ws, false, std::chrono::seconds(1), no, {}, t.taps());
        expect(r.timed_out && t.joined(OutputStream::Stdout) == "begun\n", "so does the timeout");
    }

    section("the whole output, kept beside the session");
    {
        fs::path sess = ws / "home" / "20260101-120000-tui-1.jsonl";
        fs::create_directories(sess.parent_path());
        std::ofstream(sess) << "{}\n";
        fs::path out = full_output_path(sess, "call_1");
        expect(out == ws / "home" / "20260101-120000-tui-1.d" / "call_1.out", "its place: <id>.d/<call id>.out beside the session file");
        {
            FullOutputWriter w(out, 64 << 20);
            w.add(OutputStream::Stdout, std::string(32 * 1024, 'a'));
            expect(!w.finish(sess.parent_path()) && !fs::exists(out.parent_path()), "an output the model got whole is not kept, and nothing is created");
        }
        std::string full = numbers(1, 400000).substr(0, 2 * 1024 * 1024);
        FullOutputWriter w(out, 64 << 20);
        OutputTaps taps;
        taps.on_read = [&](OutputStream s, std::string_view bytes, size_t) { w.add(s, bytes); };
        auto r = run_sandboxed("seq 1 400000 | head -c 2097152", ws, false, secs, no, {}, taps);
        auto j = w.finish(sess.parent_path());
        expect(r.output == capped(full), "2 MiB: the model's result is the capped one, byte for byte");
        expect(j && slurp(out) == full, "and the kept file is the whole stream, byte for byte");
        expect(j && (*j)["path"] == "20260101-120000-tui-1.d/call_1.out" && (*j)["bytes"] == full.size() && (*j)["sha256"] == file_sha256(out) &&
                   (*j)["delivered_to_model"] == false && !j->contains("dropped"),
               "the record's full_output: its path beside the session, bytes, sha256 and delivered_to_model false");
        expect(mode_of(out) == 0600 && mode_of(out.parent_path()) == 0700, "the file is 0600 in a 0700 directory");
        auto index = read_output_index(out);
        size_t next = 0;
        bool contiguous = !index.empty();
        for (const auto& c : index) {
            contiguous = contiguous && c.stream == 'o' && c.offset == next;
            next = c.offset + c.length;
        }
        expect(contiguous && next == full.size(), "the index covers it chunk by chunk: " + std::to_string(index.size()) + " chunks");
        expect(full_output_path(sess, "call_1") == out.parent_path() / "call_1-2.out" && full_output_path(sess, "a/../b c").filename() == "a____b_c.out",
               "a reused call id gets a suffix, and an odd one is made a plain name");
        expect(resolve_full_output(sess, *j) == out, "a reader finds it from the record");
        expect(resolve_full_output(sess, {{"path", "../../etc/passwd"}}).empty() && resolve_full_output(sess, {{"path", "20260101-120000-tui-1.d/../x.out"}}).empty(),
               "a record cannot point a reader anywhere but <id>.d/<name>.out");
    }

    section("past full_output_max_mb: the head, a note and the tail");
    {
        fs::path out = ws / "home" / "20260101-120000-tui-1.d" / "big.out";
        std::string full = numbers(1, 600000).substr(0, 3 * 1024 * 1024);
        FullOutputWriter w(out, 1 << 20);
        for (size_t i = 0; i < full.size(); i += 8192) w.add(OutputStream::Stdout, std::string_view(full).substr(i, 8192));
        auto j = w.finish(out.parent_path().parent_path());
        std::string kept = slurp(out);
        size_t head = 768 * 1024, dropped = j ? (*j)["dropped"].get<size_t>() : 0;
        std::string note = "\n... [" + std::to_string(dropped) + " bytes dropped here: the output was over full_output_max_mb] ...\n";
        size_t tail = kept.size() - head - note.size();
        expect(j && kept.compare(0, head, full, 0, head) == 0 && kept.compare(head, note.size(), note) == 0 && kept.compare(head + note.size(), tail, full, full.size() - tail, tail) == 0,
               "3 MiB under a 1 MiB cap: the first 768 KiB, a note saying how much was dropped, then the end");
        expect(tail >= 256 * 1024 && head + dropped + tail == full.size() && (*j)["bytes"] == kept.size(), "every byte is either kept or counted as dropped");
        bool noted = false;
        for (const auto& c : read_output_index(out)) noted = noted || (c.stream == 'm' && c.offset == head && c.length == note.size());
        expect(noted, "the note is MAIC's own chunk (m) in the index");
    }

    section("the screen and the replay, from the index");
    {
        fs::path out = ws / "home" / "20260101-120000-tui-1.d" / "progress.out";
        FullOutputWriter w(out, 64 << 20);
        std::string dots = std::string(33000, '.') + "\n";
        std::vector<std::pair<OutputStream, std::string>> steps = {{OutputStream::Stdout, dots}, {OutputStream::Stderr, "warn\n"}, {OutputStream::Stdout, "progress 10%\r"},
                                                                    {OutputStream::Stdout, "progress 60%\r"}, {OutputStream::Stdout, "progress 100%\r\n"}};
        for (size_t i = 0; i < steps.size(); ++i) {
            if (i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            w.add(steps[i].first, steps[i].second);
        }
        expect(w.finish(out.parent_path().parent_path()).has_value(), "kept once stdout passed the cap");
        auto index = read_output_index(out);
        bool timed = index.size() == steps.size();
        for (size_t i = 0; timed && i < index.size(); ++i) {
            timed = index[i].stream == (steps[i].first == OutputStream::Stdout ? 'o' : 'e') && index[i].length == steps[i].second.size() &&
                    std::abs(index[i].ms - static_cast<long>(i) * 200) < 120;
        }
        expect(timed, "the index has each chunk's stream, place, length and time since the call started");
        expect(full_output_screen(out) == dots + "warn\nprogress 100%\n", "the screen: carriage returns start their line over, a CRLF is a newline");
        expect(full_output_screen(out, 20) == "[" + std::to_string(dots.size() + 5 + 14 - 20) + " bytes before this are left out here]\n\nwarn\nprogress 100%\n",
               "and a capped screen keeps the end");
        std::vector<std::tuple<char, std::string, long>> played;
        auto t0 = Clock::now();
        replay_full_output(out, [&](char s, std::string_view bytes) {
            played.emplace_back(s, std::string(bytes), std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
        });
        bool replayed = played.size() == steps.size();
        for (size_t i = 0; replayed && i < played.size(); ++i) {
            replayed = std::get<0>(played[i]) == (steps[i].first == OutputStream::Stdout ? 'o' : 'e') && std::get<1>(played[i]) == steps[i].second &&
                       std::abs(std::get<2>(played[i]) - index[i].ms) < 100;
        }
        expect(replayed, "the replay: stdout and stderr interleaved as they were, each progress redraw its own write, on time");
        t0 = Clock::now();
        replay_full_output(out, [](char, std::string_view) {}, 4);
        expect(Clock::now() - t0 < std::chrono::milliseconds(500), "speed divides the waits");
    }

    fs::remove_all(ws);
    return finish();
}
