// Tests for strongly typed identities and checked fixed-point quantities.
//
// Everything here asserts observable behaviour of the public surface: what the
// parsers accept and reject, what the formatters emit, where the checked
// arithmetic refuses to produce a value, and whether the wide intermediate of
// CheckedMulDiv agrees with an independently written 128-bit oracle.

#include "test.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/strong.hpp"

namespace {

constexpr std::int64_t kInt64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kInt64Min = (std::numeric_limits<std::int64_t>::min)();
constexpr std::uint64_t kUint64Max = (std::numeric_limits<std::uint64_t>::max)();

// ---------------------------------------------------------------------------
// An independent 128-bit oracle for CheckedMulDiv.
//
// The library builds its product from 32-bit limbs and divides with a restoring
// shift/subtract loop. The oracle below builds the product from 16-bit limbs
// (schoolbook) and divides by subtracting shifted divisors from the top bit
// down, so a coding error in one decomposition does not reproduce itself in the
// other. No compiler builtin, no __int128, no floating point.
// ---------------------------------------------------------------------------

struct U128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};
};

/// Full 64x64 -> 128 product, base 2^16.
[[nodiscard]] U128 MultiplyRef(std::uint64_t a, std::uint64_t b) {
  std::uint64_t limbs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t ai = (a >> (16 * i)) & 0xFFFFu;
    std::uint64_t carry = 0;
    for (int j = 0; j < 4; ++j) {
      const std::uint64_t bj = (b >> (16 * j)) & 0xFFFFu;
      const std::uint64_t current = limbs[i + j] + ai * bj + carry;
      limbs[i + j] = current & 0xFFFFu;
      carry = current >> 16;
    }
    int k = i + 4;
    while (carry != 0 && k < 8) {
      const std::uint64_t current = limbs[k] + carry;
      limbs[k] = current & 0xFFFFu;
      carry = current >> 16;
      ++k;
    }
  }
  U128 out;
  for (int i = 3; i >= 0; --i) {
    out.lo = (out.lo << 16) | limbs[i];
  }
  for (int i = 7; i >= 4; --i) {
    out.hi = (out.hi << 16) | limbs[i];
  }
  return out;
}

[[nodiscard]] bool IsLess(U128 a, U128 b) {
  if (a.hi != b.hi) {
    return a.hi < b.hi;
  }
  return a.lo < b.lo;
}

/// a - b; the caller must have established a >= b.
[[nodiscard]] U128 SubtractRef(U128 a, U128 b) {
  U128 out;
  out.lo = a.lo - b.lo;
  const std::uint64_t borrow = a.lo < b.lo ? 1u : 0u;
  out.hi = a.hi - b.hi - borrow;
  return out;
}

/// value << bits. Returns false when the result would not fit in 128 bits;
/// a truncating shift would silently turn a large divisor into a small one.
[[nodiscard]] bool ShiftLeftRef(U128 value, unsigned bits, U128* out) {
  if (bits == 0) {
    *out = value;
    return true;
  }
  if (bits >= 128) {
    return value.hi == 0 && value.lo == 0;
  }
  if (bits >= 64) {
    const unsigned upper = bits - 64u;
    if (upper != 0 && (value.lo >> (64u - upper)) != 0u) {
      return false;
    }
    out->hi = value.lo << upper;
    out->lo = 0;
    return true;
  }
  if ((value.hi >> (64u - bits)) != 0u) {
    return false;
  }
  out->hi = (value.hi << bits) | (value.lo >> (64u - bits));
  out->lo = value.lo << bits;
  return true;
}

/// Floor division of a 128-bit dividend by a non-zero 64-bit divisor.
/// Returns false when the quotient does not fit in 64 bits.
[[nodiscard]] bool DivideRef(U128 dividend, std::uint64_t divisor, std::uint64_t* quotient) {
  const U128 step{0, divisor};
  U128 rest = dividend;
  U128 result;
  for (int bit = 127; bit >= 0; --bit) {
    U128 shifted;
    if (!ShiftLeftRef(step, static_cast<unsigned>(bit), &shifted)) {
      continue;  // divisor << bit exceeds 128 bits, so it exceeds the dividend.
    }
    if (!IsLess(rest, shifted)) {
      rest = SubtractRef(rest, shifted);
      if (bit >= 64) {
        result.hi |= (std::uint64_t{1} << (bit - 64));
      } else {
        result.lo |= (std::uint64_t{1} << bit);
      }
    }
  }
  *quotient = result.lo;
  return result.hi == 0;
}

[[nodiscard]] std::uint64_t Magnitude(std::int64_t value) {
  return value < 0 ? (~static_cast<std::uint64_t>(value) + 1u) : static_cast<std::uint64_t>(value);
}

/// The value a truncated (a * b) / d produces once the sign is applied.
[[nodiscard]] std::int64_t SignedFromMagnitude(std::uint64_t magnitude, bool negative) {
  return negative ? static_cast<std::int64_t>(~magnitude + 1u)
                  : static_cast<std::int64_t>(magnitude);
}

/// CheckedProduct spells its template arguments with commas, which a function
/// like macro would split into separate arguments; naming the instantiation once
/// keeps the assertion call sites free of that trap.
[[nodiscard]] ecg::Result<ecg::MoneyMicros> MoneyFromPriceAndEnergy(
    ecg::PriceMicrosPerKwh price, ecg::EnergyKwh energy) {
  return ecg::CheckedProduct<ecg::MoneyTag, ecg::PriceTag, ecg::EnergyKwhTag, 3>(price, energy);
}

}  // namespace

// ---------------------------------------------------------------------------
// NameId
// ---------------------------------------------------------------------------

ECG_TEST("strong.name_id_parse_accepts_valid_identifiers_byte_for_byte") {
  const std::string text = "Tariff_1.2-3:zone/eu@west";
  const auto parsed = ecg::TariffId::Parse(text);
  ECG_CHECK_OK(parsed);
  if (parsed.ok()) {
    ECG_CHECK_EQ(parsed.value().value(), text);
    ECG_CHECK_EQ(parsed.value().ToString(), text);
    ECG_CHECK_EQ(std::string(parsed.value().c_str()), text);
  }

  // Sixty-four characters is the inclusive maximum, and one is the minimum.
  const std::string longest(ecg::TariffId::kMaxLength, 'a');
  ECG_CHECK(ecg::IsValidNameId(longest));
  ECG_CHECK_OK(ecg::TariffId::Parse(longest));
  ECG_CHECK(ecg::IsValidNameId("z"));
  ECG_CHECK_OK(ecg::ZoneId::Parse("Europe/Berlin"));
  ECG_CHECK_OK(ecg::SourceId::Parse("meter-42_channel.1"));
}

ECG_TEST("strong.name_id_parse_rejects_empty_and_overlong") {
  ECG_CHECK_ERR(ecg::TariffId::Parse(""), ecg::ErrorCode::kMissingRequiredField);
  ECG_CHECK_ERR(ecg::TariffId::Parse(std::string(ecg::TariffId::kMaxLength + 1u, 'a')),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::TariffId::Parse(std::string(200, 'a')), ecg::ErrorCode::kOutOfRange);
  ECG_CHECK(!ecg::IsValidNameId(""));
  ECG_CHECK(!ecg::IsValidNameId(std::string(ecg::TariffId::kMaxLength + 1u, 'a')));
}

ECG_TEST("strong.name_id_parse_rejects_disallowed_characters") {
  const std::string control_nul("nul\0byte", 8);
  const std::string high_byte("\x80", 1);
  const std::string delete_byte("\x7f", 1);

  ECG_CHECK_ERR(ecg::SourceId::Parse("has space"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::SourceId::Parse("tab\tseparated"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::SourceId::Parse("line\nbreak"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::SourceId::Parse(control_nul), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::SourceId::Parse(high_byte), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::SourceId::Parse(delete_byte), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::PolicyId::Parse("semi;colon"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::ServiceClassId::Parse("plus+sign"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::ZoneId::Parse("brace{}"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::DemandWindowId::Parse("pipe|char"), ecg::ErrorCode::kMalformedInput);
}

// ---------------------------------------------------------------------------
// Id<Tag>
// ---------------------------------------------------------------------------

ECG_TEST("strong.id_parse_accepts_sixteen_hex_digits_and_literal_zero") {
  const auto zero = ecg::RequestId::Parse("0");
  ECG_CHECK_OK(zero);
  if (zero.ok()) {
    ECG_CHECK(zero.value().is_unset());
    ECG_CHECK_EQ(zero.value().raw(), std::uint64_t{0});
  }

  const auto value = ecg::RequestId::Parse("0123456789abcdef");
  ECG_CHECK_OK(value);
  if (value.ok()) {
    ECG_CHECK(value.value().is_set());
    ECG_CHECK_EQ(value.value().ToString(), std::string("0123456789abcdef"));
  }

  // Upper-case hex digits are accepted and rendered lower case.
  const auto upper = ecg::DecisionId::Parse("ABCDEF0123456789");
  ECG_CHECK_OK(upper);
  if (upper.ok()) {
    ECG_CHECK_EQ(upper.value().raw(), std::uint64_t{0xABCDEF0123456789ull});
    ECG_CHECK_EQ(upper.value().ToString(), std::string("abcdef0123456789"));
  }
}

ECG_TEST("strong.id_parse_rejects_wrong_lengths_and_non_hex") {
  ECG_CHECK_ERR(ecg::RequestId::Parse(""), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("0123456789abcde"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("0123456789abcdef0"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("00"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("0123456789abcdeg"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("0x123456789abcd"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::RequestId::Parse("0123456789abcde "), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::IntentId::Parse("-123456789abcdef"), ecg::ErrorCode::kMalformedInput);
}

ECG_TEST("strong.id_tostring_and_parse_round_trip") {
  const std::uint64_t raws[] = {0u, 1u, 0xdeadbeefcafebabeull, kUint64Max};
  for (const std::uint64_t raw : raws) {
    const ecg::DecisionId id = ecg::DecisionId::FromRaw(raw);
    const std::string text = id.ToString();
    ECG_CHECK_EQ(text.size(), static_cast<std::size_t>(16));
    const auto parsed = ecg::DecisionId::Parse(text);
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK_EQ(parsed.value().raw(), raw);
    }
  }
}

// ---------------------------------------------------------------------------
// Counter<Tag>
// ---------------------------------------------------------------------------

ECG_TEST("strong.counter_next_returns_successor_and_fails_at_max") {
  const auto first = ecg::PolicyGeneration::FromRaw(0).Next();
  ECG_CHECK_OK(first);
  if (first.ok()) {
    ECG_CHECK_EQ(first.value().raw(), std::uint64_t{1});
  }

  const auto last = ecg::PolicyGeneration::FromRaw(kUint64Max - 1u).Next();
  ECG_CHECK_OK(last);
  if (last.ok()) {
    ECG_CHECK_EQ(last.value().raw(), kUint64Max);
  }

  ECG_CHECK_ERR(ecg::PolicyGeneration::FromRaw(kUint64Max).Next(),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::Epoch::FromRaw(kUint64Max).Next(), ecg::ErrorCode::kNumericOverflow);

  // A failed Next() leaves the counter untouched: the successor of the
  // predecessor of the maximum is still the maximum.
  ECG_CHECK_EQ(ecg::Epoch::FromRaw(kUint64Max).raw(), kUint64Max);
}

ECG_TEST("strong.counter_parse_round_trips_decimal_literals") {
  const auto parsed = ecg::JournalSequence::Parse("18446744073709551615");
  ECG_CHECK_OK(parsed);
  if (parsed.ok()) {
    ECG_CHECK_EQ(parsed.value().raw(), kUint64Max);
    ECG_CHECK_EQ(parsed.value().ToString(), std::string("18446744073709551615"));
  }
  ECG_CHECK_ERR(ecg::JournalSequence::Parse(""), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JournalSequence::Parse("12a"), ecg::ErrorCode::kMalformedInput);
  ECG_CHECK_ERR(ecg::JournalSequence::Parse("18446744073709551616"),
                ecg::ErrorCode::kNumericOverflow);
}

// ---------------------------------------------------------------------------
// Quantity formatting and parsing
// ---------------------------------------------------------------------------

ECG_TEST("quantity.format_produces_exact_decimal_strings") {
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(0)), std::string("0"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(-1)), std::string("-0.001"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(1)), std::string("0.001"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(1234500)), std::string("1234.5"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(-1234500)), std::string("-1234.5"));

  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(0)), std::string("0"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(1)), std::string("0.000001"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(1500000)), std::string("1.5"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(-1000000)), std::string("-1"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(1234567)),
               std::string("1.234567"));

  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::DurationSec::FromRaw(0)), std::string("0"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::DurationSec::FromRaw(1234)), std::string("1234"));

  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::RatioPpm::FromRaw(1000000)), std::string("1"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::RatioPpm::FromRaw(1)), std::string("0.000001"));

  // Trailing zeros are trimmed, never padded, and no exponent ever appears.
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(1500)), std::string("1.5"));
  ECG_CHECK_EQ(ecg::FormatQuantity(ecg::PowerKw::FromRaw(1000)), std::string("1"));
}

ECG_TEST("quantity.format_and_parse_round_trip_at_every_scale") {
  const std::int64_t power_raws[] = {0, 1, -1, 999, 1000, -1000, 1234500, -1234500,
                                     1000000000000000LL, -1000000000000000LL};
  for (const std::int64_t raw : power_raws) {
    const auto parsed = ecg::ParseQuantity<ecg::PowerKwTag>(ecg::FormatQuantity(ecg::PowerKw::FromRaw(raw)));
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK_EQ(parsed.value().raw(), raw);
    }
  }

  const std::int64_t price_raws[] = {0, 1, -1, -1500000, 1500000, 1234567, -1000000000000LL,
                                     1000000000000LL};
  for (const std::int64_t raw : price_raws) {
    const auto parsed = ecg::ParseQuantity<ecg::PriceTag>(
        ecg::FormatQuantity(ecg::PriceMicrosPerKwh::FromRaw(raw)));
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK_EQ(parsed.value().raw(), raw);
    }
  }

  const std::int64_t duration_raws[] = {0, 1, 60, 3600, 1000000000000LL};
  for (const std::int64_t raw : duration_raws) {
    const auto parsed = ecg::ParseQuantity<ecg::DurationTag>(
        ecg::FormatQuantity(ecg::DurationSec::FromRaw(raw)));
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK_EQ(parsed.value().raw(), raw);
    }
  }

  const std::int64_t ratio_raws[] = {0, 1, -1, 1000000, -1000000, 999999999999LL};
  for (const std::int64_t raw : ratio_raws) {
    const auto parsed =
        ecg::ParseQuantity<ecg::RatioTag>(ecg::FormatQuantity(ecg::RatioPpm::FromRaw(raw)));
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK_EQ(parsed.value().raw(), raw);
    }
  }
}

ECG_TEST("quantity.parse_scales_fractional_digits_exactly") {
  const auto zero = ecg::ParseQuantity<ecg::PowerKwTag>("0");
  ECG_CHECK_OK(zero);
  if (zero.ok()) {
    ECG_CHECK_EQ(zero.value().raw(), std::int64_t{0});
  }

  const auto negative = ecg::ParseQuantity<ecg::PowerKwTag>("-0.001");
  ECG_CHECK_OK(negative);
  if (negative.ok()) {
    ECG_CHECK_EQ(negative.value().raw(), std::int64_t{-1});
  }

  const auto large = ecg::ParseQuantity<ecg::PowerKwTag>("1234.5");
  ECG_CHECK_OK(large);
  if (large.ok()) {
    ECG_CHECK_EQ(large.value().raw(), std::int64_t{1234500});
  }

  const auto micro = ecg::ParseQuantity<ecg::PriceTag>("0.000001");
  ECG_CHECK_OK(micro);
  if (micro.ok()) {
    ECG_CHECK_EQ(micro.value().raw(), std::int64_t{1});
  }

  // A short fraction is scaled up to the declared scale.
  const auto short_fraction = ecg::ParseQuantity<ecg::PriceTag>("1.5");
  ECG_CHECK_OK(short_fraction);
  if (short_fraction.ok()) {
    ECG_CHECK_EQ(short_fraction.value().raw(), std::int64_t{1500000});
  }

  // Duration is a whole-second quantity: a fraction of a second is rejected.
  const auto whole = ecg::ParseQuantity<ecg::DurationTag>("90");
  ECG_CHECK_OK(whole);
  if (whole.ok()) {
    ECG_CHECK_EQ(whole.value().raw(), std::int64_t{90});
  }
  ECG_CHECK(!ecg::ParseQuantity<ecg::DurationTag>("90.5").ok());
}

ECG_TEST("quantity.parse_rejects_malformed_literals") {
  const char* const rejected[] = {
      "",      "+",     "-",      ".5",     "5.",     "1e3",    "1E3",   "1.5e3",
      "1.2345", "0x10", " 1",     "1 ",     "1_000",  "--1",    "1.2.3", "nan",
      "inf",   "1,5",   "12345678901234567890123456789012345678901234",
  };
  for (const char* const text : rejected) {
    const auto parsed = ecg::ParseQuantity<ecg::PowerKwTag>(text);
    ECG_CHECK_MSG(!parsed.ok(), std::string("literal must be rejected: ") + text);
  }

  // Non-ASCII digits are not digits here.
  const std::string arabic_indic("\xD9\xA0", 2);  // U+0660 ARABIC-INDIC DIGIT ZERO
  ECG_CHECK(!ecg::ParseQuantity<ecg::PowerKwTag>(arabic_indic).ok());
  ECG_CHECK(!ecg::ParseQuantity<ecg::PowerKwTag>("5" + arabic_indic).ok());
  ECG_CHECK(!ecg::ParseQuantity<ecg::PowerKwTag>(std::string("\xC2\xB2", 2)).ok());
}

ECG_TEST("quantity.parse_rejects_values_outside_the_declared_domain") {
  // Durations are non-negative: the parser must not mint a negative one.
  ECG_CHECK_ERR(ecg::ParseQuantity<ecg::DurationTag>("-1"), ecg::ErrorCode::kOutOfRange);
  // A fractional digit in a whole-second quantity is out of scale, not merely
  // malformed, and it must still be refused.
  ECG_CHECK_ERR(ecg::ParseQuantity<ecg::DurationTag>("-0.5"), ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::ParseQuantity<ecg::DurationTag>("1000000000001"),
                ecg::ErrorCode::kOutOfRange);

  // Power is bounded at +/-10^15 raw (10^12 kW).
  ECG_CHECK_ERR(ecg::ParseQuantity<ecg::PowerKwTag>("1000000000001"),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::ParseQuantity<ecg::PowerKwTag>("-1000000000001"),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_OK(ecg::ParseQuantity<ecg::PowerKwTag>("1000000000000"));
  ECG_CHECK_OK(ecg::ParseQuantity<ecg::PowerKwTag>("-1000000000000"));
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

ECG_TEST("quantity.checked_add_reports_overflow_and_domain_violations") {
  ECG_CHECK_ERR(ecg::CheckedAdd(ecg::UtcInstant::FromRaw(kInt64Max), ecg::UtcInstant::FromRaw(1)),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::CheckedAdd(ecg::UtcInstant::FromRaw(kInt64Min), ecg::UtcInstant::FromRaw(-1)),
                ecg::ErrorCode::kNumericOverflow);

  ECG_CHECK_ERR(ecg::CheckedAdd(ecg::PowerKw::FromRaw(1000000000000000LL),
                                ecg::PowerKw::FromRaw(1)),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::CheckedAdd(ecg::DurationSec::FromRaw(0), ecg::DurationSec::FromRaw(-1)),
                ecg::ErrorCode::kOutOfRange);

  const auto sum = ecg::CheckedAdd(ecg::PowerKw::FromRaw(1500), ecg::PowerKw::FromRaw(-500));
  ECG_CHECK_OK(sum);
  if (sum.ok()) {
    ECG_CHECK_EQ(sum.value().raw(), std::int64_t{1000});
  }
  const auto boundary = ecg::CheckedAdd(ecg::PowerKw::FromRaw(999999999999999LL),
                                        ecg::PowerKw::FromRaw(1));
  ECG_CHECK_OK(boundary);
  if (boundary.ok()) {
    ECG_CHECK_EQ(boundary.value().raw(), std::int64_t{1000000000000000LL});
  }
}

ECG_TEST("quantity.checked_sub_reports_overflow_and_domain_violations") {
  ECG_CHECK_ERR(
      ecg::CheckedSub(ecg::UtcInstant::FromRaw(kInt64Min), ecg::UtcInstant::FromRaw(1)),
      ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(
      ecg::CheckedSub(ecg::UtcInstant::FromRaw(kInt64Max), ecg::UtcInstant::FromRaw(-1)),
      ecg::ErrorCode::kNumericOverflow);

  ECG_CHECK_ERR(ecg::CheckedSub(ecg::PowerKw::FromRaw(-1000000000000000LL),
                                ecg::PowerKw::FromRaw(1)),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::CheckedSub(ecg::DurationSec::FromRaw(0), ecg::DurationSec::FromRaw(1)),
                ecg::ErrorCode::kOutOfRange);

  const auto difference =
      ecg::CheckedSub(ecg::DurationSec::FromRaw(3600), ecg::DurationSec::FromRaw(60));
  ECG_CHECK_OK(difference);
  if (difference.ok()) {
    ECG_CHECK_EQ(difference.value().raw(), std::int64_t{3540});
  }
}

ECG_TEST("quantity.checked_mul_reports_overflow_and_domain_violations") {
  ECG_CHECK_ERR(ecg::CheckedMul(ecg::UtcInstant::FromRaw(kInt64Max), 2),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::CheckedMul(ecg::UtcInstant::FromRaw(kInt64Min), -1),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::CheckedMul(ecg::PowerKw::FromRaw(1000000000000000LL), 2),
                ecg::ErrorCode::kOutOfRange);

  const auto product = ecg::CheckedMul(ecg::PowerKw::FromRaw(2500), -4);
  ECG_CHECK_OK(product);
  if (product.ok()) {
    ECG_CHECK_EQ(product.value().raw(), std::int64_t{-10000});
  }
  const auto zero = ecg::CheckedMul(ecg::UtcInstant::FromRaw(kInt64Min), 0);
  ECG_CHECK_OK(zero);
  if (zero.ok()) {
    ECG_CHECK_EQ(zero.value().raw(), std::int64_t{0});
  }
}

ECG_TEST("quantity.checked_div_reports_zero_overflow_and_domain_violations") {
  ECG_CHECK_ERR(ecg::CheckedDiv(ecg::PowerKw::FromRaw(1000), 0),
                ecg::ErrorCode::kDivisionByZero);
  ECG_CHECK_ERR(ecg::CheckedDiv(ecg::UtcInstant::FromRaw(kInt64Min), -1),
                ecg::ErrorCode::kNumericOverflow);

  const auto truncated = ecg::CheckedDiv(ecg::PowerKw::FromRaw(-7000), 2);
  ECG_CHECK_OK(truncated);
  if (truncated.ok()) {
    ECG_CHECK_EQ(truncated.value().raw(), std::int64_t{-3500});
  }

  // A negative divisor drives this duration below its declared minimum of zero
  // seconds. Every other checked operation reports that; division must too.
  ECG_CHECK_ERR(ecg::CheckedDiv(ecg::DurationSec::FromRaw(9), -2),
                ecg::ErrorCode::kOutOfRange);
}

// ---------------------------------------------------------------------------
// CheckedMulDiv
// ---------------------------------------------------------------------------

ECG_TEST("quantity.checked_mul_div_exact_cases") {
  const auto six_seven_three = ecg::CheckedMulDiv(6, 7, 3);
  ECG_CHECK_OK(six_seven_three);
  if (six_seven_three.ok()) {
    ECG_CHECK_EQ(six_seven_three.value(), std::int64_t{14});
  }

  const auto one_hundred = ecg::CheckedMulDiv(1000, 1000, 10000);
  ECG_CHECK_OK(one_hundred);
  if (one_hundred.ok()) {
    ECG_CHECK_EQ(one_hundred.value(), std::int64_t{100});
  }

  const auto zero_numerator = ecg::CheckedMulDiv(0, kInt64Min, -1);
  ECG_CHECK_OK(zero_numerator);
  if (zero_numerator.ok()) {
    ECG_CHECK_EQ(zero_numerator.value(), std::int64_t{0});
  }

  const auto min_times_min = ecg::CheckedMulDiv(kInt64Min, kInt64Min, kInt64Min);
  ECG_CHECK_OK(min_times_min);
  if (min_times_min.ok()) {
    ECG_CHECK_EQ(min_times_min.value(), kInt64Min);
  }

  ECG_CHECK_ERR(ecg::CheckedMulDiv(1, 1, 0), ecg::ErrorCode::kDivisionByZero);
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kInt64Min, 1, -1), ecg::ErrorCode::kNumericOverflow);
}

ECG_TEST("quantity.checked_mul_div_truncates_toward_zero") {
  const struct {
    std::int64_t a;
    std::int64_t b;
    std::int64_t divisor;
    std::int64_t expected;
  } cases[] = {
      {-7, 1, 2, -3},   {7, 1, -2, -3},   {-7, -1, 2, 3},  {7, -1, -2, 3},
      {-1, 7, 2, -3},   {1, -7, 2, -3},   {5, 5, 4, 6},    {-5, 5, 4, -6},
      {1, 1, 2, 0},     {-1, 1, 2, 0},    {1, -1, 2, 0},   {-9, 9, -8, 10},
      {kInt64Max, 1, kInt64Max, 1},       {kInt64Min, 1, kInt64Max, -1},
  };
  for (const auto& item : cases) {
    const auto result = ecg::CheckedMulDiv(item.a, item.b, item.divisor);
    ECG_CHECK_OK(result);
    if (result.ok()) {
      ECG_CHECK_EQ(result.value(), item.expected);
    }
  }
}

ECG_TEST("quantity.checked_mul_div_boundaries_of_the_representable_range") {
  // 3037000499^2 = 9223372030926249001 is the largest square that fits int64.
  constexpr std::int64_t kRoot = 3037000499LL;
  const auto largest_positive = ecg::CheckedMulDiv(kRoot, kRoot, 1);
  ECG_CHECK_OK(largest_positive);
  if (largest_positive.ok()) {
    ECG_CHECK_EQ(largest_positive.value(), std::int64_t{9223372030926249001LL});
  }

  const auto largest_negative = ecg::CheckedMulDiv(kRoot, kRoot, -1);
  ECG_CHECK_OK(largest_negative);
  if (largest_negative.ok()) {
    ECG_CHECK_EQ(largest_negative.value(), std::int64_t{-9223372030926249001LL});
  }

  // One past the maximum in each direction.
  constexpr std::int64_t kOnePastRoot = 3037000500LL;
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kOnePastRoot, kOnePastRoot, 1),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kOnePastRoot, kOnePastRoot, -1),
                ecg::ErrorCode::kNumericOverflow);

  // Exactly the signed limits are representable.
  const auto exactly_max = ecg::CheckedMulDiv(kInt64Max, 1, 1);
  ECG_CHECK_OK(exactly_max);
  if (exactly_max.ok()) {
    ECG_CHECK_EQ(exactly_max.value(), kInt64Max);
  }
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kInt64Max, 2, 1), ecg::ErrorCode::kNumericOverflow);

  const auto exactly_min = ecg::CheckedMulDiv(kInt64Min, 1, 1);
  ECG_CHECK_OK(exactly_min);
  if (exactly_min.ok()) {
    ECG_CHECK_EQ(exactly_min.value(), kInt64Min);
  }
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kInt64Min, 2, 1), ecg::ErrorCode::kNumericOverflow);

  // A huge product whose quotient is still representable must not be rejected.
  const auto large_but_fine = ecg::CheckedMulDiv(kInt64Max, kInt64Max, kInt64Max);
  ECG_CHECK_OK(large_but_fine);
  if (large_but_fine.ok()) {
    ECG_CHECK_EQ(large_but_fine.value(), kInt64Max);
  }
  // 2^126 / (2^63 - 1) = 2^63 + 1 exactly (remainder 1), one past the maximum.
  ECG_CHECK_ERR(ecg::CheckedMulDiv(kInt64Min, kInt64Min, kInt64Max),
                ecg::ErrorCode::kNumericOverflow);
  const auto min_over_min = ecg::CheckedMulDiv(kInt64Min, kInt64Min, kInt64Min);
  ECG_CHECK_OK(min_over_min);
  if (min_over_min.ok()) {
    ECG_CHECK_EQ(min_over_min.value(), kInt64Min);
  }
}

ECG_TEST("quantity.mul_div_oracle_agrees_with_hand_computed_products") {
  // Guards the oracle itself: a broken reference would make the randomised
  // comparison below meaningless.
  const U128 all_ones = MultiplyRef(kUint64Max, kUint64Max);
  ECG_CHECK_EQ(all_ones.hi, std::uint64_t{0xFFFFFFFFFFFFFFFEull});
  ECG_CHECK_EQ(all_ones.lo, std::uint64_t{1});

  const U128 square = MultiplyRef(3037000499u, 3037000499u);
  ECG_CHECK_EQ(square.hi, std::uint64_t{0});
  ECG_CHECK_EQ(square.lo, std::uint64_t{9223372030926249001ull});

  std::uint64_t quotient = 0;
  ECG_CHECK(DivideRef(U128{0, 6}, 3, &quotient));
  ECG_CHECK_EQ(quotient, std::uint64_t{2});
  ECG_CHECK(DivideRef(U128{0, square.lo}, 3037000499u, &quotient));
  ECG_CHECK_EQ(quotient, std::uint64_t{3037000499u});
  ECG_CHECK(!DivideRef(U128{1, 0}, 1, &quotient));  // 2^64 does not fit in 64 bits.
  ECG_CHECK(DivideRef(U128{0, kUint64Max}, kUint64Max, &quotient));
  ECG_CHECK_EQ(quotient, std::uint64_t{1});
}

ECG_TEST("quantity.checked_mul_div_matches_an_independent_oracle") {
  ecgtest::Random random(0x5EEDF00DCAFEBABEull);
  int exact_cases = 0;
  int overflow_cases = 0;

  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::int64_t a = 0;
    std::int64_t b = 0;
    std::int64_t divisor = 1;
    switch (iteration % 4) {
      case 0:
        a = random.Range(-1000, 1000);
        b = random.Range(-1000, 1000);
        divisor = random.Range(-1000, 1000);
        break;
      case 1:
        a = random.Range(-1000000000, 1000000000);
        b = random.Range(-1000000000, 1000000000);
        divisor = random.Range(-1000000000, 1000000000);
        break;
      case 2:
        a = static_cast<std::int64_t>(random.Next());
        b = static_cast<std::int64_t>(random.Next());
        divisor = static_cast<std::int64_t>(random.Next());
        break;
      default:
        a = random.Coin() ? kInt64Min : kInt64Max;
        b = random.Range(-3, 3);
        divisor = random.Range(-3, 3);
        break;
    }
    if (divisor == 0) {
      divisor = 1;
    }

    const bool negative_product = (a < 0) != (b < 0);
    const bool negative_result = negative_product != (divisor < 0);
    const U128 product = MultiplyRef(Magnitude(a), Magnitude(b));
    std::uint64_t quotient = 0;
    const bool quotient_fits = DivideRef(product, Magnitude(divisor), &quotient);
    const std::uint64_t limit =
        negative_result ? static_cast<std::uint64_t>(kInt64Max) + 1u
                        : static_cast<std::uint64_t>(kInt64Max);

    const auto result = ecg::CheckedMulDiv(a, b, divisor);
    if (!quotient_fits || quotient > limit) {
      ++overflow_cases;
      ECG_CHECK_ERR(result, ecg::ErrorCode::kNumericOverflow);
      continue;
    }

    ++exact_cases;
    ECG_CHECK_MSG(result.ok(), "expected a representable quotient");
    if (!result.ok()) {
      continue;
    }
    ECG_CHECK_EQ(result.value(), SignedFromMagnitude(quotient, negative_result));

    // Truncated quotient property, checked without any 128-bit division:
    // q * |d| <= |a * b| < (q + 1) * |d|.
    const U128 lower = MultiplyRef(quotient, Magnitude(divisor));
    ECG_CHECK_MSG(!IsLess(product, lower), "quotient is larger than the exact value");
    if (quotient != kUint64Max) {
      const U128 upper = MultiplyRef(quotient + 1u, Magnitude(divisor));
      ECG_CHECK_MSG(IsLess(product, upper), "quotient is smaller than the truncated value");
    }
  }

  ECG_CHECK_MSG(exact_cases > 3000, "the sample must exercise the exact path");
  ECG_CHECK_MSG(overflow_cases > 100, "the sample must exercise the overflow path");
}

// ---------------------------------------------------------------------------
// Composite checked operations
// ---------------------------------------------------------------------------

ECG_TEST("quantity.checked_product_of_price_and_energy_yields_money") {
  const auto money =
      MoneyFromPriceAndEnergy(ecg::PriceMicrosPerKwh::FromRaw(500000), ecg::EnergyKwh::FromRaw(2000));
  ECG_CHECK_OK(money);
  if (money.ok()) {
    ECG_CHECK_EQ(money.value().raw(), std::int64_t{1000000});
    ECG_CHECK_EQ(ecg::FormatQuantity(money.value()), std::string("1"));
  }

  const auto negative = MoneyFromPriceAndEnergy(ecg::PriceMicrosPerKwh::FromRaw(-1500000),
                                                ecg::EnergyKwh::FromRaw(4000));
  ECG_CHECK_OK(negative);
  if (negative.ok()) {
    ECG_CHECK_EQ(negative.value().raw(), std::int64_t{-6000000});
    ECG_CHECK_EQ(ecg::FormatQuantity(negative.value()), std::string("-6"));
  }

  const auto zero = MoneyFromPriceAndEnergy(ecg::PriceMicrosPerKwh::FromRaw(0),
                                            ecg::EnergyKwh::FromRaw(1000000000000000LL));
  ECG_CHECK_OK(zero);
  if (zero.ok()) {
    ECG_CHECK_EQ(zero.value().raw(), std::int64_t{0});
  }

  // Out of the money domain: 10^12 * 2*10^6 / 10^3 = 2*10^15 micros.
  ECG_CHECK_ERR(MoneyFromPriceAndEnergy(ecg::PriceMicrosPerKwh::FromRaw(1000000000000LL),
                                        ecg::EnergyKwh::FromRaw(2000000)),
                ecg::ErrorCode::kOutOfRange);

  // Beyond 64 bits entirely: 10^12 * 10^15 / 10^3 = 10^24 micros.
  ECG_CHECK_ERR(MoneyFromPriceAndEnergy(ecg::PriceMicrosPerKwh::FromRaw(1000000000000LL),
                                        ecg::EnergyKwh::FromRaw(1000000000000000LL)),
                ecg::ErrorCode::kNumericOverflow);
}

ECG_TEST("quantity.checked_scale_applies_an_exact_rational_scale") {
  const auto scaled = ecg::CheckedScale(ecg::PowerKw::FromRaw(1000), 3, 2);
  ECG_CHECK_OK(scaled);
  if (scaled.ok()) {
    ECG_CHECK_EQ(scaled.value().raw(), std::int64_t{1500});
  }

  const auto reduced = ecg::CheckedScale(ecg::PowerKw::FromRaw(1000), 1, 3);
  ECG_CHECK_OK(reduced);
  if (reduced.ok()) {
    ECG_CHECK_EQ(reduced.value().raw(), std::int64_t{333});
  }

  const auto negative = ecg::CheckedScale(ecg::PowerKw::FromRaw(-1000), -3, 2);
  ECG_CHECK_OK(negative);
  if (negative.ok()) {
    ECG_CHECK_EQ(negative.value().raw(), std::int64_t{1500});
  }

  ECG_CHECK_ERR(ecg::CheckedScale(ecg::PowerKw::FromRaw(1000), 3, 0),
                ecg::ErrorCode::kDivisionByZero);
  ECG_CHECK_ERR(ecg::CheckedScale(ecg::UtcInstant::FromRaw(kInt64Max), 2, 1),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::CheckedScale(ecg::PowerKw::FromRaw(1000000000000000LL), 2, 1),
                ecg::ErrorCode::kOutOfRange);
}
