#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// JSON values, read and written: what the MCP server (mcp.cpp) speaks. Objects keep their keys in the
// order they were added. Strings are UTF-8; dump() replaces bytes that aren't valid UTF-8 with U+FFFD, so
// text from the game (Shift-JIS, if it wasn't converted) can't make the output unreadable.
namespace vanacore::json {

struct Value {
    enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : type(Type::Bool), boolean(value) {}
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T value) : type(Type::Number), number(static_cast<double>(value)) {}
    Value(const char* value) : type(Type::String), string(value != nullptr ? value : "") {}
    Value(std::string value) : type(Type::String), string(std::move(value)) {}
    Value(std::string_view value) : type(Type::String), string(value) {}

    static Value make_array() { Value v; v.type = Type::Array; return v; }
    static Value make_object() { Value v; v.type = Type::Object; return v; }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // An object's field, added (null) if it isn't there; a null value becomes an object first.
    Value& operator[](std::string_view key);
    // An object's field, or nullptr.
    const Value* find(std::string_view key) const;
    // Adds to an array (a null value becomes one first).
    Value& push(Value value);

    // A field's value as a type, or `fallback` when it's missing or another type.
    std::string get_string(std::string_view key, std::string fallback = {}) const;
    double get_number(std::string_view key, double fallback = 0) const;
    bool get_bool(std::string_view key, bool fallback = false) const;
};

// Reads `text` (one value, whitespace around it allowed). False with `error` if it isn't JSON.
bool parse(std::string_view text, Value& out, std::string& error);

// The value as compact JSON.
std::string dump(const Value& value);

} // namespace vanacore::json
