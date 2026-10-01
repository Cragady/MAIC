#include "msgpack.hpp"

#include <cstring>
#include <stdexcept>

namespace maic::msgpack {

Value Value::boolean(bool v) {
    Value out;
    out.kind = Kind::Bool;
    out.b = v;
    return out;
}

Value Value::integer(int64_t v) {
    Value out;
    out.kind = Kind::Int;
    out.i = v;
    return out;
}

Value Value::str(std::string v) {
    Value out;
    out.kind = Kind::Str;
    out.s = std::move(v);
    return out;
}

Value Value::arr(std::vector<Value> v) {
    Value out;
    out.kind = Kind::Array;
    out.array = std::move(v);
    return out;
}

namespace {

void put_be(std::string& out, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

void put_count(std::string& out, size_t n, uint8_t fix_base, uint8_t fix_max, uint8_t tag8, uint8_t tag16, uint8_t tag32) {
    if (n <= fix_max) out += static_cast<char>(fix_base | n);
    else if (tag8 && n <= 0xff) out += static_cast<char>(tag8), put_be(out, n, 1);
    else if (n <= 0xffff) out += static_cast<char>(tag16), put_be(out, n, 2);
    else out += static_cast<char>(tag32), put_be(out, n, 4);
}

void encode_into(std::string& out, const Value& v) {
    switch (v.kind) {
        case Value::Kind::Nil: out += '\xc0'; break;
        case Value::Kind::Bool: out += v.b ? '\xc3' : '\xc2'; break;
        case Value::Kind::Int:
            if (v.i >= 0 && v.i <= 0x7f) out += static_cast<char>(v.i);
            else if (v.i < 0 && v.i >= -32) out += static_cast<char>(v.i);
            else if (v.i >= 0 && v.i <= 0xff) out += '\xcc', put_be(out, static_cast<uint64_t>(v.i), 1);
            else if (v.i >= 0 && v.i <= 0xffff) out += '\xcd', put_be(out, static_cast<uint64_t>(v.i), 2);
            else if (v.i >= 0 && v.i <= 0xffffffffLL) out += '\xce', put_be(out, static_cast<uint64_t>(v.i), 4);
            else if (v.i >= 0) out += '\xcf', put_be(out, static_cast<uint64_t>(v.i), 8);
            else if (v.i >= -128) out += '\xd0', put_be(out, static_cast<uint64_t>(v.i), 1);
            else if (v.i >= -32768) out += '\xd1', put_be(out, static_cast<uint64_t>(v.i), 2);
            else if (v.i >= -2147483648LL) out += '\xd2', put_be(out, static_cast<uint64_t>(v.i), 4);
            else out += '\xd3', put_be(out, static_cast<uint64_t>(v.i), 8);
            break;
        case Value::Kind::Float: {
            uint64_t bits;
            std::memcpy(&bits, &v.f, 8);
            out += '\xcb';
            put_be(out, bits, 8);
            break;
        }
        case Value::Kind::Str:
            put_count(out, v.s.size(), 0xa0, 31, 0xd9, 0xda, 0xdb);
            out += v.s;
            break;
        case Value::Kind::Array:
            put_count(out, v.array.size(), 0x90, 15, 0, 0xdc, 0xdd);
            for (const auto& e : v.array) encode_into(out, e);
            break;
        case Value::Kind::Map:
            put_count(out, v.map.size(), 0x80, 15, 0, 0xde, 0xdf);
            for (const auto& [k, val] : v.map) encode_into(out, k), encode_into(out, val);
            break;
        case Value::Kind::Ext:
            put_count(out, v.s.size(), 0, 0, 0xc7, 0xc8, 0xc9);  // never the fixext forms; nvim reads both
            out += static_cast<char>(v.ext_type);
            out += v.s;
            break;
    }
}

struct Reader {
    const std::string& s;
    size_t pos;
    bool ok = true;  // false: ran out of bytes

    bool need(size_t n) {
        if (pos + n > s.size()) return ok = false;
        return true;
    }
    uint64_t be(int bytes) {
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<unsigned char>(s[pos++]);
        return v;
    }
    bool take(size_t n, std::string& out) {
        if (!need(n)) return false;
        out = s.substr(pos, n);
        pos += n;
        return true;
    }
    bool value(Value& out);
    bool sequence(size_t n, Value& out, bool is_map) {
        out.kind = is_map ? Value::Kind::Map : Value::Kind::Array;
        for (size_t i = 0; i < n; ++i) {
            Value k, v;
            if (!value(is_map ? k : v)) return false;
            if (is_map && !value(v)) return false;
            if (is_map) out.map.push_back({std::move(k), std::move(v)});
            else out.array.push_back(std::move(v));
        }
        return true;
    }
    bool ext(size_t n, Value& out) {
        if (!need(1 + n)) return false;
        out.kind = Value::Kind::Ext;
        out.ext_type = static_cast<int8_t>(s[pos++]);
        out.s = s.substr(pos, n);
        pos += n;
        Value handle;
        size_t p = 0;
        if (decode(out.s, p, handle) && handle.is_int() && p == out.s.size()) out.i = handle.i;
        return true;
    }
};

bool Reader::value(Value& out) {
    out = Value{};
    if (!need(1)) return false;
    unsigned char c = static_cast<unsigned char>(s[pos++]);
    if (c <= 0x7f) return out = Value::integer(c), true;
    if (c >= 0xe0) return out = Value::integer(static_cast<int8_t>(c)), true;
    if (c >= 0xa0 && c <= 0xbf) return out.kind = Value::Kind::Str, take(c & 0x1f, out.s);
    if (c >= 0x90 && c <= 0x9f) return sequence(c & 0x0f, out, false);
    if (c >= 0x80 && c <= 0x8f) return sequence(c & 0x0f, out, true);
    switch (c) {
        case 0xc0: return true;
        case 0xc2: return out = Value::boolean(false), true;
        case 0xc3: return out = Value::boolean(true), true;
        case 0xc4: case 0xd9: if (!need(1)) return false; return out.kind = Value::Kind::Str, take(be(1), out.s);
        case 0xc5: case 0xda: if (!need(2)) return false; return out.kind = Value::Kind::Str, take(be(2), out.s);
        case 0xc6: case 0xdb: if (!need(4)) return false; return out.kind = Value::Kind::Str, take(be(4), out.s);
        case 0xc7: if (!need(1)) return false; return ext(be(1), out);
        case 0xc8: if (!need(2)) return false; return ext(be(2), out);
        case 0xc9: if (!need(4)) return false; return ext(be(4), out);
        case 0xca: {
            if (!need(4)) return false;
            uint32_t bits = static_cast<uint32_t>(be(4));
            float f;
            std::memcpy(&f, &bits, 4);
            out.kind = Value::Kind::Float;
            out.f = f;
            return true;
        }
        case 0xcb: {
            if (!need(8)) return false;
            uint64_t bits = be(8);
            out.kind = Value::Kind::Float;
            std::memcpy(&out.f, &bits, 8);
            return true;
        }
        case 0xcc: if (!need(1)) return false; return out = Value::integer(static_cast<int64_t>(be(1))), true;
        case 0xcd: if (!need(2)) return false; return out = Value::integer(static_cast<int64_t>(be(2))), true;
        case 0xce: if (!need(4)) return false; return out = Value::integer(static_cast<int64_t>(be(4))), true;
        case 0xcf: if (!need(8)) return false; return out = Value::integer(static_cast<int64_t>(be(8))), true;
        case 0xd0: if (!need(1)) return false; return out = Value::integer(static_cast<int8_t>(be(1))), true;
        case 0xd1: if (!need(2)) return false; return out = Value::integer(static_cast<int16_t>(be(2))), true;
        case 0xd2: if (!need(4)) return false; return out = Value::integer(static_cast<int32_t>(be(4))), true;
        case 0xd3: if (!need(8)) return false; return out = Value::integer(static_cast<int64_t>(be(8))), true;
        case 0xd4: return ext(1, out);
        case 0xd5: return ext(2, out);
        case 0xd6: return ext(4, out);
        case 0xd7: return ext(8, out);
        case 0xd8: return ext(16, out);
        case 0xdc: if (!need(2)) return false; return sequence(be(2), out, false);
        case 0xdd: if (!need(4)) return false; return sequence(be(4), out, false);
        case 0xde: if (!need(2)) return false; return sequence(be(2), out, true);
        case 0xdf: if (!need(4)) return false; return sequence(be(4), out, true);
        default: throw std::runtime_error("not msgpack: byte 0x" + std::to_string(c));
    }
}

}  // namespace

std::string encode(const Value& v) {
    std::string out;
    encode_into(out, v);
    return out;
}

bool decode(const std::string& bytes, size_t& pos, Value& out) {
    Reader r{bytes, pos};
    if (!r.value(out) || !r.ok) return false;
    pos = r.pos;
    return true;
}

}  // namespace maic::msgpack
