#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal JSON for the bridge: parse requests, build responses. Numbers are doubles.
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    static Value Null() { return {}; }
    static Value Bool(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value Num(double v) { Value x; x.type = Type::Number; x.n = v; return x; }
    static Value Str(std::string v) { Value x; x.type = Type::String; x.s = std::move(v); return x; }
    static Value Arr() { Value x; x.type = Type::Array; return x; }
    static Value Obj() { Value x; x.type = Type::Object; return x; }

    Value& Set(const std::string& key, Value v) { type = Type::Object; o[key] = std::move(v); return *this; }
    Value& Push(Value v) { type = Type::Array; a.push_back(std::move(v)); return *this; }

    const Value* Get(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& fallback = "") const;
    double GetNumber(const std::string& key, double fallback = 0) const;
    bool GetBool(const std::string& key, bool fallback = false) const;
};

// Parses one JSON document; returns false (and sets error) on malformed input.
bool Parse(const std::string& text, Value& out, std::string& error);

// Compact single-line serialization.
std::string Write(const Value& v);

} // namespace json
