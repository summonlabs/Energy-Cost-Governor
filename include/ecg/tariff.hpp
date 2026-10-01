#ifndef ECG_TARIFF_HPP
#define ECG_TARIFF_HPP

// Tariffs, price schedules, and demand-charge rules.
//
// This runtime does not own tariffs. They are published by a tariff authority
// and consumed here as evidence. What this header owns is the *interpretation*
// of a tariff: which price applies to an instant, whether the schedule actually
// covers a window, and how a demand-charge rule's wall-clock recurrence maps
// onto UTC. Interpretation errors are reported as coverage states
// (covered / gap / ambiguous) rather than resolved by guessing, because an
// invented price would silently authorise real spend.

#include <cstdint>
#include <string>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/result.hpp"
#include "ecg/strong.hpp"
#include "ecg/time.hpp"

namespace ecg {

/// One contiguous stretch of time with a single energy price. Prices may be
/// negative: a grid that pays for consumption is a real operating condition.
struct PriceInterval {
  Interval window;
  PriceMicrosPerKwh price;

  friend bool operator==(const PriceInterval& a, const PriceInterval& b) noexcept {
    return a.window == b.window && a.price == b.price;
  }
};

/// How a demand-charge rule measures the billed peak.
enum class DemandBasis : std::uint8_t {
  /// Peak of interval averages, the common utility formulation.
  kPeakIntervalAverage = 0,
  /// Highest instantaneous interval maximum.
  kMaxInterval = 1,
};

[[nodiscard]] const char* DemandBasisName(DemandBasis basis) noexcept;

/// A demand-charge rule: during a wall-clock recurrence, a metered peak above a
/// threshold accrues a charge per kilowatt, billed at the end of the period.
struct DemandChargeRule {
  DemandWindowId id;
  /// Recurrence in the tariff's zone, for example weekdays 14:00-19:00.
  RecurringWindow recurrence;
  /// Metering interval the peak is computed over.
  DurationSec averaging_interval;
  /// Peak above this level is billed.
  PowerKw threshold_kw;
  /// Charge in micros of the tariff currency per kilowatt of billed peak.
  MoneyMicros charge_per_kw;
  DemandBasis basis{DemandBasis::kPeakIntervalAverage};
  /// The rule is in force only inside this validity range.
  Interval validity;

  friend bool operator==(const DemandChargeRule& a, const DemandChargeRule& b) noexcept;
};

/// A published tariff revision, as consumed from the tariff authority.
struct Tariff {
  TariffId id;
  TariffRevision revision;
  /// Authority epoch in which this revision was published.
  Epoch epoch;
  /// ISO 4217 alphabetic currency code, for example "USD".
  std::string currency;
  /// Price schedule. Must be pairwise disjoint; gaps are reported as coverage
  /// gaps rather than filled.
  std::vector<PriceInterval> prices;
  std::vector<DemandChargeRule> demand_charges;
  UtcInstant published_at;
  Interval validity;

  friend bool operator==(const Tariff& a, const Tariff& b) noexcept;
};

/// Result of asking "what does energy cost at instant t?".
enum class PriceCoverage : std::uint8_t {
  /// Exactly one price interval contains the instant.
  kCovered = 0,
  /// No price interval contains the instant. The schedule does not say.
  kGap = 1,
  /// More than one price interval contains the instant. The schedule is
  /// self-contradictory and no price may be chosen.
  kAmbiguous = 2,
};

[[nodiscard]] const char* PriceCoverageName(PriceCoverage coverage) noexcept;

struct PriceLookup {
  PriceCoverage coverage{PriceCoverage::kGap};
  PriceMicrosPerKwh price;
};

/// Price in effect at an instant. Never throws; ambiguity and gaps are values.
[[nodiscard]] PriceLookup PriceAt(const Tariff& tariff, UtcInstant instant) noexcept;

/// Splits a window into the tariff price slices that cover it. Slices are
/// clipped to the window and returned in ascending time order.
struct PriceSlice {
  Interval window;
  PriceMicrosPerKwh price;
};

/// Fails with kNotFound when any part of the window is uncovered, and with
/// kStateConflict when the schedule overlaps inside the window. Partial
/// coverage is therefore never silently priced.
[[nodiscard]] Result<std::vector<PriceSlice>> PriceSlicesForWindow(const Tariff& tariff,
                                                                   const Interval& window);

/// Price slices covering as much of a range as the schedule covers, clipped to
/// the range. Unlike PriceSlicesForWindow this tolerates gaps, which is what a
/// search for a cheaper target window needs: an uncovered stretch simply cannot
/// be a target.
[[nodiscard]] std::vector<PriceSlice> CoveredPriceSlices(const Tariff& tariff, const Interval& range);

/// Structural validation of a tariff as consumed from another authority: the
/// currency code, field ranges, and demand-rule sanity. This deliberately does
/// not judge whether the price schedule contradicts itself, because a schedule
/// overlap is a property of a specific window rather than of the whole tariff,
/// and the decision trace is where it must surface.
[[nodiscard]] Status ValidateTariffStructure(const Tariff& tariff);

/// Full validation: structure plus schedule disjointness. Used when a tariff is
/// being admitted as usable evidence.
[[nodiscard]] Status ValidateTariff(const Tariff& tariff);

/// Upper bounds on consumed tariff structure, so a malformed or hostile
/// publication cannot exhaust memory.
inline constexpr std::size_t kMaxPriceIntervals = 100000;
inline constexpr std::size_t kMaxDemandChargeRules = 4096;

}  // namespace ecg

#endif  // ECG_TARIFF_HPP
