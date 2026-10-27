#include "json.h"
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace mini {

static const Json kNullJson;

size_t Json::size() const
{
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const Json& Json::at(size_t i) const
{
    if (type_ == Type::Array && i < arr_.size()) return arr_[i];
    return kNullJson;
}

static const Json& findValue(const std::vector<std::pair<std::string, Json>>& map, const char* key)
{
    for (const auto& kv : map) {
        if (kv.first == key) return kv.second;
    }
    return kNullJson;
}

const Json& Json::operator[](const char* key) const
{
    return findValue(obj_, key);
}

bool Json::has(const char* key) const
{
    for (const auto& kv : obj_) {
        if (kv.first == key) return true;
    }
    return false;
}

std::string Json::asString() const
{
    if (type_ == Type::String) return str_;
    return std::string();
}

double Json::asDouble() const
{
    if (type_ == Type::Number) return num_;
    if (type_ == Type::String) return atof(str_.c_str());
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return 0.0;
}

int64_t Json::asInt64() const
{
    return static_cast<int64_t>(asDouble());
}

int Json::asInt(int def) const
{
    if (type_ == Type::Number) return static_cast<int>(num_);
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) {
        if (str_.empty()) return def;
        return atoi(str_.c_str());
    }
    return def;
}

bool Json::asBool(bool def) const
{
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return num_ != 0;
    if (type_ == Type::String) return str_ == "1" || str_ == "true";
    return def;
}

class Parser {
public:
    Parser(const char* p, size_t n) : p_(p), end_(p + n) {}

    bool parse(Json& out)
    {
        skipSpace();
        if (!parseValue(out)) return false;
        skipSpace();
        return true;
    }

private:
    const char* p_;
    const char* end_;

    void skipSpace()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }

    bool eat(char c)
    {
        if (p_ < end_ && *p_ == c) { ++p_; return true; }
        return false;
    }

    bool parseValue(Json& out)
    {
        if (p_ >= end_) return false;
        char c = *p_;
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') { out.type_ = Json::Type::String; return parseString(out.str_); }
        if (c == 't') {
            if (end_ - p_ >= 4 && memcmp(p_, "true", 4) == 0) { p_ += 4; out.type_ = Json::Type::Bool; out.bool_ = true; return true; }
            return false;
        }
        if (c == 'f') {
            if (end_ - p_ >= 5 && memcmp(p_, "false", 5) == 0) { p_ += 5; out.type_ = Json::Type::Bool; out.bool_ = false; return true; }
            return false;
        }
        if (c == 'n') {
            if (end_ - p_ >= 4 && memcmp(p_, "null", 4) == 0) { p_ += 4; out.type_ = Json::Type::Null; return true; }
            return false;
        }
        return parseNumber(out);
    }

    bool parseNumber(Json& out)
    {
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
        bool any = false;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-' || *p_ == '+')) {
            any = true;
            ++p_;
        }
        if (!any) return false;
        std::string buf(start, p_);
        out.type_ = Json::Type::Number;
        out.num_ = atof(buf.c_str());
        return true;
    }

    static void appendUtf8(std::string& s, unsigned int cp)
    {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(unsigned int& v)
    {
        if (end_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = p_[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = v * 16 + d;
        }
        p_ += 4;
        return true;
    }

    bool parseString(std::string& out)
    {
        if (!eat('"')) return false;
        out.clear();
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
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
                unsigned int cp = 0;
                if (!parseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    const char* save = p_;
                    p_ += 2;
                    unsigned int lo = 0;
                    if (parseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p_ = save;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parseArray(Json& out)
    {
        if (!eat('[')) return false;
        out.type_ = Json::Type::Array;
        out.arr_.clear();
        skipSpace();
        if (eat(']')) return true;
        while (true) {
            Json v;
            skipSpace();
            if (!parseValue(v)) return false;
            out.arr_.push_back(std::move(v));
            skipSpace();
            if (eat(',')) { skipSpace(); continue; }
            if (eat(']')) return true;
            return false;
        }
    }

    bool parseObject(Json& out)
    {
        if (!eat('{')) return false;
        out.type_ = Json::Type::Object;
        out.obj_.clear();
        skipSpace();
        if (eat('}')) return true;
        while (true) {
            skipSpace();
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (!eat(':')) return false;
            skipSpace();
            Json v;
            if (!parseValue(v)) return false;
            out.obj_.emplace_back(std::move(key), std::move(v));
            skipSpace();
            if (eat(',')) { skipSpace(); continue; }
            if (eat('}')) return true;
            return false;
        }
    }
};

bool Json::Parse(const std::string& text, Json& out)
{
    // 跳过 UTF-8 BOM
    const char* p = text.data();
    size_t n = text.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    Parser parser(p, n);
    out = Json();
    return parser.parse(out);
}

} // namespace mini
