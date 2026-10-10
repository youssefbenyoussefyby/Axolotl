#pragma once
// Minimal JSON reader for the CLI's metadata files (phases.json, queries.json).
// Read-only, no dependencies, so the build stays clonable. Not a general-purpose
// library: no comments, no streaming, depth capped at 64.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace minijson {

enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

struct Value {
    Type type = Type::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;       // array elements
    std::vector<std::string> keys;  // object keys, parallel to `members`
    std::vector<Value> members;     // object values

    const Value* Find(const std::string& key) const {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &members[i];
            }
        }
        return nullptr;
    }

    std::string Str(const std::string& key, const std::string& fallback = "") const {
        const Value* v = Find(key);
        return (v != nullptr && v->type == Type::kString) ? v->string : fallback;
    }

    double Num(const std::string& key, double fallback) const {
        const Value* v = Find(key);
        return (v != nullptr && v->type == Type::kNumber) ? v->number : fallback;
    }

    bool Has(const std::string& key) const {
        return Find(key) != nullptr;
    }

    const std::vector<Value>& List(const std::string& key) const {
        static const std::vector<Value> kEmpty;
        const Value* v = Find(key);
        return (v != nullptr && v->type == Type::kArray) ? v->items : kEmpty;
    }
};

class Parser {
   public:
    explicit Parser(const std::string& text) : s_(text) {}

    Value ParseDocument() {
        Value v = ParseValue(0);
        SkipWs();
        if (i_ != s_.size()) {
            Fail("unexpected content after the document");
        }
        return v;
    }

   private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void Fail(const std::string& msg) const {
        int line = 1;
        int col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        throw std::runtime_error("JSON error at line " + std::to_string(line) + ", column " +
                                 std::to_string(col) + ": " + msg);
    }

    void SkipWs() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    char Peek() {
        SkipWs();
        return i_ < s_.size() ? s_[i_] : '\0';
    }

    void Expect(char c) {
        if (Peek() != c) {
            Fail(std::string("expected '") + c + "'");
        }
        ++i_;
    }

    bool MatchLiteral(const char* lit) {
        const size_t n = std::strlen(lit);
        if (s_.compare(i_, n, lit) == 0) {
            i_ += n;
            return true;
        }
        return false;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    uint32_t ParseHex4() {
        if (i_ + 4 > s_.size()) {
            Fail("truncated \\u escape");
        }
        uint32_t cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') {
                cp |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                cp |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                cp |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                Fail("bad hex digit in \\u escape");
            }
        }
        return cp;
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        for (;;) {
            if (i_ >= s_.size()) {
                Fail("unterminated string");
            }
            const char c = s_[i_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) {
                Fail("unterminated escape");
            }
            const char e = s_[i_++];
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = ParseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        const uint32_t lo = ParseHex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    Fail("unknown escape sequence");
            }
        }
    }

    Value ParseValue(int depth) {
        if (depth > 64) {
            Fail("nesting too deep");
        }
        const char c = Peek();
        Value v;

        if (c == '{') {
            ++i_;
            v.type = Type::kObject;
            if (Peek() == '}') {
                ++i_;
                return v;
            }
            for (;;) {
                if (Peek() != '"') {
                    Fail("expected a string key");
                }
                std::string key = ParseString();
                Expect(':');
                Value member = ParseValue(depth + 1);
                v.keys.push_back(std::move(key));
                v.members.push_back(std::move(member));
                if (Peek() == ',') {
                    ++i_;
                    continue;
                }
                Expect('}');
                return v;
            }
        }

        if (c == '[') {
            ++i_;
            v.type = Type::kArray;
            if (Peek() == ']') {
                ++i_;
                return v;
            }
            for (;;) {
                v.items.push_back(ParseValue(depth + 1));
                if (Peek() == ',') {
                    ++i_;
                    continue;
                }
                Expect(']');
                return v;
            }
        }

        if (c == '"') {
            v.type = Type::kString;
            v.string = ParseString();
            return v;
        }

        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = i_;
            while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_]) != nullptr) {
                ++i_;
            }
            v.type = Type::kNumber;
            v.number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
            return v;
        }

        if (MatchLiteral("true")) {
            v.type = Type::kBool;
            v.boolean = true;
            return v;
        }
        if (MatchLiteral("false")) {
            v.type = Type::kBool;
            return v;
        }
        if (MatchLiteral("null")) {
            return v;
        }
        Fail("unexpected character");
    }
};

inline Value Parse(const std::string& text) {
    return Parser(text).ParseDocument();
}

}  // namespace minijson