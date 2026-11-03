#pragma once
// Minimal strict JSON (RFC 8259) parser used for configuration files.
// - No comments, no trailing commas, depth-limited, error messages carry line:column.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace gateway::json {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, size_t line, size_t col)
        : std::runtime_error("JSON parse error at line " + std::to_string(line) +
                             ", column " + std::to_string(col) + ": " + msg) {}
};

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;         // Array elements
    std::vector<std::string> keys;    // Object keys (insertion order, parallel to `items`)

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_null() const { return type == Type::Null; }

    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &items[i];
        }
        return nullptr;
    }

    const char* type_name() const {
        switch (type) {
            case Type::Null: return "null";
            case Type::Bool: return "boolean";
            case Type::Number: return "number";
            case Type::String: return "string";
            case Type::Array: return "array";
            case Type::Object: return "object";
        }
        return "?";
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    Value parse_document() {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;  // UTF-8 BOM
        }
        skip_ws();
        Value v = parse_value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("unexpected trailing content");
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;
    const std::string& s_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        throw ParseError(msg, line, col);
    }

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }

    void skip_ws() {
        while (!eof() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    bool match_literal(const char* lit) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(pos_, n, lit) == 0) { pos_ += n; return true; }
        return false;
    }

    Value parse_value(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        if (eof()) fail("unexpected end of input");
        char c = peek();
        Value v;
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') { v.type = Value::Type::String; v.str = parse_string(); return v; }
        if (c == 't') { if (!match_literal("true")) fail("invalid literal"); v.type = Value::Type::Bool; v.boolean = true; return v; }
        if (c == 'f') { if (!match_literal("false")) fail("invalid literal"); v.type = Value::Type::Bool; v.boolean = false; return v; }
        if (c == 'n') { if (!match_literal("null")) fail("invalid literal"); return v; }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail(std::string("unexpected character '") + c + "'");
    }

    Value parse_object(int depth) {
        Value v;
        v.type = Value::Type::Object;
        expect('{');
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        for (;;) {
            skip_ws();
            if (peek() != '"') fail("expected string key");
            std::string key = parse_string();
            for (const auto& k : v.keys) {
                if (k == key) fail("duplicate key '" + key + "'");
            }
            skip_ws();
            expect(':');
            skip_ws();
            Value child = parse_value(depth + 1);
            v.keys.push_back(std::move(key));
            v.items.push_back(std::move(child));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; break; }
            fail("expected ',' or '}'");
        }
        return v;
    }

    Value parse_array(int depth) {
        Value v;
        v.type = Value::Type::Array;
        expect('[');
        skip_ws();
        if (peek() == ']') { ++pos_; return v; }
        for (;;) {
            skip_ws();
            v.items.push_back(parse_value(depth + 1));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; break; }
            fail("expected ',' or ']'");
        }
        return v;
    }

    Value parse_number() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            fail("invalid number");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("invalid number: digits required after '.'");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("invalid number: digits required in exponent");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        Value v;
        v.type = Value::Type::Number;
        v.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        if (!std::isfinite(v.number)) fail("number out of range");
        return v;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
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

    uint32_t parse_hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("invalid hex digit in \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            if (eof()) fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') break;
            if (c < 0x20) { --pos_; fail("control character in string"); }
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (eof()) fail("unterminated escape");
            char e = s_[pos_++];
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
                    uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!(pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u')) {
                            fail("unpaired surrogate");
                        }
                        pos_ += 2;
                        uint32_t lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    --pos_;
                    fail("invalid escape sequence");
            }
        }
        return out;
    }
};

inline Value parse(const std::string& text) {
    Parser p(text);
    return p.parse_document();
}

}  // namespace gateway::json
