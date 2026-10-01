#pragma once

#include <string>
#include <vector>

namespace maic {

// Markdown features a span can carry; a span may have several (bold text inside a quote, ...).
enum MdFlag : unsigned {
    MdNone = 0,
    MdHeading = 1u << 0,
    MdBold = 1u << 1,
    MdItalic = 1u << 2,
    MdCode = 1u << 3,       // inline code
    MdCodeBlock = 1u << 4,  // inside a fenced block
    MdLink = 1u << 5,       // link text
    MdUrl = 1u << 6,        // link target, code fence line, other dim syntax
    MdQuote = 1u << 7,
    MdBullet = 1u << 8,     // list marker
    MdRule = 1u << 9,
    // from the nvim highlighter (cli/src/highlight.cpp): treesitter captures mapped to the hl_* styles
    HlKeyword = 1u << 10,
    HlString = 1u << 11,
    HlComment = 1u << 12,
    HlHeading = 1u << 13,
    HlCode = 1u << 14,
    // a diff shown in the conversation window or the approval box
    DiffAdd = 1u << 15,
    DiffDel = 1u << 16,
    DiffHunk = 1u << 17,    // @@ lines and file headers
};

struct Span {
    std::string text;
    unsigned flags = MdNone;
};

using StyledLine = std::vector<Span>;

// Splits text into lines of styled spans. Markup characters stay in the output (like an editor with
// conceal off) so nothing is lost when copying; only their style changes.
std::vector<StyledLine> markdown_lines(const std::string& text);

// The same text with no markup interpretation.
std::vector<StyledLine> plain_lines(const std::string& text);

// Number of terminal columns a line occupies (code points; wide characters count as one column here).
size_t line_width(const StyledLine& line);

// Soft-wraps at `width` columns, breaking at spaces when one is reasonably close. Never returns no lines.
std::vector<StyledLine> wrap_line(const StyledLine& line, size_t width);

std::string line_text(const StyledLine& line);

}  // namespace maic
