#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vanacore::json {

Value& Value::operator[](std::string_view key)
{
    if (type != Type::Object) {
        *this = make_object();
    }
    for (auto& [name, value] : object)
        if (name == key)
            return value;
    object.emplace_back(std::string(key), Value());
    return object.back().second;
}

const Value* Value::find(std::string_view key) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto& [name, value] : object)
        if (name == key)
            return &value;
    return nullptr;
}

Value& Value::push(Value value)
{
    if (type != Type::Array)
        *this = make_array();
    array.push_back(std::move(value));
    return array.back();
}

std::string Value::get_string(std::string_view key, std::string fallback) const
{
    const Value* value = find(key);
    return value != nullptr && value->is_string() ? value->string : fallback;
}

double Value::get_number(std::string_view key, double fallback) const
{
    const Value* value = find(key);
    return value != nullptr && value->is_number() ? value->number : fallback;
}

bool Value::get_bool(std::string_view key, bool fallback) const
{
    const Value* value = find(key);
    return value != nullptr && value->is_bool() ? value->boolean : fallback;
}

namespace {

struct Reader {
    std::string_view text;
    size_t at = 0;
    std::string error;

    void space()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
            ++at;
    }

    bool fail(const char* what)
    {
        if (error.empty())
            error = std::string(what) + " at offset " + std::to_string(at);
        return false;
    }

    bool literal(std::string_view word)
    {
        if (text.substr(at, word.size()) != word)
            return false;
        at += word.size();
        return true;
    }

    static void utf8(std::string& out, unsigned code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(unsigned& code)
    {
        if (at + 4 > text.size())
            return fail("a short \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at++];
            code <<= 4;
            if (c >= '0' && c <= '9')
                code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                code |= static_cast<unsigned>(c - 'A' + 10);
            else
                return fail("a bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out)
    {
        ++at; // the opening quote
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size())
                break;
            const char e = text[at++];
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
                unsigned code = 0;
                if (!hex4(code))
                    return false;
                if (code >= 0xD800 && code < 0xDC00 && text.substr(at, 2) == "\\u") {
                    at += 2;
                    unsigned low = 0;
                    if (!hex4(low))
                        return false;
                    if (low >= 0xDC00 && low < 0xE000)
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    else
                        code = 0xFFFD;
                } else if (code >= 0xD800 && code < 0xE000) {
                    code = 0xFFFD;
                }
                utf8(out, code);
                break;
            }
            default:
                return fail("a bad escape");
            }
        }
        return fail("an unfinished string");
    }

    bool value(Value& out, int depth)
    {
        if (depth > 64)
            return fail("too deep");
        space();
        if (at >= text.size())
            return fail("nothing");
        const char c = text[at];
        if (c == '{') {
            ++at;
            out = Value::make_object();
            space();
            if (at < text.size() && text[at] == '}') {
                ++at;
                return true;
            }
            for (;;) {
                space();
                if (at >= text.size() || text[at] != '"')
                    return fail("a missing key");
                std::string key;
                if (!string(key))
                    return false;
                space();
                if (at >= text.size() || text[at] != ':')
                    return fail("a missing ':'");
                ++at;
                Value item;
                if (!value(item, depth + 1))
                    return false;
                out.object.emplace_back(std::move(key), std::move(item));
                space();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == '}') {
                    ++at;
                    return true;
                }
                return fail("a missing ',' or '}'");
            }
        }
        if (c == '[') {
            ++at;
            out = Value::make_array();
            space();
            if (at < text.size() && text[at] == ']') {
                ++at;
                return true;
            }
            for (;;) {
                Value item;
                if (!value(item, depth + 1))
                    return false;
                out.array.push_back(std::move(item));
                space();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == ']') {
                    ++at;
                    return true;
                }
                return fail("a missing ',' or ']'");
            }
        }
        if (c == '"') {
            out = Value(std::string());
            return string(out.string);
        }
        if (literal("true")) {
            out = Value(true);
            return true;
        }
        if (literal("false")) {
            out = Value(false);
            return true;
        }
        if (literal("null")) {
            out = Value();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = at;
            ++at;
            while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '.' || text[at] == 'e' ||
                                         text[at] == 'E' || text[at] == '+' || text[at] == '-'))
                ++at;
            const std::string number(text.substr(start, at - start));
            char* end = nullptr;
            const double parsed = std::strtod(number.c_str(), &end);
            if (end == nullptr || *end != '\0')
                return fail("a bad number");
            out = Value(parsed);
            return true;
        }
        return fail("an unexpected character");
    }
};

// Bytes of `text` that start a valid UTF-8 sequence: its length, or 0.
size_t utf8_length(std::string_view text, size_t at)
{
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char c = byte(at);
    size_t length = 0;
    unsigned code = 0;
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        length = 2, code = c & 0x1F;
    else if ((c & 0xF0) == 0xE0)
        length = 3, code = c & 0x0F;
    else if ((c & 0xF8) == 0xF0)
        length = 4, code = c & 0x07;
    else
        return 0;
    if (at + length > text.size())
        return 0;
    for (size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80)
            return 0;
        code = (code << 6) | (byte(at + i) & 0x3F);
    }
    // overlong forms, surrogates and past U+10FFFF aren't UTF-8
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code < 0xE000))
        return 0;
    return length;
}

void write_string(std::string& out, std::string_view text)
{
    out += '"';
    for (size_t at = 0; at < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[at]);
        if (c >= 0x80) {
            const size_t length = utf8_length(text, at);
            if (length == 0) {
                out += "\xEF\xBF\xBD"; // U+FFFD
                ++at;
            } else {
                out.append(text.substr(at, length));
                at += length;
            }
            continue;
        }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                out += escaped;
            } else {
                out += static_cast<char>(c);
            }
        }
        ++at;
    }
    out += '"';
}

void write(std::string& out, const Value& value)
{
    switch (value.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += value.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
        const double n = value.number;
        char text[40];
        if (!std::isfinite(n))
            std::snprintf(text, sizeof text, "null");
        else if (n == std::floor(n) && std::fabs(n) < 9007199254740992.0)
            std::snprintf(text, sizeof text, "%lld", static_cast<long long>(n));
        else
            std::snprintf(text, sizeof text, "%.17g", n);
        out += text;
        break;
    }
    case Value::Type::String: write_string(out, value.string); break;
    case Value::Type::Array: {
        out += '[';
        for (size_t i = 0; i < value.array.size(); ++i) {
            if (i != 0)
                out += ',';
            write(out, value.array[i]);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.object) {
            if (!first)
                out += ',';
            first = false;
            write_string(out, key);
            out += ':';
            write(out, item);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

bool parse(std::string_view text, Value& out, std::string& error)
{
    Reader reader{text};
    if (!reader.value(out, 0)) {
        error = reader.error;
        return false;
    }
    reader.space();
    if (reader.at != text.size()) {
        error = "more after the value at offset " + std::to_string(reader.at);
        return false;
    }
    return true;
}

std::string dump(const Value& value)
{
    std::string out;
    write(out, value);
    return out;
}

} // namespace vanacore::json
