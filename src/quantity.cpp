#include "ecg/quantity.hpp"

namespace ecg {
namespace {

/// Unsigned 128-bit accumulator used only by CheckedMulDiv. Kept private: no
/// wider integer type is assumed to exist on any supported compiler.
struct UInt128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};
};

/// Adds a plain 64-bit value to the accumulator, propagating the carry.
void AddU64(UInt128& acc, std::uint64_t value) noexcept {
  const std::uint64_t lo = acc.lo + value;
  const std::uint64_t carry = lo < acc.lo ? 1u : 0u;
  acc.lo = lo;
  acc.hi += carry;
}

/// Adds value << shift, for shift in {0, 32, 64}.
void AddShifted(UInt128& acc, std::uint64_t value, unsigned shift) noexcept {
  if (value == 0) {
    return;
  }
  if (shift == 0) {
    AddU64(acc, value);
    return;
  }
  if (shift >= 64) {
    acc.hi += value << (shift - 64u);
    return;
  }
  // shift == 32: the low half lands in lo, the high half carries into hi.
  const std::uint64_t lowPart = value << shift;
  const std::uint64_t highPart = value >> (64u - shift);
  const std::uint64_t lo = acc.lo + lowPart;
  const std::uint64_t carry = lo < acc.lo ? 1u : 0u;
  acc.lo = lo;
  acc.hi += highPart + carry;
}

/// Full 128-bit product of two 64-bit values built from four 32-bit partial
/// products. Every accumulator step is carry-checked.
[[nodiscard]] UInt128 MulU64(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a0 = a & 0xFFFFFFFFu;
  const std::uint64_t a1 = a >> 32;
  const std::uint64_t b0 = b & 0xFFFFFFFFu;
  const std::uint64_t b1 = b >> 32;
  UInt128 acc;
  AddShifted(acc, a0 * b0, 0);
  AddShifted(acc, a0 * b1, 32);
  AddShifted(acc, a1 * b0, 32);
  AddShifted(acc, a1 * b1, 64);
  return acc;
}

[[nodiscard]] bool TestBit(const UInt128& value, unsigned index) noexcept {
  if (index >= 64) {
    return ((value.hi >> (index - 64u)) & 1u) != 0u;
  }
  return ((value.lo >> index) & 1u) != 0u;
}

void SetBit(UInt128& value, unsigned index) noexcept {
  if (index >= 64) {
    value.hi |= (std::uint64_t{1} << (index - 64u));
  } else {
    value.lo |= (std::uint64_t{1} << index);
  }
}

/// Restoring division of a 128-bit dividend by a non-zero 64-bit divisor.
/// Returns the quotient in 128 bits and the remainder, which always fits 64.
[[nodiscard]] UInt128 DivModU128ByU64(UInt128 dividend, std::uint64_t divisor,
                                      std::uint64_t* remainder) noexcept {
  UInt128 quotient;
  std::uint64_t rem = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t nextBit = TestBit(dividend, static_cast<unsigned>(bit)) ? 1u : 0u;
    const bool carry = (rem >> 63) != 0u;
    rem = (rem << 1) | nextBit;
    if (carry || rem >= divisor) {
      rem -= divisor;
      SetBit(quotient, static_cast<unsigned>(bit));
    }
  }
  *remainder = rem;
  return quotient;
}

}  // namespace

Result<std::int64_t> CheckedMulDiv(std::int64_t a, std::int64_t b, std::int64_t divisor) {
  if (divisor == 0) {
    return MakeError(ErrorCode::kDivisionByZero, "checked_mul_div", "divisor is zero");
  }

  const bool negative = (a < 0) != (b < 0);
  const bool divisorNegative = divisor < 0;
  const bool resultNegative = negative != divisorNegative;

  const auto magnitude = [](std::int64_t value) -> std::uint64_t {
    return value < 0 ? (~static_cast<std::uint64_t>(value) + 1u) : static_cast<std::uint64_t>(value);
  };

  const std::uint64_t ua = magnitude(a);
  const std::uint64_t ub = magnitude(b);
  const std::uint64_t ud = magnitude(divisor);

  const UInt128 product = MulU64(ua, ub);
  std::uint64_t remainder = 0;
  const UInt128 quotient = DivModU128ByU64(product, ud, &remainder);

  const std::uint64_t limit =
      resultNegative ? static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u
                     : static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  if (quotient.hi != 0 || quotient.lo > limit) {
    return MakeError(ErrorCode::kNumericOverflow, "checked_mul_div",
                     "(" + std::to_string(a) + " * " + std::to_string(b) + ") / " +
                         std::to_string(divisor) + " does not fit in 64 bits");
  }

  const std::uint64_t magnitudeResult = quotient.lo;
  const std::int64_t value = resultNegative ? static_cast<std::int64_t>(~magnitudeResult + 1u)
                                            : static_cast<std::int64_t>(magnitudeResult);
  return value;
}

}  // namespace ecg
