#pragma once

#include "maid/sandbox.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maid {

// How every reader introduces a kept output, so it is never taken for what the model was given.
inline constexpr const char* kFullOutputLabel = "full output, display only: the model saw the capped result";

// A tool call's whole output, kept beside its session once it outgrows what the model is given (docs/sessions.md,
// Full output). `out` (<id>.d/<call>.out) gets every byte of stdout and stderr in the order they were read, and
// `<call>.idx` beside it one line per chunk, `STREAM OFFSET LENGTH MS`: o (stdout), e (stderr) or m (MAID's own
// note), where the chunk starts in the .out, its length, and milliseconds since the writer was made. Nothing is
// written until either stream passes the model's cap; from then on every chunk is written as it arrives (files
// 0600, the directory 0700). Past `max_bytes` the file keeps its head, a note saying how much was dropped and
// the last quarter (at most 8 MiB, held in memory until the end).
class FullOutputWriter {
public:
    FullOutputWriter(std::filesystem::path out, size_t max_bytes);
    ~FullOutputWriter();
    FullOutputWriter(const FullOutputWriter&) = delete;
    FullOutputWriter& operator=(const FullOutputWriter&) = delete;

    void add(OutputStream stream, std::string_view bytes);  // OutputTaps::on_read
    // Closes the files. The record's `full_output`, with `path` relative to `base`, or nullopt when the output
    // stayed under the cap (nothing was written) or the files could not be made (error() says why).
    std::optional<nlohmann::json> finish(const std::filesystem::path& base);
    const std::string& error() const { return error_; }

private:
    struct Chunk {
        char stream;
        std::string data;
        long ms;
    };
    void open_files();
    void put(char stream, std::string_view data, long ms);  // to the head on disk, or the tail in memory
    void write_chunk(char stream, std::string_view data, long ms);

    std::filesystem::path out_;
    size_t head_limit_, tail_limit_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    size_t totals_[2] = {0, 0};
    std::vector<Chunk> early_;  // before the cap is passed
    std::deque<Chunk> tail_;    // past head_limit_
    size_t tail_bytes_ = 0, dropped_ = 0, written_ = 0;
    int out_fd_ = -1, idx_fd_ = -1;
    bool opened_ = false;
    std::string error_;
};

// A free name for `call_id`'s kept output in the session's directory for them, <dir>/<id>.d/<call id>.out (a
// suffix when the model reused an id). Nothing is created.
std::filesystem::path full_output_path(const std::filesystem::path& session_file, const std::string& call_id);

// The file a record's `full_output` names, read from `session_file`: beside it, or beside the session the path's
// <id>.d names (a fork reading its parent's records). Empty when it is not there.
std::filesystem::path resolve_full_output(const std::filesystem::path& session_file, const nlohmann::json& full_output);

struct OutputChunk {
    char stream;  // 'o', 'e' or 'm'
    size_t offset, length;
    long ms;
};
// The chunks of a kept output from its .idx, in order; one chunk of stdout for the whole file when there is no index.
std::vector<OutputChunk> read_output_index(const std::filesystem::path& out);

// The output as a terminal would have left it: the chunks in order, a carriage return starting its line over and
// a CRLF a newline. At most `max_bytes` of the result, from the end, after a line saying how much is left out.
std::string full_output_screen(const std::filesystem::path& out, size_t max_bytes = ~size_t(0));

// Plays the output back as it happened: each chunk to `write` at its time since the start, divided by `speed`.
void replay_full_output(const std::filesystem::path& out, const std::function<void(char stream, std::string_view bytes)>& write, double speed = 1);

}  // namespace maid
