#ifndef ECG_JSON_HPP
#define ECG_JSON_HPP

// A strict, exact, dependency-free JSON dialect for the operator and automation
// surface of this runtime.
//
// Two deliberate departures from permissive JSON practice:
//
//   1. Numbers are not binary floating point. A JSON number is carried as its
//      exact decimal literal and converted only through ParseQuantity, which
//      knows the scale of the target unit. "0.1" can therefore never become
//      0.1000000000000000055511151231257827.
//   2. The dialect is a closed subset. Exponent notation, duplicate object
//      keys, control characters in strings, invalid UTF-8, and trailing content
//      after the top-level value are rejected instead of being interpreted
//      charitably. A rejected request is safe; a reinterpreted one is not.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ecg/result.hpp"

namespace ecg {

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  using Member = std::pair<std::string, JsonValue>;
  using Object = std::vector<Member>;

  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };

  /// Bounds enforced by the parser and by the writers.
  static constexpr std::size_t kMaxDepth = 64;
  static constexpr std::size_t kMaxStringLength = 1u << 20;
  static constexpr std::size_t kMaxElements = 1000000;
  static constexpr std::size_t kMaxInputLength = 16u << 20;

  JsonValue() = default;

  [[nodiscard]] static JsonValue Null() { return JsonValue(); }
  [[nodiscard]] static JsonValue Bool(bool value);
  /// Carries the exact decimal literal; rejects any literal the dialect forbids.
  [[nodiscard]] static Result<JsonValue> Number(std::string literal);
  [[nodiscard]] static JsonValue String(std::string text);
  [[nodiscard]] static JsonValue ArrayValue(Array items);
  [[nodiscard]] static JsonValue ObjectValue(Object members);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::kNull; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::kBool; }
  [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::kNumber; }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::kString; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::kArray; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::kObject; }

  [[nodiscard]] bool as_bool() const noexcept { return bool_; }
  /// Exact decimal literal for a number member.
  [[nodiscard]] const std::string& as_number_text() const noexcept { return text_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return text_; }
  [[nodiscard]] const Array& as_array() const noexcept { return array_; }
  [[nodiscard]] const Object& as_object() const noexcept { return object_; }

  /// Object member lookup; returns nullptr when absent or when this is not an
  /// object.
  [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;

  /// Inserts or replaces an object member. Only valid on object values.
  [[nodiscard]] Status Set(std::string key, JsonValue value);

  /// Appends an array element. Only valid on array values.
  [[nodiscard]] Status Push(JsonValue value);

  /// Parses the strict dialect. On failure the error names the byte offset.
  [[nodiscard]] static Result<JsonValue> Parse(std::string_view text);

  /// Canonical rendering: object members sorted by key, no insignificant
  /// whitespace, strings escaped minimally, numbers as their exact literal.
  /// Two equal JSON values always render to identical bytes.
  [[nodiscard]] std::string Dump() const;

  /// Same value, indented for human reading. Never used for hashing or digests.
  [[nodiscard]] std::string DumpPretty(int indent_width = 2) const;

 private:
  void DumpInto(std::string* out, bool pretty, int indent_width, int depth) const;

  Kind kind_{Kind::kNull};
  bool bool_{false};
  std::string text_;
  Array array_;
  Object object_;
};

/// Escapes a string for JSON output using the canonical minimal form.
[[nodiscard]] std::string JsonEscape(std::string_view text);

/// Validates that a byte range is well-formed UTF-8 (no overlong forms, no
/// surrogates, no truncated sequences).
[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

}  // namespace ecg

#endif  // ECG_JSON_HPP
