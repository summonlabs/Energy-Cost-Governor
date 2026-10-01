#ifndef ECG_QUANTITY_HPP
#define ECG_QUANTITY_HPP

// Checked, fixed-point physical and economic quantities.
//
// Energy-cost decisions must be reproducible bit for bit: the same evidence and
// policy have to yield the same authorisation on every host, in every process,
// and after any restart. Binary floating point cannot promise that, so every
// quantity in this runtime is an exact scaled integer with an explicit unit, a
// documented scale, and enforced domain bounds. Every arithmetic operation is
// checked; overflow is a first-class failure value, never undefined behaviour
// and never silent saturation.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "ecg/result.hpp"

namespace ecg {

/// Per-quantity metadata: display scale, unit, and the inclusive raw range that
/// the domain accepts. Specialisations live at the bottom of this header.
template <class Tag>
struct QuantityTraits;

// ---------------------------------------------------------------------------
// Overflow-detecting 64-bit primitives.
//
// These are portable, constexpr, and free of compiler builtins or compiler
// generated traps: they must behave identically under MSVC, GCC, and Clang, and
// they must be usable in constant expressions and in hardened builds alike.
// ---------------------------------------------------------------------------

/// Computes a + b, reporting overflow instead of producing it.
[[nodiscard]] constexpr bool TryAddInt64(std::int64_t a, std::int64_t b, std::int64_t* out) noexcept {
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
  if (b > 0 && a > kMax - b) {
    return true;
  }
  if (b < 0 && a < kMin - b) {
    return true;
  }
  *out = a + b;
  return false;
}

/// Computes a - b, reporting overflow instead of producing it.
[[nodiscard]] constexpr bool TrySubInt64(std::int64_t a, std::int64_t b, std::int64_t* out) noexcept {
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
  if (b < 0 && a > kMax + b) {
    return true;
  }
  if (b > 0 && a < kMin + b) {
    return true;
  }
  *out = a - b;
  return false;
}

/// Computes a * b, reporting overflow instead of producing it.
[[nodiscard]] constexpr bool TryMulInt64(std::int64_t a, std::int64_t b, std::int64_t* out) noexcept {
  if (a == 0 || b == 0) {
    *out = 0;
    return false;
  }
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = a < 0 ? (~static_cast<std::uint64_t>(a) + 1u) : static_cast<std::uint64_t>(a);
  const std::uint64_t ub = b < 0 ? (~static_cast<std::uint64_t>(b) + 1u) : static_cast<std::uint64_t>(b);
  const std::uint64_t limit =
      negative ? static_cast<std::uint64_t>(kMax) + 1u : static_cast<std::uint64_t>(kMax);
  if (ua > limit / ub) {
    return true;
  }
  const std::uint64_t product = ua * ub;
  *out = negative ? static_cast<std::int64_t>(~product + 1u) : static_cast<std::int64_t>(product);
  return false;
}

/// 10^n as a compile-time integer constant.
[[nodiscard]] constexpr std::int64_t Pow10(unsigned exponent) noexcept {
  std::int64_t value = 1;
  for (unsigned i = 0; i < exponent; ++i) {
    value *= 10;
  }
  return value;
}

/// An exact scaled-integer quantity carrying its unit at the type level.
/// Interchanging kilowatts with kilowatt-hours is a compile error, not a
/// production incident.
template <class Tag>
class Quantity {
 public:
  using rep = std::int64_t;
  using traits = QuantityTraits<Tag>;

  constexpr Quantity() noexcept = default;

  /// Unchecked construction from raw scaled units. Callers must already have
  /// proven the value is in range (for example, immediately after a checked
  /// operation). Prefer QuantityFromRaw for untrusted input.
  [[nodiscard]] static constexpr Quantity FromRaw(rep raw) noexcept { return Quantity(raw); }

  [[nodiscard]] constexpr rep raw() const noexcept { return raw_; }

  friend constexpr bool operator==(Quantity a, Quantity b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(Quantity a, Quantity b) noexcept {
    return a.raw_ <=> b.raw_;
  }

  /// Exact negation. Callers that cannot prove the value is not the most
  /// negative raw value should use CheckedNegate instead.
  [[nodiscard]] constexpr Quantity operator-() const noexcept { return Quantity(-raw_); }

  /// True when the value is exactly zero.
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
  /// True when the value is strictly positive.
  [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }
  /// True when the value is strictly negative.
  [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }

 private:
  explicit constexpr Quantity(rep raw) noexcept : raw_(raw) {}

  rep raw_{0};
};

/// Range-checked construction from raw scaled units.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> QuantityFromRaw(typename Quantity<Tag>::rep raw) {
  if (raw < QuantityTraits<Tag>::kMinRaw || raw > QuantityTraits<Tag>::kMaxRaw) {
    return MakeError(ErrorCode::kOutOfRange, std::string(QuantityTraits<Tag>::kName),
                     "raw value " + std::to_string(raw) + " outside [" +
                         std::to_string(QuantityTraits<Tag>::kMinRaw) + ", " +
                         std::to_string(QuantityTraits<Tag>::kMaxRaw) + "]");
  }
  return Quantity<Tag>::FromRaw(raw);
}

namespace detail {

/// True when value fits the declared domain of Tag.
template <class Tag>
[[nodiscard]] constexpr bool InDomain(std::int64_t value) noexcept {
  return value >= QuantityTraits<Tag>::kMinRaw && value <= QuantityTraits<Tag>::kMaxRaw;
}

template <class Tag>
[[nodiscard]] Error DomainError(std::int64_t value) {
  return MakeError(ErrorCode::kOutOfRange, std::string(QuantityTraits<Tag>::kName),
                   "value " + std::to_string(value) + " outside [" +
                       std::to_string(QuantityTraits<Tag>::kMinRaw) + ", " +
                       std::to_string(QuantityTraits<Tag>::kMaxRaw) + "]");
}

}  // namespace detail

/// Checked addition. The result is bounds-checked in both the int64 sense and
/// the quantity's declared domain.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedAdd(Quantity<Tag> a, Quantity<Tag> b) {
  const std::int64_t x = a.raw();
  const std::int64_t y = b.raw();
  std::int64_t sum = 0;
  if (TryAddInt64(x, y, &sum)) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(QuantityTraits<Tag>::kName),
                     "addition overflow");
  }
  if (!detail::InDomain<Tag>(sum)) {
    return detail::DomainError<Tag>(sum);
  }
  return Quantity<Tag>::FromRaw(sum);
}

/// Checked subtraction.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedSub(Quantity<Tag> a, Quantity<Tag> b) {
  const std::int64_t x = a.raw();
  const std::int64_t y = b.raw();
  std::int64_t difference = 0;
  if (TrySubInt64(x, y, &difference)) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(QuantityTraits<Tag>::kName),
                     "subtraction overflow");
  }
  if (!detail::InDomain<Tag>(difference)) {
    return detail::DomainError<Tag>(difference);
  }
  return Quantity<Tag>::FromRaw(difference);
}

/// Checked negation.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedNegate(Quantity<Tag> a) {
  if (a.raw() == (std::numeric_limits<std::int64_t>::min)()) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(QuantityTraits<Tag>::kName),
                     "negation overflow");
  }
  const std::int64_t negated = -a.raw();
  if (!detail::InDomain<Tag>(negated)) {
    return detail::DomainError<Tag>(negated);
  }
  return Quantity<Tag>::FromRaw(negated);
}

/// Checked multiplication by a plain integer.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedMul(Quantity<Tag> a, std::int64_t factor) {
  std::int64_t product = 0;
  if (TryMulInt64(a.raw(), factor, &product)) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(QuantityTraits<Tag>::kName),
                     "multiplication overflow");
  }
  if (!detail::InDomain<Tag>(product)) {
    return detail::DomainError<Tag>(product);
  }
  return Quantity<Tag>::FromRaw(product);
}

/// Checked division by a plain integer, truncated toward zero. The result is
/// bounds-checked in the quantity's declared domain, so division cannot produce
/// a value the type says is impossible -- a negative DurationSec, for instance.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedDiv(Quantity<Tag> a, std::int64_t divisor) {
  if (divisor == 0) {
    return MakeError(ErrorCode::kDivisionByZero, std::string(QuantityTraits<Tag>::kName),
                     "division by zero");
  }
  if (a.raw() == (std::numeric_limits<std::int64_t>::min)() && divisor == -1) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(QuantityTraits<Tag>::kName),
                     "division overflow");
  }
  const std::int64_t quotient = a.raw() / divisor;
  if (!detail::InDomain<Tag>(quotient)) {
    return detail::DomainError<Tag>(quotient);
  }
  return Quantity<Tag>::FromRaw(quotient);
}

/// Checked (a * b) / divisor with a 128-bit intermediate, so the product never
/// silently wraps even when the final result is comfortably representable.
[[nodiscard]] Result<std::int64_t> CheckedMulDiv(std::int64_t a, std::int64_t b, std::int64_t divisor);

/// Multiplies two quantities with different units and removes ScaleShift
/// decimal places, producing a third unit. Micros-per-kWh applied to
/// milli-kWh needs a shift of 3 to yield micros.
template <class ResultTag, class TagA, class TagB, unsigned ScaleShift>
[[nodiscard]] Result<Quantity<ResultTag>> CheckedProduct(Quantity<TagA> a, Quantity<TagB> b) {
  static_assert(ScaleShift <= 18, "scale shift out of range");
  const std::int64_t divisor = Pow10(ScaleShift);
  ECG_TRY(scaled, CheckedMulDiv(a.raw(), b.raw(), divisor));
  if (!detail::InDomain<ResultTag>(scaled)) {
    return detail::DomainError<ResultTag>(scaled);
  }
  return Quantity<ResultTag>::FromRaw(scaled);
}

/// Applies an exact rational scale: value * numerator / denominator.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> CheckedScale(Quantity<Tag> value, std::int64_t numerator,
                                                 std::int64_t denominator) {
  if (denominator == 0) {
    return MakeError(ErrorCode::kDivisionByZero, std::string(QuantityTraits<Tag>::kName),
                     "scale denominator is zero");
  }
  ECG_TRY(scaled, CheckedMulDiv(value.raw(), numerator, denominator));
  if (!detail::InDomain<Tag>(scaled)) {
    return detail::DomainError<Tag>(scaled);
  }
  return Quantity<Tag>::FromRaw(scaled);
}

/// Renders a quantity as an exact decimal string in its declared unit, with
/// trailing zeros removed and no exponent notation. Locale independent.
template <class Tag>
[[nodiscard]] std::string FormatQuantity(Quantity<Tag> value) {
  constexpr int kScale = QuantityTraits<Tag>::kScaleDigits;
  const std::int64_t raw = value.raw();
  std::string out;
  const bool negative = raw < 0;
  // Magnitude as an unsigned value so the most negative raw value is handled.
  std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(raw) + 1u)
                                     : static_cast<std::uint64_t>(raw);
  if (negative) {
    out.push_back('-');
  }
  if constexpr (kScale == 0) {
    out.append(std::to_string(magnitude));
  } else {
    const std::uint64_t scale = static_cast<std::uint64_t>(Pow10(static_cast<unsigned>(kScale)));
    out.append(std::to_string(magnitude / scale));
    std::uint64_t fraction = magnitude % scale;
    if (fraction != 0) {
      std::string digits(static_cast<std::size_t>(kScale), '0');
      for (int i = kScale - 1; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = static_cast<char>('0' + (fraction % 10u));
        fraction /= 10u;
      }
      while (!digits.empty() && digits.back() == '0') {
        digits.pop_back();
      }
      out.push_back('.');
      out.append(digits);
    }
  }
  return out;
}

/// Parses an exact decimal string in the quantity's declared unit. The grammar
/// is deliberately narrow: an optional sign, one or more digits, an optional
/// fraction of at most the declared scale, and nothing else. Exponent notation,
/// whitespace, grouping separators, and locale digits are rejected.
template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> ParseQuantity(std::string_view text) {
  constexpr int kScale = QuantityTraits<Tag>::kScaleDigits;
  const std::string_view name(QuantityTraits<Tag>::kName);

  if (text.empty()) {
    return MakeError(ErrorCode::kMalformedInput, std::string(name), "empty numeric literal");
  }
  if (text.size() > 40) {
    return MakeError(ErrorCode::kMalformedInput, std::string(name),
                     "numeric literal longer than 40 characters");
  }

  std::size_t index = 0;
  bool negative = false;
  if (text[index] == '+' || text[index] == '-') {
    negative = text[index] == '-';
    ++index;
  }
  if (index >= text.size()) {
    return MakeError(ErrorCode::kMalformedInput, std::string(name), "sign without digits");
  }

  std::uint64_t magnitude = 0;
  std::size_t integer_digits = 0;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return MakeError(ErrorCode::kNumericOverflow, std::string(name), "numeric literal overflow");
    }
    magnitude = magnitude * 10u + digit;
    ++integer_digits;
    ++index;
  }
  if (integer_digits == 0) {
    return MakeError(ErrorCode::kMalformedInput, std::string(name),
                     "no digits before the decimal point");
  }

  int fraction_digits = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
      if (fraction_digits >= kScale) {
        return MakeError(ErrorCode::kOutOfRange, std::string(name),
                         "more than " + std::to_string(kScale) + " fractional digits");
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
      if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
        return MakeError(ErrorCode::kNumericOverflow, std::string(name), "numeric literal overflow");
      }
      magnitude = magnitude * 10u + digit;
      ++fraction_digits;
      ++index;
    }
    if (fraction_digits == 0) {
      return MakeError(ErrorCode::kMalformedInput, std::string(name),
                       "decimal point without fractional digits");
    }
  }
  if (index != text.size()) {
    return MakeError(ErrorCode::kMalformedInput, std::string(name),
                     "unexpected character in numeric literal");
  }
  magnitude *= static_cast<std::uint64_t>(Pow10(static_cast<unsigned>(kScale - fraction_digits)));

  const std::uint64_t kPositiveLimit = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  const std::uint64_t kNegativeLimit = kPositiveLimit + 1u;
  if (magnitude > (negative ? kNegativeLimit : kPositiveLimit)) {
    return MakeError(ErrorCode::kNumericOverflow, std::string(name),
                     "numeric literal does not fit in 64 bits");
  }
  const std::int64_t raw = negative ? static_cast<std::int64_t>(~magnitude + 1u)
                                    : static_cast<std::int64_t>(magnitude);
  if (!detail::InDomain<Tag>(raw)) {
    return detail::DomainError<Tag>(raw);
  }
  return Quantity<Tag>::FromRaw(raw);
}

// ---------------------------------------------------------------------------
// Quantity tags and domain declarations
// ---------------------------------------------------------------------------

struct PowerKwTag;
struct EnergyKwhTag;
struct PriceTag;
struct MoneyTag;
struct DurationTag;
struct RatioTag;
struct UtcInstantTag;
struct PowerRateTag;

/// Power in kilowatts (scale 10^-3 kW, i.e. watts).
using PowerKw = Quantity<PowerKwTag>;
/// Energy in kilowatt-hours (scale 10^-3 kWh, i.e. watt-hours).
using EnergyKwh = Quantity<EnergyKwhTag>;
/// Energy price in micros of currency per kilowatt-hour. Negative values are
/// representable and meaningful: curtailment can be paid for.
using PriceMicrosPerKwh = Quantity<PriceTag>;
/// Money in micros of the tariff's currency.
using MoneyMicros = Quantity<MoneyTag>;
/// A non-negative duration in whole seconds.
using DurationSec = Quantity<DurationTag>;
/// A dimensionless ratio in parts per million (1'000'000 == 1.0).
using RatioPpm = Quantity<RatioTag>;
/// Microseconds since the Unix epoch, UTC.
using UtcInstant = Quantity<UtcInstantTag>;
/// A rate of change of power in kilowatts per minute (scale 10^-3 kW/min).
using PowerKwPerMin = Quantity<PowerRateTag>;

template <>
struct QuantityTraits<PowerKwTag> {
  static constexpr int kScaleDigits = 3;
  static constexpr std::int64_t kMinRaw = -1000000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000000LL;
  static constexpr std::string_view kUnit = "kW";
  static constexpr std::string_view kName = "power_kw";
};

template <>
struct QuantityTraits<EnergyKwhTag> {
  static constexpr int kScaleDigits = 3;
  static constexpr std::int64_t kMinRaw = -1000000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000000LL;
  static constexpr std::string_view kUnit = "kWh";
  static constexpr std::string_view kName = "energy_kwh";
};

template <>
struct QuantityTraits<PriceTag> {
  static constexpr int kScaleDigits = 6;
  static constexpr std::int64_t kMinRaw = -1000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000LL;
  static constexpr std::string_view kUnit = "currency/kWh";
  static constexpr std::string_view kName = "price_micros_per_kwh";
};

template <>
struct QuantityTraits<MoneyTag> {
  static constexpr int kScaleDigits = 6;
  static constexpr std::int64_t kMinRaw = -1000000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000000LL;
  static constexpr std::string_view kUnit = "currency";
  static constexpr std::string_view kName = "money_micros";
};

template <>
struct QuantityTraits<DurationTag> {
  static constexpr int kScaleDigits = 0;
  static constexpr std::int64_t kMinRaw = 0;
  static constexpr std::int64_t kMaxRaw = 1000000000000LL;
  static constexpr std::string_view kUnit = "s";
  static constexpr std::string_view kName = "duration_s";
};

template <>
struct QuantityTraits<RatioTag> {
  static constexpr int kScaleDigits = 6;
  static constexpr std::int64_t kMinRaw = -1000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000LL;
  static constexpr std::string_view kUnit = "ratio";
  static constexpr std::string_view kName = "ratio_ppm";
};

template <>
struct QuantityTraits<UtcInstantTag> {
  static constexpr int kScaleDigits = 6;
  static constexpr std::int64_t kMinRaw = (-9223372036854775807LL - 1);
  static constexpr std::int64_t kMaxRaw = 9223372036854775807LL;
  static constexpr std::string_view kUnit = "us";
  static constexpr std::string_view kName = "utc_instant_us";
};

template <>
struct QuantityTraits<PowerRateTag> {
  static constexpr int kScaleDigits = 3;
  static constexpr std::int64_t kMinRaw = -1000000000000LL;
  static constexpr std::int64_t kMaxRaw = 1000000000000LL;
  static constexpr std::string_view kUnit = "kW/min";
  static constexpr std::string_view kName = "power_rate_kw_per_min";
};

/// Exactly 1.0 as a ratio.
inline constexpr RatioPpm kUnitRatio = RatioPpm::FromRaw(1000000);
/// Exactly 0.0 as a ratio.
inline constexpr RatioPpm kZeroRatio = RatioPpm::FromRaw(0);

}  // namespace ecg

#endif  // ECG_QUANTITY_HPP
