#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "json.h"

namespace json {

const Value* Value::Get(const std::string& key) const
{
    if (type != Type::Object)
        return nullptr;
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

std::string Value::GetString(const std::string& key, const std::string& fallback) const
{
    const Value* v = Get(key);
    return v && v->type == Type::String ? v->s : fallback;
}

double Value::GetNumber(const std::string& key, double fallback) const
{
    const Value* v = Get(key);
    return v && v->type == Type::Number ? v->n : fallback;
}

bool Value::GetBool(const std::string& key, bool fallback) const
{
    const Value* v = Get(key);
    return v && v->type == Type::Bool ? v->b : fallback;
}

namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    std::string error;

    void Skip()
    {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n'))
            ++i;
    }

    bool Fail(const char* what)
    {
        if (error.empty())
            error = std::string(what) + " at " + std::to_string(i);
        return false;
    }

    bool Literal(const char* word)
    {
        size_t n = strlen(word);
        if (t.compare(i, n, word) != 0)
            return Fail("bad literal");
        i += n;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool String(std::string& out)
    {
        if (t[i] != '"')
            return Fail("expected string");
        ++i;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size())
                return Fail("bad escape");
            char e = t[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > t.size())
                    return Fail("bad \\u");
                unsigned cp = static_cast<unsigned>(strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                i += 4;
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (i >= t.size())
            return Fail("unterminated string");
        ++i;
        return true;
    }

    bool Val(Value& v, int depth)
    {
        if (depth > 64)
            return Fail("too deep");
        Skip();
        if (i >= t.size())
            return Fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            v = Value::Obj();
            ++i;
            Skip();
            if (i < t.size() && t[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                Skip();
                std::string key;
                if (!String(key))
                    return false;
                Skip();
                if (i >= t.size() || t[i] != ':')
                    return Fail("expected :");
                ++i;
                Value item;
                if (!Val(item, depth + 1))
                    return false;
                v.o[key] = std::move(item);
                Skip();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    ++i;
                    return true;
                }
                return Fail("expected , or }");
            }
        }
        if (c == '[') {
            v = Value::Arr();
            ++i;
            Skip();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                Value item;
                if (!Val(item, depth + 1))
                    return false;
                v.a.push_back(std::move(item));
                Skip();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    ++i;
                    return true;
                }
                return Fail("expected , or ]");
            }
        }
        if (c == '"') {
            v = Value::Str("");
            return String(v.s);
        }
        if (c == 't') {
            v = Value::Bool(true);
            return Literal("true");
        }
        if (c == 'f') {
            v = Value::Bool(false);
            return Literal("false");
        }
        if (c == 'n') {
            v = Value::Null();
            return Literal("null");
        }
        char* end = nullptr;
        double n = strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i)
            return Fail("unexpected character");
        i = static_cast<size_t>(end - t.c_str());
        v = Value::Num(n);
        return true;
    }
};

void WriteString(const std::string& s, std::string& out)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void WriteValue(const Value& v, std::string& out)
{
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.b ? "true" : "false"; break;
    case Value::Type::Number: {
        char buf[40];
        if (v.n == static_cast<double>(static_cast<long long>(v.n)) && v.n < 9e15 && v.n > -9e15)
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.n));
        else
            snprintf(buf, sizeof(buf), "%.6g", v.n);
        out += buf;
        break;
    }
    case Value::Type::String: WriteString(v.s, out); break;
    case Value::Type::Array: {
        out += '[';
        for (size_t k = 0; k < v.a.size(); ++k) {
            if (k)
                out += ',';
            WriteValue(v.a[k], out);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : v.o) {
            if (!first)
                out += ',';
            first = false;
            WriteString(key, out);
            out += ':';
            WriteValue(item, out);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

bool Parse(const std::string& text, Value& out, std::string& error)
{
    Parser p{text};
    if (!p.Val(out, 0)) {
        error = p.error;
        return false;
    }
    p.Skip();
    if (p.i != text.size()) {
        error = "trailing characters";
        return false;
    }
    return true;
}

std::string Write(const Value& v)
{
    std::string out;
    WriteValue(v, out);
    return out;
}

} // namespace json
