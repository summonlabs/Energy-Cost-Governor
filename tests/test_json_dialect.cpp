// Tests for the strict JSON dialect: exact decimal numbers, canonical dumping,
// and every rejection the dialect promises.
//
// Byte sequences are built from explicit values rather than source-encoded
// literals, so no compiler code-page decision can change what the parser sees.

#include "test.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/json.hpp"

namespace {

[[nodiscard]] std::string Bytes(std::initializer_list<unsigned> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned value : values) {
    out.push_back(static_cast<char>(value & 0xFFu));
  }
  return out;
}

/// Parses a document that the dialect must accept, recording a failure when it
/// does not so the rest of a test can keep making its point.
[[nodiscard]] ecg::JsonValue ParseOk(std::string_view text) {
  const auto parsed = ecg::JsonValue::Parse(text);
  ECG_CHECK_MSG(parsed.ok(), std::string("expected a valid document: ") + std::string(text));
  if (!parsed.ok()) {
    return ecg::JsonValue::Null();
  }
  return parsed.value();
}

/// Asserts that a byte sequence is rejected, and that it is rejected as a
/// malformed input rather than silently reinterpreted.
void ExpectRejected(std::string_view text, const char* const what) {
  const auto parsed = ecg::JsonValue::Parse(text);
  ECG_CHECK_MSG(!parsed.ok(), std::string("expected rejection of ") + what);
  if (!parsed.ok()) {
    ECG_CHECK_MSG(parsed.error().code() == ecg::ErrorCode::kMalformedInput,
                  std::string("expected malformed_input for ") + what + ", got " +
                      std::string(ecg::ErrorCodeName(parsed.error().code())));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trip and canonical rendering
// ---------------------------------------------------------------------------

ECG_TEST("json.parse_and_dump_round_trip_a_nested_document") {
  const std::string text =
      "{\"schema\":\"ecg/1\","
      "\"request\":{\"id\":\"abc\",\"tags\":[\"a\",\"b\",\"c\"],"
      "\"limits\":{\"power_kw\":\"1.5\",\"count\":3,\"ratio\":0.000001,"
      "\"flag\":true,\"none\":null}},"
      "\"items\":[{\"k\":1},{\"k\":2}]}";

  const ecg::JsonValue document = ParseOk(text);
  ECG_CHECK(document.is_object());
  if (!document.is_object()) {
    return;
  }

  const ecg::JsonValue* schema = document.Find("schema");
  ECG_CHECK(schema != nullptr);
  if (schema != nullptr) {
    ECG_CHECK(schema->is_string());
    ECG_CHECK_EQ(schema->as_string(), std::string("ecg/1"));
  }

  const ecg::JsonValue* request = document.Find("request");
  ECG_CHECK(request != nullptr);
  if (request != nullptr) {
    const ecg::JsonValue* tags = request->Find("tags");
    ECG_CHECK(tags != nullptr);
    if (tags != nullptr) {
      ECG_CHECK(tags->is_array());
      ECG_CHECK_EQ(tags->as_array().size(), static_cast<std::size_t>(3));
    }
    const ecg::JsonValue* limits = request->Find("limits");
    ECG_CHECK(limits != nullptr);
    if (limits != nullptr) {
      const ecg::JsonValue* none = limits->Find("none");
      ECG_CHECK(none != nullptr);
      if (none != nullptr) {
        ECG_CHECK(none->is_null());
      }
      const ecg::JsonValue* flag = limits->Find("flag");
      ECG_CHECK(flag != nullptr);
      if (flag != nullptr) {
        ECG_CHECK(flag->is_bool());
        ECG_CHECK_EQ(flag->as_bool(), true);
      }
      const ecg::JsonValue* count = limits->Find("count");
      ECG_CHECK(count != nullptr);
      if (count != nullptr) {
        ECG_CHECK(count->is_number());
        ECG_CHECK_EQ(count->as_number_text(), std::string("3"));
        // Find on a non-object reports absence rather than guessing.
        ECG_CHECK(count->Find("nothing") == nullptr);
      }
    }
  }

  ECG_CHECK(document.Find("absent") == nullptr);

  const std::string dumped = document.Dump();
  const ecg::JsonValue reparsed = ParseOk(dumped);
  ECG_CHECK_EQ(reparsed.Dump(), dumped);
  ECG_CHECK_EQ(reparsed.DumpPretty(4), document.DumpPretty(4));
}

ECG_TEST("json.canonical_dump_sorts_object_members_by_key") {
  const ecg::JsonValue parsed = ParseOk("{\"b\":1,\"a\":2,\"c\":3}");
  ECG_CHECK_EQ(parsed.Dump(), std::string("{\"a\":2,\"b\":1,\"c\":3}"));

  // The same members inserted in two different orders render identically.
  ecg::JsonValue::Object first_order;
  first_order.emplace_back("zulu", ecg::JsonValue::Bool(true));
  first_order.emplace_back("alpha", ecg::JsonValue::Bool(false));
  first_order.emplace_back("mike", ecg::JsonValue::Null());

  ecg::JsonValue::Object second_order;
  second_order.emplace_back("mike", ecg::JsonValue::Null());
  second_order.emplace_back("alpha", ecg::JsonValue::Bool(false));
  second_order.emplace_back("zulu", ecg::JsonValue::Bool(true));

  const ecg::JsonValue one = ecg::JsonValue::ObjectValue(first_order);
  const ecg::JsonValue two = ecg::JsonValue::ObjectValue(second_order);
  ECG_CHECK_EQ(one.Dump(), std::string("{\"alpha\":false,\"mike\":null,\"zulu\":true}"));
  ECG_CHECK_EQ(one.Dump(), two.Dump());

  // Nested objects are sorted at every level.
  const ecg::JsonValue nested = ParseOk("{\"b\":{\"y\":1,\"x\":2},\"a\":[]}");
  ECG_CHECK_EQ(nested.Dump(), std::string("{\"a\":[],\"b\":{\"x\":2,\"y\":1}}"));

  // Set replaces an existing member instead of appending a duplicate, and the
  // canonical order survives.
  ecg::JsonValue mutable_object = ecg::JsonValue::ObjectValue({});
  ECG_CHECK_OK(mutable_object.Set("beta", ecg::JsonValue::Bool(true)));
  ECG_CHECK_OK(mutable_object.Set("alpha", ecg::JsonValue::Bool(false)));
  ECG_CHECK_OK(mutable_object.Set("beta", ecg::JsonValue::Bool(false)));
  ECG_CHECK_EQ(mutable_object.as_object().size(), static_cast<std::size_t>(2));
  ECG_CHECK_EQ(mutable_object.Dump(), std::string("{\"alpha\":false,\"beta\":false}"));

  // Set and Push refuse the wrong kind of value instead of switching kind.
  ecg::JsonValue not_an_object = ecg::JsonValue::Bool(true);
  ECG_CHECK_ERR(not_an_object.Set("k", ecg::JsonValue::Null()),
                ecg::ErrorCode::kInvalidArgument);
  ecg::JsonValue not_an_array = ecg::JsonValue::Bool(true);
  ECG_CHECK_ERR(not_an_array.Push(ecg::JsonValue::Null()), ecg::ErrorCode::kInvalidArgument);
}

ECG_TEST("json.equal_values_dump_to_identical_bytes") {
  const ecg::JsonValue ordered = ParseOk("{\"one\":1,\"two\":[1,2,3],\"three\":{\"a\":null}}");
  const ecg::JsonValue shuffled = ParseOk("{\"three\":{\"a\":null},\"two\":[1,2,3],\"one\":1}");
  const std::string canonical = ordered.Dump();
  ECG_CHECK_EQ(shuffled.Dump(), canonical);

  // Repeated calls are stable and do not mutate the value.
  ECG_CHECK_EQ(ordered.Dump(), canonical);
  ECG_CHECK_EQ(ordered.Dump(), canonical);
  ECG_CHECK_EQ(shuffled.Dump(), canonical);

  // Pretty printing is a different rendering of the same value, also stable.
  const std::string pretty = ordered.DumpPretty();
  ECG_CHECK(pretty != canonical);
  ECG_CHECK_EQ(ordered.DumpPretty(), pretty);
  ECG_CHECK_EQ(shuffled.DumpPretty(), pretty);

  // Re-parsing the canonical form yields the same canonical bytes again.
  ECG_CHECK_EQ(ParseOk(canonical).Dump(), canonical);
}

// ---------------------------------------------------------------------------
// Numbers are exact decimal literals
// ---------------------------------------------------------------------------

ECG_TEST("json.numbers_are_exact_literals_and_never_floating_point") {
  const ecg::JsonValue tenth = ParseOk("0.1");
  ECG_CHECK(tenth.is_number());
  ECG_CHECK_EQ(tenth.as_number_text(), std::string("0.1"));
  ECG_CHECK_EQ(tenth.Dump(), std::string("0.1"));

  // 2^53 + 1 is not representable in a double; it must survive unchanged.
  const ecg::JsonValue large = ParseOk("9007199254740993");
  ECG_CHECK_EQ(large.Dump(), std::string("9007199254740993"));

  // Far beyond double precision, digit for digit. The last digit is non-zero
  // on purpose: a trailing zero is insignificant and is canonically dropped.
  const std::string exact =
      "123456789012345678901234567890.123456789012345678901234567891";
  const ecg::JsonValue wide = ParseOk(exact);
  ECG_CHECK_EQ(wide.as_number_text(), exact);
  ECG_CHECK_EQ(wide.Dump(), exact);

  // Two literals that a double would conflate stay distinct.
  const ecg::JsonValue pair = ParseOk("[0.1,0.1000000000000000055511151231257827]");
  ECG_CHECK_EQ(pair.Dump(), std::string("[0.1,0.1000000000000000055511151231257827]"));
  const ecg::JsonValue array = pair.is_array() ? pair : ecg::JsonValue::Null();
  if (array.is_array() && array.as_array().size() == 2u) {
    ECG_CHECK(array.as_array()[0].as_number_text() != array.as_array()[1].as_number_text());
  }

  // A number is not a string, and the literal is available only as text.
  ECG_CHECK(!tenth.is_string());
  ecg::JsonValue constructed = ecg::JsonValue::Null();
  const auto via_factory = ecg::JsonValue::Number("0.1");
  ECG_CHECK_OK(via_factory);
  if (via_factory.ok()) {
    constructed = via_factory.value();
    ECG_CHECK_EQ(constructed.Dump(), std::string("0.1"));
  }
  ECG_CHECK_ERR(ecg::JsonValue::Number("1e3"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JsonValue::Number(""), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JsonValue::Number(".5"), ecg::ErrorCode::kMalformedInput);
}

ECG_TEST("json.numbers_canonicalise_to_the_shortest_exact_form") {
  const struct {
    const char* input;
    const char* canonical;
  } cases[] = {
      {"0.1", "0.1"},   {"1.500", "1.5"},    {"-0", "0"},       {"+3", "3"},
      {"0.0", "0"},     {"-0.000", "0"},     {"10.00", "10"},   {"+0.5", "0.5"},
      {"-1.500", "-1.5"}, {"1234.000", "1234"}, {"0", "0"},     {"-0.5", "-0.5"},
      {"9007199254740993", "9007199254740993"},
  };
  for (const auto& item : cases) {
    const ecg::JsonValue value = ParseOk(item.input);
    ECG_CHECK_MSG(value.Dump() == item.canonical,
                  std::string("canonical form of ") + item.input + " should be " + item.canonical +
                      ", got " + value.Dump());
  }

  // Canonical numbers survive a further parse/dump cycle unchanged.
  for (const auto& item : cases) {
    const ecg::JsonValue once = ParseOk(item.canonical);
    ECG_CHECK_EQ(once.Dump(), std::string(item.canonical));
  }
}

ECG_TEST("json.parse_rejects_numbers_with_leading_zeros") {
  // The dialect is a closed subset of JSON, so a leading zero in a multi-digit
  // integer part is a malformed literal rather than something to reinterpret.
  const char* const rejected[] = {
      "007", "0000", "+000.500", "-01", "00", "00.5", "-00", "+00", "01.5",
  };
  for (const char* const text : rejected) {
    ExpectRejected(text, text);
  }

  // The literal factory refuses them as well.
  ECG_CHECK_ERR(ecg::JsonValue::Number("007"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JsonValue::Number("00.5"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JsonValue::Number("-01"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JsonValue::Number("+000.500"), ecg::ErrorCode::kMalformedInput);

  // A single zero is still an ordinary number, with or without a sign.
  ECG_CHECK_EQ(ParseOk("0").Dump(), std::string("0"));
  ECG_CHECK_EQ(ParseOk("-0").Dump(), std::string("0"));
  ECG_CHECK_EQ(ParseOk("+0").Dump(), std::string("0"));
  ECG_CHECK_EQ(ParseOk("0.5").Dump(), std::string("0.5"));
  ECG_CHECK_EQ(ParseOk("-0.5").Dump(), std::string("-0.5"));
  ECG_CHECK_EQ(ParseOk("10.00").Dump(), std::string("10"));
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

ECG_TEST("json.parse_rejects_notation_the_dialect_forbids") {
  const char* const rejected[] = {
      "1e3", "1E3", "1.5e3", "1.5E-3", "-2e+10", "0e0", "+1e1",
  };
  for (const char* const text : rejected) {
    ExpectRejected(text, text);
  }

  const char* const malformed[] = {
      ".5", "5.", "+", "-", "1.", "-.", "+.", "1.2.3", "--1", "1-", "nan", "inf",
  };
  for (const char* const text : malformed) {
    ExpectRejected(text, text);
  }
}

ECG_TEST("json.parse_rejects_duplicate_object_keys") {
  ExpectRejected("{\"a\":1,\"a\":2}", "a duplicated key");
  ExpectRejected("{\"a\":1,\"b\":2,\"a\":3}", "a duplicated key after another member");
  ExpectRejected("{\"outer\":{\"k\":1,\"k\":2}}", "a duplicated nested key");
  // The key is compared after unescaping, so two spellings of one key collide.
  ExpectRejected("{\"a\":1,\"\\u0061\":2}", "an escaped duplicate of a plain key");

  // Distinct keys that merely look similar are still accepted.
  const ecg::JsonValue distinct = ParseOk("{\"a\":1,\"A\":2,\"aa\":3}");
  ECG_CHECK_EQ(distinct.as_object().size(), static_cast<std::size_t>(3));
}

ECG_TEST("json.parse_rejects_raw_control_characters_in_strings") {
  ExpectRejected(Bytes({'"', 'a', 0x01, 'b', '"'}), "a 0x01 byte in a string");
  ExpectRejected(Bytes({'"', 'a', 0x1F, 'b', '"'}), "a 0x1F byte in a string");
  ExpectRejected(Bytes({'"', 'a', 0x09, 'b', '"'}), "a raw tab in a string");
  ExpectRejected(Bytes({'"', 'a', 0x0A, 'b', '"'}), "a raw newline in a string");
  ExpectRejected(Bytes({'"', 'a', 0x0D, 'b', '"'}), "a raw carriage return in a string");
  ExpectRejected(Bytes({'"', 0x00, '"'}), "a raw NUL in a string");

  // Outside a string the same bytes are ordinary whitespace.
  const ecg::JsonValue spaced = ParseOk("{\r\n\t\"a\" : 1 }");
  ECG_CHECK_EQ(spaced.Dump(), std::string("{\"a\":1}"));
}

ECG_TEST("json.parse_rejects_invalid_utf8_in_strings") {
  ExpectRejected(Bytes({'"', 0xFF, '"'}), "a 0xFF byte");
  ExpectRejected(Bytes({'"', 0x80, '"'}), "a lone continuation byte");
  ExpectRejected(Bytes({'"', 0xC0, 0x80, '"'}), "an overlong two-byte sequence");
  ExpectRejected(Bytes({'"', 0xE0, 0x80, 0x80, '"'}), "an overlong three-byte sequence");
  ExpectRejected(Bytes({'"', 0xE2, 0x82, '"'}), "a truncated three-byte sequence");
  ExpectRejected(Bytes({'"', 0xED, 0xA0, 0x80, '"'}), "a directly encoded surrogate");
  ExpectRejected(Bytes({'"', 0xF4, 0x90, 0x80, 0x80, '"'}), "a code point above U+10FFFF");

  // The same byte sequences are accepted when they are well formed.
  const ecg::JsonValue two_byte = ParseOk(Bytes({'"', 0xC3, 0xA9, '"'}));
  ECG_CHECK_EQ(two_byte.as_string(), Bytes({0xC3, 0xA9}));
  const ecg::JsonValue three_byte = ParseOk(Bytes({'"', 0xE2, 0x82, 0xAC, '"'}));
  ECG_CHECK_EQ(three_byte.as_string(), Bytes({0xE2, 0x82, 0xAC}));
  const ecg::JsonValue four_byte = ParseOk(Bytes({'"', 0xF0, 0x9F, 0x98, 0x80, '"'}));
  ECG_CHECK_EQ(four_byte.as_string(), Bytes({0xF0, 0x9F, 0x98, 0x80}));
}

ECG_TEST("json.parse_rejects_lone_surrogate_escapes") {
  ExpectRejected("\"\\ud83d\"", "a lone high surrogate");
  ExpectRejected("\"\\uD83D\"", "an upper-case lone high surrogate");
  ExpectRejected("\"\\udc00\"", "a lone low surrogate");
  ExpectRejected("\"\\ud83d\\u0041\"", "a high surrogate followed by a non-surrogate");
  ExpectRejected("\"\\ud83d\\ud83d\"", "two high surrogates in a row");
  ExpectRejected("\"\\ud83d\\ude00extra\\ud83d\"", "a trailing lone high surrogate");
  ExpectRejected("\"\\u12\"", "a truncated escape");

  // The well-formed pair is accepted and becomes one code point.
  const ecg::JsonValue grinning = ParseOk("\"\\ud83d\\ude00\"");
  ECG_CHECK_EQ(grinning.as_string(), Bytes({0xF0, 0x9F, 0x98, 0x80}));
  ECG_CHECK(ecg::IsValidUtf8(grinning.as_string()));
}

ECG_TEST("json.parse_rejects_unterminated_constructs") {
  ExpectRejected("\"abc", "an unterminated string");
  ExpectRejected("\"abc\\", "an unterminated escape");
  ExpectRejected("[1,2", "an unterminated array");
  ExpectRejected("[", "an array with no closing bracket");
  ExpectRejected("{\"a\":1", "an unterminated object");
  ExpectRejected("{\"a\"", "an object key with no colon");
  ExpectRejected("{\"a\":", "an object key with no value");
  ExpectRejected("{", "an object with no closing brace");
  ExpectRejected("[1,", "an array with a trailing comma");
  ExpectRejected("{\"a\":1,}", "an object with a trailing comma");
  ExpectRejected("[1 2]", "an array without a separator");
  ExpectRejected("{\"a\":1 \"b\":2}", "an object without a separator");
  ExpectRejected("{a:1}", "an unquoted object key");
}

ECG_TEST("json.parse_rejects_trailing_content_after_the_top_level_value") {
  ExpectRejected("{}\n{}", "a second document");
  ExpectRejected("{} {}", "a second document after a space");
  ExpectRejected("1 2", "two numbers");
  ExpectRejected("null null", "two nulls");
  ExpectRejected("\"a\"\"b\"", "two adjacent strings");
  ExpectRejected("[]x", "a stray character");
  ExpectRejected("truefalse", "a concatenated literal");

  // Trailing whitespace is not trailing content.
  ECG_CHECK_EQ(ParseOk("{}\n\t ").Dump(), std::string("{}"));
}

ECG_TEST("json.parse_rejects_nesting_deeper_than_the_limit") {
  std::string deep;
  for (int i = 0; i < 200; ++i) {
    deep.push_back('[');
  }
  deep.append("0");
  for (int i = 0; i < 200; ++i) {
    deep.push_back(']');
  }
  ExpectRejected(deep, "two hundred nested arrays");

  std::string deep_objects;
  for (int i = 0; i < 200; ++i) {
    deep_objects.append("{\"a\":");
  }
  deep_objects.append("1");
  for (int i = 0; i < 200; ++i) {
    deep_objects.push_back('}');
  }
  ExpectRejected(deep_objects, "two hundred nested objects");

  // A document comfortably inside the limit is accepted.
  std::string shallow;
  for (int i = 0; i < 10; ++i) {
    shallow.push_back('[');
  }
  shallow.append("0");
  for (int i = 0; i < 10; ++i) {
    shallow.push_back(']');
  }
  const ecg::JsonValue parsed = ParseOk(shallow);
  ECG_CHECK(parsed.is_array());
}

// ---------------------------------------------------------------------------
// Escapes, UTF-8 validation, and JSON escaping
// ---------------------------------------------------------------------------

ECG_TEST("json.parse_accepts_the_documented_escape_sequences") {
  const ecg::JsonValue accented = ParseOk("\"\\u00e9\"");
  ECG_CHECK_EQ(accented.as_string(), Bytes({0xC3, 0xA9}));
  ECG_CHECK_EQ(accented.Dump(), std::string("\"\xC3\xA9\""));

  const std::string escape_document =
      Bytes({'"', '\\', '"', '\\', '\\', '\\', '/', '\\', 'b', '\\', 'f',
             '\\', 'n', '\\', 'r', '\\', 't', '"'});
  const ecg::JsonValue escaped = ParseOk(escape_document);
  ECG_CHECK_EQ(escaped.as_string(), std::string("\"\\/\b\f\n\r\t"));
  // Re-escaping is canonical: an escaped forward slash becomes a bare slash.
  const std::string canonically_escaped =
      Bytes({'"', '\\', '"', '\\', '\\', '/', '\\', 'b', '\\', 'f',
             '\\', 'n', '\\', 'r', '\\', 't', '"'});
  ECG_CHECK_EQ(escaped.Dump(), canonically_escaped);
  ECG_CHECK(escaped.Dump() != escape_document);

  // An escaped NUL is a real NUL byte in the value and is re-escaped on output.
  const ecg::JsonValue nul = ParseOk("\"\\u0000\"");
  ECG_CHECK_EQ(nul.as_string(), std::string("\0", 1));
  ECG_CHECK_EQ(nul.Dump(), std::string("\"\\u0000\""));

  // Escapes are not required for characters that have a short form.
  ExpectRejected("\"\\x41\"", "an unsupported escape");
  ExpectRejected("\"\\'\"", "an unsupported escape");
  ExpectRejected("\"\\u00g1\"", "a bad hexadecimal digit");
}

ECG_TEST("json.is_valid_utf8_accepts_well_formed_sequences") {
  ECG_CHECK(ecg::IsValidUtf8(std::string_view()));
  ECG_CHECK(ecg::IsValidUtf8(std::string_view("plain ASCII 123")));
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xC2, 0x80})));              // U+0080, shortest form
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xC3, 0xA9})));              // U+00E9
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xDF, 0xBF})));              // U+07FF
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xE0, 0xA0, 0x80})));        // U+0800, shortest form
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xE2, 0x82, 0xAC})));        // U+20AC
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xEF, 0xBF, 0xBD})));        // U+FFFD
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xF0, 0x90, 0x80, 0x80})));  // U+10000, shortest form
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xF0, 0x9F, 0x98, 0x80})));  // U+1F600
  ECG_CHECK(ecg::IsValidUtf8(Bytes({0xF4, 0x8F, 0xBF, 0xBF})));  // U+10FFFF
  ECG_CHECK(ecg::IsValidUtf8(Bytes({'a', 0xC3, 0xA9, 'b', 0xF0, 0x9F, 0x98, 0x80, 'c'})));
}

ECG_TEST("json.is_valid_utf8_rejects_overlong_truncated_and_surrogate_forms") {
  const std::vector<std::string> rejected = {
      Bytes({0x80}),                          // lone continuation
      Bytes({0xBF}),                          // lone continuation
      Bytes({0xC0, 0x80}),                    // overlong NUL
      Bytes({0xC1, 0xBF}),                    // overlong
      Bytes({0xC2}),                          // truncated
      Bytes({0xC3}),                          // truncated
      Bytes({0xE0, 0x80, 0x80}),              // overlong
      Bytes({0xE0, 0x9F, 0xBF}),              // overlong, just below U+0800
      Bytes({0xE2, 0x82}),                    // truncated
      Bytes({0xE2}),                          // truncated
      Bytes({0xED, 0xA0, 0x80}),              // U+D800 encoded directly
      Bytes({0xED, 0xBF, 0xBF}),              // U+DFFF encoded directly
      Bytes({0xF0, 0x80, 0x80, 0x80}),        // overlong
      Bytes({0xF0, 0x8F, 0xBF, 0xBF}),        // overlong, just below U+10000
      Bytes({0xF0, 0x9F, 0x98}),              // truncated
      Bytes({0xF0, 0x9F}),                    // truncated
      Bytes({0xF0}),                          // truncated
      Bytes({0xF4, 0x90, 0x80, 0x80}),        // U+110000
      Bytes({0xF5, 0x80, 0x80, 0x80}),        // beyond the last lead byte
      Bytes({0xF8, 0x88, 0x80, 0x80, 0x80}),  // five-byte sequence
      Bytes({'a', 0xC3, 0x28}),               // continuation byte is not a continuation
  };
  for (const std::string& text : rejected) {
    ECG_CHECK_MSG(!ecg::IsValidUtf8(text), "must be rejected as UTF-8");
  }
}

ECG_TEST("json.escape_encodes_control_characters_and_preserves_utf8") {
  ECG_CHECK_EQ(ecg::JsonEscape(std::string_view()), std::string());
  ECG_CHECK_EQ(ecg::JsonEscape(Bytes({0x01})), std::string("\\u0001"));
  ECG_CHECK_EQ(ecg::JsonEscape(Bytes({0x1F})), std::string("\\u001f"));
  ECG_CHECK_EQ(ecg::JsonEscape(Bytes({0x0B})), std::string("\\u000b"));
  ECG_CHECK_EQ(ecg::JsonEscape(Bytes({0x00})), std::string("\\u0000"));
  ECG_CHECK_EQ(ecg::JsonEscape(std::string("\b\f\n\r\t")), std::string("\\b\\f\\n\\r\\t"));
  ECG_CHECK_EQ(ecg::JsonEscape(std::string("\"\\")), std::string("\\\"\\\\"));
  ECG_CHECK_EQ(ecg::JsonEscape(std::string_view("/")), std::string("/"));
  ECG_CHECK_EQ(ecg::JsonEscape(std::string_view("plain")), std::string("plain"));

  // Ordinary UTF-8 passes through untouched, byte for byte.
  const std::string utf8 = Bytes({0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80});
  ECG_CHECK_EQ(ecg::JsonEscape(utf8), utf8);

  // Escaping then parsing returns the original bytes.
  const std::string escaped = ecg::JsonEscape(std::string("a\x01\"b\\c\n"));
  const ecg::JsonValue reparsed = ParseOk("\"" + escaped + "\"");
  ECG_CHECK_EQ(reparsed.as_string(), std::string("a\x01\"b\\c\n"));
}
