#include "ecg/json.hpp"

#include <algorithm>
#include <cstdio>

namespace ecg {
namespace {

[[nodiscard]] bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool IsValidNumberLiteral(std::string_view literal) noexcept {
  if (literal.empty() || literal.size() > 64) {
    return false;
  }
  std::size_t index = 0;
  if (literal[index] == '-' || literal[index] == '+') {
    ++index;
  }
  const std::size_t integer_start = index;
  while (index < literal.size() && IsDigit(literal[index])) {
    ++index;
  }
  if (index == integer_start) {
    return false;  // No integer part.
  }
  // JSON forbids leading zeros, and accepting them would mean silently
  // reinterpreting one literal as another.
  if (literal[integer_start] == '0' && index - integer_start > 1) {
    return false;
  }
  if (index < literal.size() && literal[index] == '.') {
    ++index;
    const std::size_t fraction_start = index;
    while (index < literal.size() && IsDigit(literal[index])) {
      ++index;
    }
    if (index == fraction_start) {
      return false;  // No fractional digits.
    }
  }
  return index == literal.size();
}

/// Canonicalises a decimal literal: drops a leading '+', removes leading zeros
/// in the integer part, removes trailing zeros in the fraction, drops a zero
/// fraction, and normalises "-0" to "0".
[[nodiscard]] std::string CanonicalNumber(std::string_view literal) {
  std::size_t index = 0;
  bool negative = false;
  if (index < literal.size() && (literal[index] == '-' || literal[index] == '+')) {
    negative = literal[index] == '-';
    ++index;
  }
  std::string integer_part;
  while (index < literal.size() && IsDigit(literal[index])) {
    integer_part.push_back(literal[index]);
    ++index;
  }
  std::string fraction_part;
  if (index < literal.size() && literal[index] == '.') {
    ++index;
    while (index < literal.size() && IsDigit(literal[index])) {
      fraction_part.push_back(literal[index]);
      ++index;
    }
  }
  std::size_t first_significant = integer_part.find_first_not_of('0');
  integer_part = first_significant == std::string::npos ? std::string("0")
                                                        : integer_part.substr(first_significant);
  while (!fraction_part.empty() && fraction_part.back() == '0') {
    fraction_part.pop_back();
  }
  const bool is_zero = integer_part == "0" && fraction_part.empty();
  std::string out;
  if (negative && !is_zero) {
    out.push_back('-');
  }
  out.append(integer_part);
  if (!fraction_part.empty()) {
    out.push_back('.');
    out.append(fraction_part);
  }
  return out;
}

[[nodiscard]] constexpr char HexDigit(unsigned value) noexcept {
  return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  [[nodiscard]] Result<JsonValue> Parse() {
    SkipWhitespace();
    ECG_TRY(root, ParseValue(0));
    SkipWhitespace();
    if (index_ != text_.size()) {
      return Fail("trailing content after the top-level value");
    }
    return root;
  }

 private:
  [[nodiscard]] Error Fail(std::string message) const {
    return MakeError(ErrorCode::kMalformedInput, "json",
                     std::move(message) + " at byte offset " + std::to_string(index_));
  }

  void SkipWhitespace() noexcept {
    while (index_ < text_.size()) {
      const char c = text_[index_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++index_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] Result<JsonValue> ParseValue(std::size_t depth) {
    if (depth > JsonValue::kMaxDepth) {
      return Fail("nesting deeper than " + std::to_string(JsonValue::kMaxDepth) + " levels");
    }
    if (index_ >= text_.size()) {
      return Fail("unexpected end of input");
    }
    const char c = text_[index_];
    switch (c) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': {
        ECG_TRY(text, ParseString());
        return JsonValue::String(std::move(text));
      }
      case 't':
        if (text_.compare(index_, 4, "true") == 0) {
          index_ += 4;
          return JsonValue::Bool(true);
        }
        return Fail("invalid literal");
      case 'f':
        if (text_.compare(index_, 5, "false") == 0) {
          index_ += 5;
          return JsonValue::Bool(false);
        }
        return Fail("invalid literal");
      case 'n':
        if (text_.compare(index_, 4, "null") == 0) {
          index_ += 4;
          return JsonValue::Null();
        }
        return Fail("invalid literal");
      default:
        break;
    }
    if (c == '-' || c == '+' || IsDigit(c)) {
      const std::size_t start = index_;
      if (text_[index_] == '-' || text_[index_] == '+') {
        ++index_;
      }
      while (index_ < text_.size() && IsDigit(text_[index_])) {
        ++index_;
      }
      if (index_ < text_.size() && text_[index_] == '.') {
        ++index_;
        while (index_ < text_.size() && IsDigit(text_[index_])) {
          ++index_;
        }
      }
      const std::string_view literal = text_.substr(start, index_ - start);
      if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
        return Fail("exponent notation is not part of this dialect; write the exact decimal");
      }
      {
        std::size_t digit_start = 0;
        if (literal[digit_start] == '-' || literal[digit_start] == '+') {
          ++digit_start;
        }
        std::size_t digit_count = 0;
        while (digit_start + digit_count < literal.size() &&
               IsDigit(literal[digit_start + digit_count])) {
          ++digit_count;
        }
        if (digit_count > 1 && literal[digit_start] == '0') {
          return Fail("numbers may not carry leading zeros");
        }
      }
      if (!IsValidNumberLiteral(literal)) {
        return Fail("malformed number literal");
      }
      return JsonValue::Number(std::string(literal));
    }
    return Fail(std::string("unexpected character '") + c + "'");
  }

  [[nodiscard]] Result<std::string> ParseString() {
    ++index_;  // Opening quote.
    std::string out;
    while (true) {
      if (index_ >= text_.size()) {
        return Fail("unterminated string");
      }
      const unsigned char c = static_cast<unsigned char>(text_[index_]);
      if (c == '"') {
        ++index_;
        break;
      }
      if (c == '\\') {
        ++index_;
        if (index_ >= text_.size()) {
          return Fail("unterminated escape sequence");
        }
        const char escape = text_[index_++];
        switch (escape) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            ECG_TRY(code_unit, ParseHex4());
            std::uint32_t code_point = code_unit;
            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
              if (index_ + 1 < text_.size() && text_[index_] == '\\' && text_[index_ + 1] == 'u') {
                index_ += 2;
                ECG_TRY(low, ParseHex4());
                if (low < 0xDC00 || low > 0xDFFF) {
                  return Fail("high surrogate not followed by a low surrogate");
                }
                code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
              } else {
                return Fail("lone high surrogate escape");
              }
            } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
              return Fail("lone low surrogate escape");
            }
            AppendUtf8(&out, code_point);
            break;
          }
          default:
            return Fail("unsupported escape sequence");
        }
        continue;
      }
      if (c < 0x20) {
        return Fail("raw control character in string");
      }
      out.push_back(static_cast<char>(c));
      ++index_;
      if (out.size() > JsonValue::kMaxStringLength) {
        return Fail("string longer than the dialect limit");
      }
    }
    if (!IsValidUtf8(out)) {
      return Fail("string is not well-formed UTF-8");
    }
    return out;
  }

  [[nodiscard]] Result<std::uint32_t> ParseHex4() {
    if (index_ + 4 > text_.size()) {
      return Fail("truncated \\u escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[index_ + static_cast<std::size_t>(i)];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10u;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10u;
      } else {
        return Fail("invalid hexadecimal digit in \\u escape");
      }
      value = (value << 4) | digit;
    }
    index_ += 4;
    return value;
  }

  static void AppendUtf8(std::string* out, std::uint32_t code_point) {
    if (code_point < 0x80u) {
      out->push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800u) {
      out->push_back(static_cast<char>(0xC0u | (code_point >> 6)));
      out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000u) {
      out->push_back(static_cast<char>(0xE0u | (code_point >> 12)));
      out->push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
      out->push_back(static_cast<char>(0xF0u | (code_point >> 18)));
      out->push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
      out->push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
  }

  [[nodiscard]] Result<JsonValue> ParseArray(std::size_t depth) {
    ++index_;  // '['
    JsonValue::Array items;
    SkipWhitespace();
    if (index_ < text_.size() && text_[index_] == ']') {
      ++index_;
      return JsonValue::ArrayValue(std::move(items));
    }
    while (true) {
      SkipWhitespace();
      ECG_TRY(item, ParseValue(depth + 1));
      items.push_back(std::move(item));
      if (items.size() > JsonValue::kMaxElements) {
        return Fail("array longer than the dialect limit");
      }
      SkipWhitespace();
      if (index_ >= text_.size()) {
        return Fail("unterminated array");
      }
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == ']') {
        ++index_;
        break;
      }
      return Fail("expected ',' or ']' in array");
    }
    return JsonValue::ArrayValue(std::move(items));
  }

  [[nodiscard]] Result<JsonValue> ParseObject(std::size_t depth) {
    ++index_;  // '{'
    JsonValue::Object members;
    SkipWhitespace();
    if (index_ < text_.size() && text_[index_] == '}') {
      ++index_;
      return JsonValue::ObjectValue(std::move(members));
    }
    while (true) {
      SkipWhitespace();
      if (index_ >= text_.size() || text_[index_] != '"') {
        return Fail("expected a quoted object key");
      }
      ECG_TRY(key, ParseString());
      if (key.size() > 256) {
        return Fail("object key longer than 256 bytes");
      }
      SkipWhitespace();
      if (index_ >= text_.size() || text_[index_] != ':') {
        return Fail("expected ':' after object key");
      }
      ++index_;
      SkipWhitespace();
      ECG_TRY(value, ParseValue(depth + 1));
      for (const auto& member : members) {
        if (member.first == key) {
          return Fail("duplicate object key '" + key + "'");
        }
      }
      members.emplace_back(std::move(key), std::move(value));
      if (members.size() > JsonValue::kMaxElements) {
        return Fail("object larger than the dialect limit");
      }
      SkipWhitespace();
      if (index_ >= text_.size()) {
        return Fail("unterminated object");
      }
      if (text_[index_] == ',') {
        ++index_;
        continue;
      }
      if (text_[index_] == '}') {
        ++index_;
        break;
      }
      return Fail("expected ',' or '}' in object");
    }
    return JsonValue::ObjectValue(std::move(members));
  }

  std::string_view text_;
  std::size_t index_{0};
};

}  // namespace

bool IsValidUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (byte < 0x80u) {
      ++index;
      continue;
    }
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = byte & 0x1Fu;
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = byte & 0x07u;
    } else {
      return false;
    }
    if (index + extra >= text.size() + 0 && index + extra > text.size() - 1) {
      return false;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
      const auto continuation = static_cast<unsigned char>(text[index + i]);
      if ((continuation & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    const std::uint32_t minimum = extra == 1 ? 0x80u : (extra == 2 ? 0x800u : 0x10000u);
    if (code_point < minimum || code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out.append("\\\""); continue;
      case '\\': out.append("\\\\"); continue;
      case '\b': out.append("\\b"); continue;
      case '\f': out.append("\\f"); continue;
      case '\n': out.append("\\n"); continue;
      case '\r': out.append("\\r"); continue;
      case '\t': out.append("\\t"); continue;
      default: break;
    }
    if (byte < 0x20u) {
      out.append("\\u00");
      out.push_back(HexDigit((byte >> 4) & 0xFu));
      out.push_back(HexDigit(byte & 0xFu));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

JsonValue JsonValue::Bool(bool value) {
  JsonValue result;
  result.kind_ = Kind::kBool;
  result.bool_ = value;
  return result;
}

Result<JsonValue> JsonValue::Number(std::string literal) {
  if (!IsValidNumberLiteral(literal)) {
    return MakeError(ErrorCode::kMalformedInput, "json", "malformed number literal '" + literal + "'");
  }
  JsonValue result;
  result.kind_ = Kind::kNumber;
  result.text_ = CanonicalNumber(literal);
  return result;
}

JsonValue JsonValue::String(std::string text) {
  JsonValue result;
  result.kind_ = Kind::kString;
  result.text_ = std::move(text);
  return result;
}

JsonValue JsonValue::ArrayValue(Array items) {
  JsonValue result;
  result.kind_ = Kind::kArray;
  result.array_ = std::move(items);
  return result;
}

JsonValue JsonValue::ObjectValue(Object members) {
  JsonValue result;
  result.kind_ = Kind::kObject;
  result.object_ = std::move(members);
  return result;
}

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
  if (kind_ != Kind::kObject) {
    return nullptr;
  }
  for (const auto& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

Status JsonValue::Set(std::string key, JsonValue value) {
  if (kind_ != Kind::kObject) {
    return MakeError(ErrorCode::kInvalidArgument, "json", "Set on a non-object value");
  }
  for (auto& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return OkStatus();
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
  return OkStatus();
}

Status JsonValue::Push(JsonValue value) {
  if (kind_ != Kind::kArray) {
    return MakeError(ErrorCode::kInvalidArgument, "json", "Push on a non-array value");
  }
  array_.push_back(std::move(value));
  return OkStatus();
}

Result<JsonValue> JsonValue::Parse(std::string_view text) {
  if (text.size() > kMaxInputLength) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "json",
                     "input longer than " + std::to_string(kMaxInputLength) + " bytes");
  }
  Parser parser(text);
  return parser.Parse();
}

void JsonValue::DumpInto(std::string* out, bool pretty, int indent_width, int depth) const {
  const auto newline_indent = [&](int level) {
    if (!pretty) {
      return;
    }
    out->push_back('\n');
    out->append(static_cast<std::size_t>(level * indent_width), ' ');
  };

  switch (kind_) {
    case Kind::kNull: out->append("null"); return;
    case Kind::kBool: out->append(bool_ ? "true" : "false"); return;
    case Kind::kNumber: out->append(text_); return;
    case Kind::kString:
      out->push_back('"');
      out->append(JsonEscape(text_));
      out->push_back('"');
      return;
    case Kind::kArray: {
      if (array_.empty()) {
        out->append("[]");
        return;
      }
      out->push_back('[');
      for (std::size_t i = 0; i < array_.size(); ++i) {
        if (i != 0) {
          out->push_back(',');
        }
        newline_indent(depth + 1);
        array_[i].DumpInto(out, pretty, indent_width, depth + 1);
      }
      newline_indent(depth);
      out->push_back(']');
      return;
    }
    case Kind::kObject: {
      if (object_.empty()) {
        out->append("{}");
        return;
      }
      // Canonical member order: byte-wise ascending key order. Ties are
      // impossible because duplicate keys are rejected at parse time and by Set.
      std::vector<const Member*> ordered;
      ordered.reserve(object_.size());
      for (const Member& member : object_) {
        ordered.push_back(&member);
      }
      std::sort(ordered.begin(), ordered.end(), [](const Member* a, const Member* b) {
        return a->first < b->first;
      });
      out->push_back('{');
      for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (i != 0) {
          out->push_back(',');
        }
        newline_indent(depth + 1);
        out->push_back('"');
        out->append(JsonEscape(ordered[i]->first));
        out->append(pretty ? "\": " : "\":");
        ordered[i]->second.DumpInto(out, pretty, indent_width, depth + 1);
      }
      newline_indent(depth);
      out->push_back('}');
      return;
    }
  }
}

std::string JsonValue::Dump() const {
  std::string out;
  DumpInto(&out, false, 2, 0);
  return out;
}

std::string JsonValue::DumpPretty(int indent_width) const {
  std::string out;
  DumpInto(&out, true, indent_width, 0);
  return out;
}

}  // namespace ecg
