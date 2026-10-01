#include "ecg/tariff.hpp"

#include <algorithm>

namespace ecg {

const char* DemandBasisName(DemandBasis basis) noexcept {
  switch (basis) {
    case DemandBasis::kPeakIntervalAverage: return "peak_interval_average";
    case DemandBasis::kMaxInterval: return "max_interval";
  }
  return "unknown";
}

const char* PriceCoverageName(PriceCoverage coverage) noexcept {
  switch (coverage) {
    case PriceCoverage::kCovered: return "covered";
    case PriceCoverage::kGap: return "gap";
    case PriceCoverage::kAmbiguous: return "ambiguous";
  }
  return "unknown";
}

bool operator==(const DemandChargeRule& a, const DemandChargeRule& b) noexcept {
  return a.id == b.id && a.recurrence.weekday_mask == b.recurrence.weekday_mask &&
         a.recurrence.start_second_of_day == b.recurrence.start_second_of_day &&
         a.recurrence.duration_seconds == b.recurrence.duration_seconds &&
         a.averaging_interval == b.averaging_interval && a.threshold_kw == b.threshold_kw &&
         a.charge_per_kw == b.charge_per_kw && a.basis == b.basis && a.validity == b.validity;
}

bool operator==(const Tariff& a, const Tariff& b) noexcept {
  return a.id == b.id && a.revision == b.revision && a.epoch == b.epoch && a.currency == b.currency &&
         a.prices == b.prices && a.demand_charges == b.demand_charges &&
         a.published_at == b.published_at && a.validity == b.validity;
}

PriceLookup PriceAt(const Tariff& tariff, UtcInstant instant) noexcept {
  PriceLookup lookup;
  std::size_t matches = 0;
  for (const PriceInterval& interval : tariff.prices) {
    if (interval.window.contains(instant)) {
      ++matches;
      if (matches == 1) {
        lookup.price = interval.price;
      }
    }
  }
  if (matches == 0) {
    lookup.coverage = PriceCoverage::kGap;
    return lookup;
  }
  if (matches > 1) {
    lookup.coverage = PriceCoverage::kAmbiguous;
    return lookup;
  }
  lookup.coverage = PriceCoverage::kCovered;
  return lookup;
}

Result<std::vector<PriceSlice>> PriceSlicesForWindow(const Tariff& tariff, const Interval& window) {
  // Ambiguity must be detected across the whole window before any price is
  // reported. Walking alone cannot see a contradiction that begins inside a
  // slice the walk is about to swallow whole.
  {
    std::vector<Interval> clipped;
    clipped.reserve(tariff.prices.size());
    for (const PriceInterval& interval : tariff.prices) {
      const auto intersection = interval.window.Intersect(window);
      if (intersection.ok() && intersection.value().has_value()) {
        clipped.push_back(*intersection.value());
      }
    }
    if (const auto overlap = FindFirstOverlap(clipped); overlap.has_value()) {
      return MakeError(ErrorCode::kStateConflict, "tariff:" + tariff.id.ToString(),
                       "price schedule overlaps inside the requested window at " +
                           FormatRfc3339(overlap->first.begin()) + " and " +
                           FormatRfc3339(overlap->second.begin()));
    }
  }

  std::vector<PriceSlice> slices;
  UtcInstant cursor = window.begin();
  // Walk the schedule in time order, taking the earliest interval that covers
  // the cursor. Adjacent intervals are merged by the caller's schedule; here we
  // only need to find, for each step, the earliest covering interval.
  std::vector<const PriceInterval*> ordered;
  ordered.reserve(tariff.prices.size());
  for (const PriceInterval& interval : tariff.prices) {
    ordered.push_back(&interval);
  }
  std::sort(ordered.begin(), ordered.end(), [](const PriceInterval* a, const PriceInterval* b) {
    if (a->window.begin() != b->window.begin()) {
      return a->window.begin() < b->window.begin();
    }
    return a->window.end() < b->window.end();
  });

  std::size_t overlap_guard = 0;
  while (cursor < window.end()) {
    if (++overlap_guard > ordered.size() + 1) {
      return MakeError(ErrorCode::kInternalInvariantViolation, "tariff",
                       "price slice walk did not terminate");
    }
    const PriceInterval* covering = nullptr;
    std::size_t covering_count = 0;
    for (const PriceInterval* candidate : ordered) {
      if (candidate->window.contains(cursor)) {
        if (covering == nullptr) {
          covering = candidate;
        }
        ++covering_count;
      }
    }
    if (covering == nullptr) {
      return MakeError(ErrorCode::kNotFound, "tariff:" + tariff.id.ToString(),
                       "price schedule does not cover " + FormatRfc3339(cursor));
    }
    if (covering_count > 1) {
      return MakeError(ErrorCode::kStateConflict, "tariff:" + tariff.id.ToString(),
                       "price schedule is ambiguous at " + FormatRfc3339(cursor));
    }
    const UtcInstant slice_end = covering->window.end() < window.end() ? covering->window.end()
                                                                      : window.end();
    const auto slice = Interval::Make(cursor, slice_end);
    if (!slice.ok()) {
      return slice.error();
    }
    slices.push_back(PriceSlice{slice.value(), covering->price});
    cursor = slice_end;
  }
  return slices;
}

std::vector<PriceSlice> CoveredPriceSlices(const Tariff& tariff, const Interval& range) {
  std::vector<PriceSlice> slices;
  for (const PriceInterval& interval : tariff.prices) {
    const auto clipped = interval.window.Intersect(range);
    if (!clipped.ok() || !clipped.value().has_value()) {
      continue;
    }
    slices.push_back(PriceSlice{*clipped.value(), interval.price});
  }
  std::sort(slices.begin(), slices.end(),
            [](const PriceSlice& a, const PriceSlice& b) { return a.window < b.window; });
  return slices;
}

Status ValidateTariff(const Tariff& tariff) {
  const Status structure = ValidateTariffStructure(tariff);
  if (!structure.ok()) {
    return structure.error();
  }
  std::vector<Interval> windows;
  windows.reserve(tariff.prices.size());
  for (const PriceInterval& interval : tariff.prices) {
    windows.push_back(interval.window);
  }
  if (const auto overlap = FindFirstOverlap(windows); overlap.has_value()) {
    return MakeError(ErrorCode::kStateConflict, "tariff:" + tariff.id.ToString(),
                     "price intervals overlap: " + FormatRfc3339(overlap->first.begin()) + " and " +
                         FormatRfc3339(overlap->second.begin()));
  }
  return OkStatus();
}

Status ValidateTariffStructure(const Tariff& tariff) {
  if (tariff.id.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, "tariff", "tariff id is empty");
  }
  if (tariff.currency.size() != 3) {
    return MakeError(ErrorCode::kMalformedInput, "tariff:" + tariff.id.ToString(),
                     "currency code must be three letters");
  }
  for (const char c : tariff.currency) {
    if (c < 'A' || c > 'Z') {
      return MakeError(ErrorCode::kMalformedInput, "tariff:" + tariff.id.ToString(),
                       "currency code must be three upper-case letters");
    }
  }
  if (tariff.prices.size() > kMaxPriceIntervals) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "tariff:" + tariff.id.ToString(),
                     "more than " + std::to_string(kMaxPriceIntervals) + " price intervals");
  }
  if (tariff.demand_charges.size() > kMaxDemandChargeRules) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "tariff:" + tariff.id.ToString(),
                     "more than " + std::to_string(kMaxDemandChargeRules) + " demand-charge rules");
  }
  for (const DemandChargeRule& rule : tariff.demand_charges) {
    if (rule.id.empty()) {
      return MakeError(ErrorCode::kMissingRequiredField, "tariff:" + tariff.id.ToString(),
                       "demand-charge rule id is empty");
    }
    if (rule.averaging_interval.raw() <= 0) {
      return MakeError(ErrorCode::kInvalidArgument, "tariff:" + tariff.id.ToString(),
                       "demand-charge rule " + rule.id.ToString() + " has a non-positive averaging interval");
    }
    if (rule.recurrence.duration_seconds <= 0) {
      return MakeError(ErrorCode::kInvalidArgument, "tariff:" + tariff.id.ToString(),
                       "demand-charge rule " + rule.id.ToString() + " has a non-positive duration");
    }
    if ((rule.recurrence.weekday_mask & 0x80u) != 0u) {
      return MakeError(ErrorCode::kInvalidArgument, "tariff:" + tariff.id.ToString(),
                       "demand-charge rule " + rule.id.ToString() + " sets a reserved weekday bit");
    }
    if (rule.charge_per_kw.is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "tariff:" + tariff.id.ToString(),
                       "demand-charge rule " + rule.id.ToString() + " has a negative charge");
    }
  }
  return OkStatus();
}

}  // namespace ecg
