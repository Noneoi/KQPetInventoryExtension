#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <limits>

namespace kqpet::release::json {
struct Value {
  enum class Kind { Null, Boolean, Number, String, Array, Object } kind = Kind::Null;
  bool boolean = false;
  std::string text;
  std::vector<Value> array;
  std::map<std::string, Value> object;
  const Value& at(const std::string& name) const {
    static const Value missing;
    const auto it = object.find(name); return it == object.end() ? missing : it->second;
  }
  bool keys(std::initializer_list<const char*> expected) const {
    if (kind != Kind::Object || object.size() != expected.size()) return false;
    for (const char* key : expected) if (!object.count(key)) return false;
    return true;
  }
  bool unsignedInteger(std::uint64_t* number) const {
    if (kind != Kind::Number || text.empty()) return false;
    std::uint64_t value = 0;
    for (char digit : text) {
      if (digit < '0' || digit > '9' || value > (UINT64_MAX - (digit - '0')) / 10) return false;
      value = value * 10 + digit - '0';
    }
    *number = value; return true;
  }
};
inline void appendUtf8(std::string& result, unsigned value) {
  if (value <= 0x7f) result += static_cast<char>(value);
  else if (value <= 0x7ff) { result += char(0xc0 | (value >> 6)); result += char(0x80 | (value & 63)); }
  else if (value <= 0xffff) { result += char(0xe0 | (value >> 12)); result += char(0x80 | ((value >> 6) & 63)); result += char(0x80 | (value & 63)); }
  else { result += char(0xf0 | (value >> 18)); result += char(0x80 | ((value >> 12) & 63)); result += char(0x80 | ((value >> 6) & 63)); result += char(0x80 | (value & 63)); }
}
class Parser final {
public:
  explicit Parser(std::string_view input) : input_(input) {}
  bool parse(Value* result) {
    if (input_.empty() || input_.size() > 16 * 1024 * 1024 ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input_.data(), static_cast<int>(input_.size()), nullptr, 0)) return false;
    if (!value(result, 0)) return false;
    space(); return offset_ == input_.size();
  }
private:
  void space() { while (offset_ < input_.size() && (input_[offset_] == ' ' || input_[offset_] == '\t' || input_[offset_] == '\r' || input_[offset_] == '\n')) ++offset_; }
  bool consume(char c) { space(); if (offset_ >= input_.size() || input_[offset_] != c) return false; ++offset_; return true; }
  bool hex(unsigned* result) {
    if (input_.size() - offset_ < 4) return false;
    unsigned value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const char c = input_[offset_++];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return false; value = value * 16 + digit;
    }
    *result = value; return true;
  }
  bool string(std::string* result) {
    if (!consume('"')) return false;
    result->clear();
    while (offset_ < input_.size() && result->size() <= 131072) {
      char c = input_[offset_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 32) return false;
      if (c != '\\') { *result += c; continue; }
      if (offset_ == input_.size()) return false;
      c = input_[offset_++];
      switch (c) {
        case '"': case '\\': case '/': *result += c; break;
        case 'b': *result += '\b'; break; case 'f': *result += '\f'; break;
        case 'n': *result += '\n'; break; case 'r': *result += '\r'; break; case 't': *result += '\t'; break;
        case 'u': {
          unsigned first = 0; if (!hex(&first)) return false;
          if (first >= 0xd800 && first <= 0xdbff) {
            if (input_.size() - offset_ < 6 || input_[offset_++] != '\\' || input_[offset_++] != 'u') return false;
            unsigned second = 0; if (!hex(&second) || second < 0xdc00 || second > 0xdfff) return false;
            first = 0x10000 + ((first - 0xd800) << 10) + second - 0xdc00;
          } else if (first >= 0xdc00 && first <= 0xdfff) return false;
          appendUtf8(*result, first); break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool number(Value* result) {
    const auto begin = offset_;
    if (input_[offset_] == '-') ++offset_;
    if (offset_ == input_.size()) return false;
    if (input_[offset_] == '0') ++offset_;
    else {
      if (input_[offset_] < '1' || input_[offset_] > '9') return false;
      while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') ++offset_;
    }
    if (offset_ < input_.size() && input_[offset_] == '.') {
      const auto start = ++offset_;
      while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') ++offset_;
      if (offset_ == start) return false;
    }
    if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
      ++offset_; if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) ++offset_;
      const auto start = offset_;
      while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') ++offset_;
      if (offset_ == start) return false;
    }
    if (offset_ - begin > 128) return false;
    result->kind = Value::Kind::Number; result->text = input_.substr(begin, offset_ - begin);
    std::uint64_t checked = 0; return result->unsignedInteger(&checked);
  }
  bool value(Value* result, unsigned depth) {
    space(); if (depth > 32 || ++nodes_ > 200000 || offset_ == input_.size()) return false;
    const char c = input_[offset_];
    if (c == '"') { result->kind = Value::Kind::String; return string(&result->text); }
    if (c == '{') {
      result->kind = Value::Kind::Object; ++offset_; space();
      if (consume('}')) return true;
      do {
        std::string key; Value child;
        if (!string(&key) || result->object.count(key) || !consume(':') || !value(&child, depth + 1)) return false;
        result->object.emplace(std::move(key), std::move(child));
        if (consume('}')) return true;
      } while (consume(','));
      return false;
    }
    if (c == '[') {
      result->kind = Value::Kind::Array; ++offset_; space();
      if (consume(']')) return true;
      do { Value child; if (!value(&child, depth + 1)) return false; result->array.push_back(std::move(child)); if (consume(']')) return true; } while (consume(','));
      return false;
    }
    for (const auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
      if (input_.substr(offset_, literal.size()) == literal) {
        offset_ += literal.size(); result->kind = literal == "null" ? Value::Kind::Null : Value::Kind::Boolean;
        result->boolean = literal == "true"; return true;
      }
    }
    return c == '-' || (c >= '0' && c <= '9') ? number(result) : false;
  }
  std::string_view input_;
  std::size_t offset_ = 0, nodes_ = 0;
};
inline std::string quote(const std::string& text) {
  std::string result = "\"";
  static constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += c; }
    else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    else result += c;
  }
  return result + '"';
}
inline std::string stringify(const Value& value) {
  switch (value.kind) {
    case Value::Kind::Null: return "null";
    case Value::Kind::Boolean: return value.boolean ? "true" : "false";
    case Value::Kind::Number: return value.text;
    case Value::Kind::String: return quote(value.text);
    case Value::Kind::Array: {
      std::string result = "["; bool first = true;
      for (const auto& child : value.array) { if (!first) result += ','; first = false; result += stringify(child); }
      return result + ']';
    }
    case Value::Kind::Object: {
      std::string result = "{"; bool first = true;
      for (const auto& [key, child] : value.object) { if (!first) result += ','; first = false; result += quote(key) + ':' + stringify(child); }
      return result + '}';
    }
  }
  return {};
}
}
