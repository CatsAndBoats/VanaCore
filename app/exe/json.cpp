#include "json.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace vcapp::json {

namespace {

const Value kNull;

class Reader {
public:
    explicit Reader(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool document(Value& out)
    {
        if (!value(out, 0))
            return false;
        space();
        return p_ == end_;
    }

private:
    void space()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
            ++p_;
    }

    bool word(const char* w)
    {
        const std::size_t n = std::strlen(w);
        if (static_cast<std::size_t>(end_ - p_) < n || std::memcmp(p_, w, n) != 0)
            return false;
        p_ += n;
        return true;
    }

    static void utf8(std::string& out, unsigned c)
    {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool hex4(unsigned& c)
    {
        if (end_ - p_ < 4)
            return false;
        c = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            const char h = *p_;
            c <<= 4;
            if (h >= '0' && h <= '9')
                c |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
                c |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                c |= static_cast<unsigned>(h - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool escape(std::string& out)
    {
        if (p_ >= end_)
            return false;
        const char e = *p_++;
        switch (e) {
        case '"': out += '"'; return true;
        case '\\': out += '\\'; return true;
        case '/': out += '/'; return true;
        case 'b': out += '\b'; return true;
        case 'f': out += '\f'; return true;
        case 'n': out += '\n'; return true;
        case 'r': out += '\r'; return true;
        case 't': out += '\t'; return true;
        case 'u': {
            unsigned c = 0;
            if (!hex4(c))
                return false;
            // a surrogate pair: the second half follows as its own \u
            if (c >= 0xD800 && c < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                p_ += 2;
                unsigned low = 0;
                if (!hex4(low))
                    return false;
                c = low >= 0xDC00 && low < 0xE000 ? 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
            }
            utf8(out, c);
            return true;
        }
        default:
            return false;
        }
    }

    bool string(std::string& out)
    {
        if (p_ >= end_ || *p_ != '"')
            return false;
        ++p_;
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"')
                return true;
            if (c == '\\') {
                if (!escape(out))
                    return false;
            } else {
                out += c;
            }
        }
        return false;
    }

    bool number(Value& out)
    {
        char* stop = nullptr;
        const std::string text(p_, static_cast<std::size_t>(std::min<std::ptrdiff_t>(end_ - p_, 64)));
        out.number = std::strtod(text.c_str(), &stop);
        if (stop == text.c_str())
            return false;
        p_ += stop - text.c_str();
        out.type = Value::Type::Number;
        return true;
    }

    bool array(Value& out, int depth)
    {
        ++p_;
        out.type = Value::Type::Array;
        space();
        if (p_ < end_ && *p_ == ']')
            return ++p_, true;
        while (true) {
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1))
                return false;
            space();
            if (p_ < end_ && *p_ == ',') {
                ++p_;
                continue;
            }
            return p_ < end_ && *p_++ == ']';
        }
    }

    bool object(Value& out, int depth)
    {
        ++p_;
        out.type = Value::Type::Object;
        space();
        if (p_ < end_ && *p_ == '}')
            return ++p_, true;
        while (true) {
            space();
            std::string key;
            if (!string(key))
                return false;
            space();
            if (p_ >= end_ || *p_++ != ':')
                return false;
            out.members.emplace_back(std::move(key), Value{});
            if (!value(out.members.back().second, depth + 1))
                return false;
            space();
            if (p_ < end_ && *p_ == ',') {
                ++p_;
                continue;
            }
            return p_ < end_ && *p_++ == '}';
        }
    }

    bool value(Value& out, int depth)
    {
        if (depth > 64)
            return false;
        space();
        if (p_ >= end_)
            return false;
        switch (*p_) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': out.type = Value::Type::String; return string(out.string);
        case 't': out.type = Value::Type::Bool; out.boolean = true; return word("true");
        case 'f': out.type = Value::Type::Bool; return word("false");
        case 'n': return word("null");
        default: return number(out);
        }
    }

    const char* p_;
    const char* end_;
};

} // namespace

const Value& Value::operator[](const char* key) const
{
    if (type == Type::Object)
        for (const auto& [k, v] : members)
            if (k == key)
                return v;
    return kNull;
}

bool parse(const std::string& text, Value& out)
{
    out = Value{};
    return Reader(text).document(out);
}

} // namespace vcapp::json
