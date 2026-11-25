#pragma once
// A small JSON reader, enough for GitHub's API and app.json: objects, arrays, strings (UTF-8, \u escapes), numbers,
// true, false, null. A value that isn't there reads as null, so lookups chain without checks.

#include <string>
#include <utility>
#include <vector>

namespace vcapp::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;                            // an array's
    std::vector<std::pair<std::string, Value>> members;  // an object's, in order

    // An object's member (null when it isn't one or hasn't it)
    const Value& operator[](const char* key) const;
    const std::string& str() const { return string; }
    bool is_true() const { return type == Type::Bool && boolean; }
};

// `text` read whole; false when it isn't JSON (or nests deeper than 64)
bool parse(const std::string& text, Value& out);

} // namespace vcapp::json
