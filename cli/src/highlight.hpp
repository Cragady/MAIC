#pragma once

#include "maid/markdown.hpp"

#include <sys/types.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace maid {

// The input highlighted by an embedded nvim (`highlight = "nvim"`): one `nvim --embed --headless` child for
// the session, msgpack-rpc over its stdio, the input text set as a markdown buffer and treesitter's highlight
// captures read back as (row, byte range, capture name), mapped to the hl_* and md_* styles. When nvim is
// missing or fails, alive() goes false and error() says why; the caller keeps the built-in highlighter.
class NvimHighlighter {
public:
    // `nvim` is the program (a name on PATH or a path). `on_ready` runs on the reader thread when a reply
    // arrives after highlight() gave up waiting, so the UI can redraw and pick it up.
    explicit NvimHighlighter(std::string nvim = "nvim", std::function<void()> on_ready = {});
    ~NvimHighlighter();
    NvimHighlighter(const NvimHighlighter&) = delete;
    NvimHighlighter& operator=(const NvimHighlighter&) = delete;

    // The text's lines with the flags nvim's captures give, or nullopt when the reply is not in within `budget`
    // (a later call returns it) or nvim is gone. The same text twice is answered from the last reply.
    std::optional<std::vector<StyledLine>> highlight(const std::string& text, std::chrono::milliseconds budget);
    bool alive() const;
    std::string error() const;

private:
    void start();
    void read_loop();
    void fail(const std::string& why);  // under mu_

    std::string nvim_;
    std::function<void()> on_ready_;
    pid_t pid_ = -1;
    int to_nvim_ = -1, from_nvim_ = -1;
    std::thread reader_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool dead_ = false;
    std::string error_;
    uint32_t next_id_ = 1;
    uint32_t pending_id_ = 0;  // the request in flight; 0 when none
    std::string pending_text_;
    bool have_done_ = false;   // done_lines_ is the highlight of done_text_
    std::string done_text_;
    std::vector<StyledLine> done_lines_;
    int waiting_ = 0;          // highlight() calls blocked in wait
};

// A treesitter capture name ("markup.heading.1", "keyword.function", "string") to the flags it paints with;
// MdNone for the ones MAID has no style for.
unsigned capture_flags(const std::string& name);

}  // namespace maid
