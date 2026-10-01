#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The part of msgpack that talking to nvim over msgpack-rpc needs: nil, booleans, integers, floats, strings
// (binary is read as a string), arrays, maps, and ext (nvim's Buffer / Window / Tabpage handles).
namespace maic::msgpack {

struct Value {
    enum class Kind { Nil, Bool, Int, Float, Str, Array, Map, Ext };
    Kind kind = Kind::Nil;
    bool b = false;
    int64_t i = 0;  // Int; for an Ext whose payload is an integer, that handle
    double f = 0;
    std::string s;  // Str, and an Ext's raw payload
    std::vector<Value> array;
    std::vector<std::pair<Value, Value>> map;
    int8_t ext_type = 0;

    static Value nil() { return {}; }
    static Value boolean(bool v);
    static Value integer(int64_t v);
    static Value str(std::string v);
    static Value arr(std::vector<Value> v);

    bool is_nil() const { return kind == Kind::Nil; }
    bool is_int() const { return kind == Kind::Int; }
    bool is_str() const { return kind == Kind::Str; }
    bool is_array() const { return kind == Kind::Array; }
};

std::string encode(const Value& v);

// Decodes one value starting at `pos` and moves `pos` past it. Returns false, leaving `pos` alone, when the
// bytes end before the value does (read more and retry). Throws std::runtime_error on a byte that is not msgpack.
bool decode(const std::string& bytes, size_t& pos, Value& out);

}  // namespace maic::msgpack
