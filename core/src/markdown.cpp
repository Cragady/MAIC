#include "maic/markdown.hpp"

#include <sstream>

namespace maic {

namespace {

size_t next_cp(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t cp_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

void push(StyledLine& line, std::string text, unsigned flags) {
    if (text.empty()) return;
    if (!line.empty() && line.back().flags == flags) {
        line.back().text += text;
    } else {
        line.push_back({std::move(text), flags});
    }
}

bool starts(const std::string& s, size_t i, const char* what) {
    return s.compare(i, std::char_traits<char>::length(what), what) == 0;
}

// Inline markup: `code`, **bold**, *italic* / _italic_, [text](url), raw URLs.
void inline_spans(const std::string& s, unsigned base, StyledLine& out) {
    size_t i = 0;
    std::string plain;
    auto flush = [&] {
        push(out, plain, base);
        plain.clear();
    };
    auto is_word = [&](size_t k) { return k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) || static_cast<unsigned char>(s[k]) >= 0x80); };
    while (i < s.size()) {
        if (s[i] == '`') {
            size_t close = s.find('`', i + 1);
            if (close != std::string::npos) {
                flush();
                push(out, s.substr(i, close - i + 1), base | MdCode);
                i = close + 1;
                continue;
            }
        }
        if (starts(s, i, "**") || starts(s, i, "__")) {
            std::string mark = s.substr(i, 2);
            size_t close = s.find(mark, i + 2);
            if (close != std::string::npos && close > i + 2) {
                flush();
                push(out, mark, base | MdUrl);
                inline_spans(s.substr(i + 2, close - i - 2), base | MdBold, out);
                push(out, mark, base | MdUrl);
                i = close + 2;
                continue;
            }
        }
        if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] != ' ' && (s[i] == '*' || !is_word(i == 0 ? s.size() : i - 1))) {
            size_t close = s.find(s[i], i + 1);
            while (close != std::string::npos && (s[close - 1] == ' ' || (s[i] == '_' && is_word(close + 1)))) close = s.find(s[i], close + 1);
            if (close != std::string::npos && close > i + 1) {
                flush();
                push(out, std::string(1, s[i]), base | MdUrl);
                inline_spans(s.substr(i + 1, close - i - 1), base | MdItalic, out);
                push(out, std::string(1, s[i]), base | MdUrl);
                i = close + 1;
                continue;
            }
        }
        if (s[i] == '[') {
            size_t close = s.find("](", i);
            size_t end = close == std::string::npos ? std::string::npos : s.find(')', close);
            if (end != std::string::npos) {
                flush();
                push(out, "[", base | MdUrl);
                inline_spans(s.substr(i + 1, close - i - 1), base | MdLink, out);
                push(out, "](", base | MdUrl);
                push(out, s.substr(close + 2, end - close - 2), base | MdUrl);
                push(out, ")", base | MdUrl);
                i = end + 1;
                continue;
            }
        }
        if (starts(s, i, "http://") || starts(s, i, "https://")) {
            size_t end = i;
            while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])) && s[end] != ')' && s[end] != '>') ++end;
            flush();
            push(out, s.substr(i, end - i), base | MdLink);
            i = end;
            continue;
        }
        size_t n = next_cp(s, i);
        plain += s.substr(i, n - i);
        i = n;
    }
    flush();
}

}  // namespace

std::vector<StyledLine> plain_lines(const std::string& text) {
    std::vector<StyledLine> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        StyledLine l;
        push(l, line, MdNone);
        out.push_back(l);
    }
    if (out.empty() || (!text.empty() && text.back() == '\n')) out.push_back({});
    return out;
}

std::vector<StyledLine> markdown_lines(const std::string& text) {
    std::vector<StyledLine> out;
    bool in_fence = false;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        StyledLine l;
        size_t indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) indent = line.size();
        std::string body = line.substr(indent);
        if (starts(body, 0, "```") || starts(body, 0, "~~~")) {
            in_fence = !in_fence;
            push(l, line, MdCodeBlock | MdUrl);
            out.push_back(l);
            continue;
        }
        if (in_fence) {
            push(l, line, MdCodeBlock);
            out.push_back(l);
            continue;
        }
        if (body.size() >= 3 && (body.find_first_not_of('-') == std::string::npos || body.find_first_not_of('*') == std::string::npos ||
                                 body.find_first_not_of('_') == std::string::npos)) {
            push(l, line, MdRule);
            out.push_back(l);
            continue;
        }
        unsigned base = MdNone;
        std::string prefix = line.substr(0, indent);
        if (body[0] == '#') {
            size_t level = body.find_first_not_of('#');
            if (level != std::string::npos && level <= 6 && body[level] == ' ') {
                push(l, prefix + body.substr(0, level + 1), MdHeading | MdUrl);
                inline_spans(body.substr(level + 1), MdHeading, l);
                out.push_back(l);
                continue;
            }
        }
        if (body[0] == '>') {
            size_t after = body.size() > 1 && body[1] == ' ' ? 2 : 1;
            push(l, prefix + body.substr(0, after), MdQuote | MdUrl);
            inline_spans(body.substr(after), MdQuote, l);
            out.push_back(l);
            continue;
        }
        if ((body[0] == '-' || body[0] == '*' || body[0] == '+') && body.size() > 1 && body[1] == ' ') {
            push(l, prefix + body.substr(0, 2), MdBullet);
            inline_spans(body.substr(2), base, l);
            out.push_back(l);
            continue;
        }
        size_t digits = body.find_first_not_of("0123456789");
        if (digits != std::string::npos && digits > 0 && digits + 1 < body.size() + 1 && (body[digits] == '.' || body[digits] == ')') &&
            digits + 1 < body.size() && body[digits + 1] == ' ') {
            push(l, prefix + body.substr(0, digits + 2), MdBullet);
            inline_spans(body.substr(digits + 2), base, l);
            out.push_back(l);
            continue;
        }
        push(l, prefix, base);
        inline_spans(body, base, l);
        out.push_back(l);
    }
    if (out.empty() || (!text.empty() && text.back() == '\n')) out.push_back({});
    return out;
}

size_t line_width(const StyledLine& line) {
    size_t n = 0;
    for (const auto& s : line) n += cp_len(s.text);
    return n;
}

std::string line_text(const StyledLine& line) {
    std::string out;
    for (const auto& s : line) out += s.text;
    return out;
}

std::vector<StyledLine> wrap_line(const StyledLine& line, size_t width) {
    width = std::max<size_t>(width, 8);
    std::vector<StyledLine> out;
    StyledLine current;
    size_t col = 0;
    for (const auto& span : line) {
        size_t i = 0;
        while (i < span.text.size()) {
            size_t n = next_cp(span.text, i);
            if (col == width) {
                // Break at the last space of this row when it isn't too far back.
                size_t break_at = std::string::npos;
                size_t seen = 0;
                for (size_t si = 0; si < current.size(); ++si) {
                    size_t sp = current[si].text.rfind(' ');
                    if (sp != std::string::npos) break_at = si, seen = sp;
                }
                if (break_at != std::string::npos) {
                    size_t before = 0;
                    for (size_t si = 0; si < break_at; ++si) before += cp_len(current[si].text);
                    before += cp_len(current[break_at].text.substr(0, seen));
                    if (before >= width / 2) {
                        StyledLine rest;
                        Span tail{current[break_at].text.substr(seen + 1), current[break_at].flags};
                        current[break_at].text.erase(seen);
                        push(rest, tail.text, tail.flags);
                        for (size_t si = break_at + 1; si < current.size(); ++si) push(rest, current[si].text, current[si].flags);
                        current.resize(break_at + 1);
                        if (current.back().text.empty()) current.pop_back();
                        out.push_back(current);
                        current = rest;
                        col = line_width(current);
                        continue;
                    }
                }
                out.push_back(current);
                current.clear();
                col = 0;
            }
            push(current, span.text.substr(i, n - i), span.flags);
            ++col;
            i = n;
        }
    }
    out.push_back(current);
    return out;
}

}  // namespace maic
