// Tests for tariff interpretation: which price applies to an instant, how a
// window is split into price slices, whether coverage gaps and ambiguous
// schedules are reported instead of guessed, and structural validation.

#include "test.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/tariff.hpp"
#include "ecg/time.hpp"

namespace {

constexpr std::int64_t kMicrosPerSecond = 1000000;

/// Parses an instant, recording a failure when the timestamp itself is broken.
[[nodiscard]] ecg::UtcInstant At(std::string_view text) {
  const auto parsed = ecg::ParseRfc3339(text);
  ECG_CHECK_MSG(parsed.ok(), std::string("could not parse instant ") + std::string(text));
  if (!parsed.ok()) {
    return ecg::UtcInstant::FromRaw(0);
  }
  return parsed.value();
}

[[nodiscard]] ecg::Interval Window(std::string_view begin, std::string_view end) {
  const auto made = ecg::Interval::Make(At(begin), At(end));
  ECG_CHECK_MSG(made.ok(), std::string("interval ") + std::string(begin) + " .. " +
                               std::string(end) + " must be constructible");
  if (!made.ok()) {
    return ecg::Interval();
  }
  return made.value();
}

/// A window built straight from raw microseconds, for bulk schedule fixtures.
[[nodiscard]] ecg::Interval RawWindow(std::int64_t begin_micros, std::int64_t end_micros) {
  const auto made =
      ecg::Interval::Make(ecg::UtcInstant::FromRaw(begin_micros), ecg::UtcInstant::FromRaw(end_micros));
  if (!made.ok()) {
    return ecg::Interval();
  }
  return made.value();
}

[[nodiscard]] ecg::PriceMicrosPerKwh Price(std::int64_t raw) {
  return ecg::PriceMicrosPerKwh::FromRaw(raw);
}

[[nodiscard]] ecg::PriceInterval Priced(std::string_view begin, std::string_view end,
                                        std::int64_t raw_price) {
  return ecg::PriceInterval{Window(begin, end), Price(raw_price)};
}

void CheckSlice(const ecg::PriceSlice& slice, std::string_view begin, std::string_view end,
                std::int64_t raw_price, const char* const what) {
  const bool bounds_ok = slice.window.begin() == At(begin) && slice.window.end() == At(end);
  ECG_CHECK_MSG(bounds_ok, std::string(what) + ": got [" + ecg::FormatRfc3339(slice.window.begin()) +
                               ", " + ecg::FormatRfc3339(slice.window.end()) + ")");
  ECG_CHECK_MSG(slice.price.raw() == raw_price,
                std::string(what) + ": price " + ecg::FormatQuantity(slice.price) + " expected " +
                    ecg::FormatQuantity(Price(raw_price)));
}

[[nodiscard]] ecg::DemandChargeRule DemandRule(std::string_view id) {
  ecg::DemandChargeRule rule;
  rule.id = ecg::DemandWindowId::FromTrusted(std::string(id));
  // Weekdays 14:00-19:00 local, measured over a fifteen-minute average.
  rule.recurrence.weekday_mask = 0x1Fu;
  rule.recurrence.start_second_of_day = 14 * 3600;
  rule.recurrence.duration_seconds = 5 * 3600;
  rule.averaging_interval = ecg::DurationSec::FromRaw(900);
  rule.threshold_kw = ecg::PowerKw::FromRaw(50000);
  rule.charge_per_kw = ecg::MoneyMicros::FromRaw(10000000);
  rule.basis = ecg::DemandBasis::kPeakIntervalAverage;
  rule.validity = Window("2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z");
  return rule;
}

/// A tariff that is structurally valid unless a test changes exactly one field.
[[nodiscard]] ecg::Tariff ValidTariff() {
  ecg::Tariff tariff;
  tariff.id = ecg::TariffId::FromTrusted("tariff/example");
  tariff.revision = ecg::TariffRevision::FromRaw(7);
  tariff.epoch = ecg::Epoch::FromRaw(3);
  tariff.currency = "USD";
  tariff.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
  };
  tariff.demand_charges = {DemandRule("dw/afternoon")};
  tariff.published_at = At("2025-12-15T00:00:00Z");
  tariff.validity = Window("2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z");
  return tariff;
}
}  // namespace

// ---------------------------------------------------------------------------
// PriceAt
// ---------------------------------------------------------------------------

ECG_TEST("tariff.price_at_reports_covered_gap_and_ambiguous_coverage") {
  ecg::Tariff tariff = ValidTariff();
  ECG_CHECK_EQ(ecg::PriceCoverageName(ecg::PriceCoverage::kCovered), std::string("covered"));
  ECG_CHECK_EQ(ecg::PriceCoverageName(ecg::PriceCoverage::kGap), std::string("gap"));
  ECG_CHECK_EQ(ecg::PriceCoverageName(ecg::PriceCoverage::kAmbiguous), std::string("ambiguous"));

  const ecg::PriceLookup first = ecg::PriceAt(tariff, At("2026-01-01T00:00:00Z"));
  ECG_CHECK(first.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(first.price.raw(), std::int64_t{100000});

  const ecg::PriceLookup last_of_first =
      ecg::PriceAt(tariff, At("2026-01-01T05:59:59.999999Z"));
  ECG_CHECK(last_of_first.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(last_of_first.price.raw(), std::int64_t{100000});

  // The shared boundary belongs to the second interval: intervals are half-open.
  const ecg::PriceLookup boundary = ecg::PriceAt(tariff, At("2026-01-01T06:00:00Z"));
  ECG_CHECK(boundary.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(boundary.price.raw(), std::int64_t{200000});

  const ecg::PriceLookup after_schedule = ecg::PriceAt(tariff, At("2026-01-01T12:00:00Z"));
  ECG_CHECK(after_schedule.coverage == ecg::PriceCoverage::kGap);
  const ecg::PriceLookup before_schedule = ecg::PriceAt(tariff, At("2025-12-31T23:59:59Z"));
  ECG_CHECK(before_schedule.coverage == ecg::PriceCoverage::kGap);
  const ecg::PriceLookup empty_schedule =
      ecg::PriceAt(ecg::Tariff{}, At("2026-01-01T00:00:00Z"));
  ECG_CHECK(empty_schedule.coverage == ecg::PriceCoverage::kGap);

  // A second interval that overlaps the first makes the instant ambiguous, but
  // only inside the overlap.
  tariff.prices.push_back(Priced("2026-01-01T05:00:00Z", "2026-01-01T07:00:00Z", 300000));
  const ecg::PriceLookup ambiguous = ecg::PriceAt(tariff, At("2026-01-01T06:00:00Z"));
  ECG_CHECK(ambiguous.coverage == ecg::PriceCoverage::kAmbiguous);
  const ecg::PriceLookup still_covered = ecg::PriceAt(tariff, At("2026-01-01T04:00:00Z"));
  ECG_CHECK(still_covered.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(still_covered.price.raw(), std::int64_t{100000});
  const ecg::PriceLookup ambiguity_boundary = ecg::PriceAt(tariff, At("2026-01-01T05:00:00Z"));
  ECG_CHECK(ambiguity_boundary.coverage == ecg::PriceCoverage::kAmbiguous);
}

ECG_TEST("tariff.price_at_preserves_negative_prices_exactly") {
  ecg::Tariff tariff = ValidTariff();
  tariff.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z", -1500000),
      Priced("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z", -1),
  };

  const ecg::PriceLookup curtailed = ecg::PriceAt(tariff, At("2026-01-01T00:30:00Z"));
  ECG_CHECK(curtailed.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(curtailed.price.raw(), std::int64_t{-1500000});
  ECG_CHECK_EQ(ecg::FormatQuantity(curtailed.price), std::string("-1.5"));
  ECG_CHECK(curtailed.price.is_negative());

  const ecg::PriceLookup micro = ecg::PriceAt(tariff, At("2026-01-01T01:30:00Z"));
  ECG_CHECK(micro.coverage == ecg::PriceCoverage::kCovered);
  ECG_CHECK_EQ(micro.price.raw(), std::int64_t{-1});
  ECG_CHECK_EQ(ecg::FormatQuantity(micro.price), std::string("-0.000001"));

  // Negative prices survive the slice walk unchanged as well.
  const auto slices = ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T00:00:00Z",
                                                               "2026-01-01T02:00:00Z"));
  ECG_CHECK_OK(slices);
  if (slices.ok()) {
    ECG_CHECK_EQ(slices.value().size(), static_cast<std::size_t>(2));
    if (slices.value().size() == 2u) {
      CheckSlice(slices.value()[0], "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z", -1500000,
                 "negative slice");
      CheckSlice(slices.value()[1], "2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z", -1,
                 "negative micro slice");
    }
  }
}

// ---------------------------------------------------------------------------
// PriceSlicesForWindow and CoveredPriceSlices
// ---------------------------------------------------------------------------

ECG_TEST("tariff.price_slices_split_a_multi_price_window") {
  ecg::Tariff tariff = ValidTariff();
  tariff.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
      Priced("2026-01-01T12:00:00Z", "2026-01-01T18:00:00Z", 300000),
  };

  const auto slices = ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T03:00:00Z",
                                                               "2026-01-01T13:00:00Z"));
  ECG_CHECK_OK(slices);
  if (!slices.ok()) {
    return;
  }
  ECG_CHECK_EQ(slices.value().size(), static_cast<std::size_t>(3));
  if (slices.value().size() == 3u) {
    CheckSlice(slices.value()[0], "2026-01-01T03:00:00Z", "2026-01-01T06:00:00Z", 100000,
               "first slice");
    CheckSlice(slices.value()[1], "2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000,
               "middle slice");
    CheckSlice(slices.value()[2], "2026-01-01T12:00:00Z", "2026-01-01T13:00:00Z", 300000,
               "last slice");
    // Ascending time order and no gaps between consecutive slices.
    ECG_CHECK(slices.value()[0].window.end() == slices.value()[1].window.begin());
    ECG_CHECK(slices.value()[1].window.end() == slices.value()[2].window.begin());
  }

  // A window inside a single interval yields exactly that interval's price.
  const auto single = ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T01:00:00Z",
                                                               "2026-01-01T02:00:00Z"));
  ECG_CHECK_OK(single);
  if (single.ok()) {
    ECG_CHECK_EQ(single.value().size(), static_cast<std::size_t>(1));
    if (single.value().size() == 1u) {
      CheckSlice(single.value()[0], "2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z", 100000,
                 "window inside one interval");
    }
  }

  // A window that starts and ends on interval boundaries is not split further.
  const auto exact = ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T06:00:00Z",
                                                              "2026-01-01T12:00:00Z"));
  ECG_CHECK_OK(exact);
  if (exact.ok()) {
    ECG_CHECK_EQ(exact.value().size(), static_cast<std::size_t>(1));
    if (exact.value().size() == 1u) {
      CheckSlice(exact.value()[0], "2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000,
                 "window equal to one interval");
    }
  }

  // The schedule may be supplied out of order and still be walked in time order.
  ecg::Tariff shuffled = ValidTariff();
  shuffled.prices = {
      Priced("2026-01-01T12:00:00Z", "2026-01-01T18:00:00Z", 300000),
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
  };
  const auto shuffled_slices = ecg::PriceSlicesForWindow(
      shuffled, Window("2026-01-01T03:00:00Z", "2026-01-01T13:00:00Z"));
  ECG_CHECK_OK(shuffled_slices);
  if (shuffled_slices.ok() && slices.ok()) {
    ECG_CHECK_EQ(shuffled_slices.value().size(), slices.value().size());
    if (shuffled_slices.value().size() == slices.value().size()) {
      for (std::size_t i = 0; i < slices.value().size(); ++i) {
        ECG_CHECK(shuffled_slices.value()[i].window == slices.value()[i].window);
        ECG_CHECK(shuffled_slices.value()[i].price == slices.value()[i].price);
      }
    }
  }
}

ECG_TEST("tariff.price_slices_never_silently_price_a_partial_window") {
  ecg::Tariff tariff = ValidTariff();
  tariff.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
  };

  // The window ends after the schedule does.
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T03:00:00Z",
                                                         "2026-01-01T07:00:00Z")),
                ecg::ErrorCode::kNotFound);
  // The window starts before the schedule does.
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(tariff, Window("2025-12-31T23:00:00Z",
                                                         "2026-01-01T01:00:00Z")),
                ecg::ErrorCode::kNotFound);
  // The window lies entirely inside a gap.
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T08:00:00Z",
                                                         "2026-01-01T09:00:00Z")),
                ecg::ErrorCode::kNotFound);
  // An empty schedule covers nothing.
  ecg::Tariff bare = ValidTariff();
  bare.prices.clear();
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(bare, Window("2026-01-01T00:00:00Z",
                                                       "2026-01-01T01:00:00Z")),
                ecg::ErrorCode::kNotFound);

  // Covered exactly up to the last instant of the schedule is fine.
  const auto exact = ecg::PriceSlicesForWindow(tariff, Window("2026-01-01T05:00:00Z",
                                                              "2026-01-01T06:00:00Z"));
  ECG_CHECK_OK(exact);
  if (exact.ok()) {
    ECG_CHECK_EQ(exact.value().size(), static_cast<std::size_t>(1));
  }
}

ECG_TEST("tariff.price_slices_report_an_ambiguous_schedule") {
  // Two intervals cover the first instant of the window: the schedule is
  // self-contradictory there and no price may be chosen.
  ecg::Tariff at_the_start = ValidTariff();
  at_the_start.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T20:00:00Z", 100000),
      Priced("2026-01-01T00:00:00Z", "2026-01-01T12:00:00Z", 200000),
  };
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(at_the_start, Window("2026-01-01T05:00:00Z",
                                                               "2026-01-01T15:00:00Z")),
                ecg::ErrorCode::kStateConflict);

  // A schedule that does not contradict itself is still walked normally.
  ecg::Tariff clean = ValidTariff();
  clean.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
  };
  const auto unambiguous = ecg::PriceSlicesForWindow(clean, Window("2026-01-01T05:00:00Z",
                                                                   "2026-01-01T07:00:00Z"));
  ECG_CHECK_OK(unambiguous);
  if (unambiguous.ok()) {
    ECG_CHECK_EQ(unambiguous.value().size(), static_cast<std::size_t>(2));
  }

  // The contradiction may also sit strictly inside the window, under the
  // interval that starts first. The schedule overlaps inside the window there,
  // so the walk must not return one confident price for the whole stretch.
  ecg::Tariff swallowed = ValidTariff();
  swallowed.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T20:00:00Z", 100000),
      Priced("2026-01-01T10:00:00Z", "2026-01-01T23:00:00Z", 200000),
  };
  ECG_CHECK_ERR(ecg::PriceSlicesForWindow(swallowed, Window("2026-01-01T05:00:00Z",
                                                            "2026-01-01T15:00:00Z")),
                ecg::ErrorCode::kStateConflict);
}

ECG_TEST("tariff.covered_price_slices_tolerate_gaps_and_stay_sorted") {
  ecg::Tariff tariff = ValidTariff();
  tariff.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z", 100000),
      Priced("2026-01-01T05:00:00Z", "2026-01-01T06:00:00Z", 200000),
      Priced("2026-01-01T03:00:00Z", "2026-01-01T04:00:00Z", 300000),
  };

  const std::vector<ecg::PriceSlice> slices =
      ecg::CoveredPriceSlices(tariff, Window("2026-01-01T00:30:00Z", "2026-01-01T05:30:00Z"));
  ECG_CHECK_EQ(slices.size(), static_cast<std::size_t>(3));
  if (slices.size() == 3u) {
    CheckSlice(slices[0], "2026-01-01T00:30:00Z", "2026-01-01T01:00:00Z", 100000, "covered head");
    CheckSlice(slices[1], "2026-01-01T03:00:00Z", "2026-01-01T04:00:00Z", 300000, "middle island");
    CheckSlice(slices[2], "2026-01-01T05:00:00Z", "2026-01-01T05:30:00Z", 200000, "covered tail");
    ECG_CHECK(slices[0].window.end() <= slices[1].window.begin());
    ECG_CHECK(slices[1].window.end() <= slices[2].window.begin());
  }

  // A range that touches nothing covered yields nothing at all, not an error.
  ECG_CHECK(ecg::CoveredPriceSlices(tariff, Window("2026-01-01T01:00:00Z",
                                                   "2026-01-01T03:00:00Z"))
                .empty());
  ECG_CHECK(ecg::CoveredPriceSlices(tariff, Window("2027-01-01T00:00:00Z",
                                                   "2027-01-02T00:00:00Z"))
                .empty());

  // A range that touches the very edge of an interval yields a sliver.
  const std::vector<ecg::PriceSlice> sliver =
      ecg::CoveredPriceSlices(tariff, Window("2026-01-01T00:59:59Z", "2026-01-01T01:00:00Z"));
  ECG_CHECK_EQ(sliver.size(), static_cast<std::size_t>(1));
  if (sliver.size() == 1u) {
    CheckSlice(sliver[0], "2026-01-01T00:59:59Z", "2026-01-01T01:00:00Z", 100000, "sliver");
  }
}

// ---------------------------------------------------------------------------
// ValidateTariff
// ---------------------------------------------------------------------------

ECG_TEST("tariff.validate_accepts_a_well_formed_tariff") {
  const ecg::Tariff tariff = ValidTariff();
  ECG_CHECK_OK(ecg::ValidateTariff(tariff));
  ECG_CHECK_EQ(ecg::DemandBasisName(ecg::DemandBasis::kPeakIntervalAverage),
               std::string("peak_interval_average"));
  ECG_CHECK_EQ(ecg::DemandBasisName(ecg::DemandBasis::kMaxInterval), std::string("max_interval"));

  // A tariff with no demand charges is fine, and touching price intervals are
  // disjoint, not overlapping.
  ecg::Tariff simple = ValidTariff();
  simple.demand_charges.clear();
  ECG_CHECK_OK(ecg::ValidateTariff(simple));

  // The boundary sizes are accepted.
  ecg::Tariff at_limit = ValidTariff();
  at_limit.prices.clear();
  at_limit.prices.reserve(ecg::kMaxPriceIntervals);
  const std::int64_t step = 60 * kMicrosPerSecond;
  for (std::size_t i = 0; i < ecg::kMaxPriceIntervals; ++i) {
    const std::int64_t begin = static_cast<std::int64_t>(i) * step;
    at_limit.prices.push_back(ecg::PriceInterval{RawWindow(begin, begin + step), Price(100000)});
  }
  ECG_CHECK_OK(ecg::ValidateTariff(at_limit));
}

ECG_TEST("tariff.validate_rejects_an_empty_id") {
  ecg::Tariff tariff = ValidTariff();
  tariff.id = ecg::TariffId();
  ECG_CHECK_ERR(ecg::ValidateTariff(tariff), ecg::ErrorCode::kMissingRequiredField);

  // A demand-charge rule with no id is rejected as well.
  ecg::Tariff with_bad_rule = ValidTariff();
  with_bad_rule.demand_charges.push_back(ecg::DemandChargeRule{});
  ECG_CHECK_ERR(ecg::ValidateTariff(with_bad_rule), ecg::ErrorCode::kMissingRequiredField);
}

ECG_TEST("tariff.validate_rejects_a_malformed_currency_code") {
  ecg::Tariff lower = ValidTariff();
  lower.currency = "usd";
  ECG_CHECK_ERR(ecg::ValidateTariff(lower), ecg::ErrorCode::kMalformedInput);

  ecg::Tariff mixed = ValidTariff();
  mixed.currency = "Usd";
  ECG_CHECK_ERR(ecg::ValidateTariff(mixed), ecg::ErrorCode::kMalformedInput);

  ecg::Tariff short_code = ValidTariff();
  short_code.currency = "US";
  ECG_CHECK_ERR(ecg::ValidateTariff(short_code), ecg::ErrorCode::kMalformedInput);

  ecg::Tariff long_code = ValidTariff();
  long_code.currency = "USDX";
  ECG_CHECK_ERR(ecg::ValidateTariff(long_code), ecg::ErrorCode::kMalformedInput);

  ecg::Tariff empty_code = ValidTariff();
  empty_code.currency.clear();
  ECG_CHECK_ERR(ecg::ValidateTariff(empty_code), ecg::ErrorCode::kMalformedInput);

  ecg::Tariff digits = ValidTariff();
  digits.currency = "US1";
  ECG_CHECK_ERR(ecg::ValidateTariff(digits), ecg::ErrorCode::kMalformedInput);

  // Currency codes of the right shape are accepted.
  ecg::Tariff euro = ValidTariff();
  euro.currency = "EUR";
  ECG_CHECK_OK(ecg::ValidateTariff(euro));
}

ECG_TEST("tariff.validate_rejects_a_negative_demand_charge") {
  ecg::Tariff tariff = ValidTariff();
  tariff.demand_charges[0].charge_per_kw = ecg::MoneyMicros::FromRaw(-1);
  ECG_CHECK_ERR(ecg::ValidateTariff(tariff), ecg::ErrorCode::kInvalidArgument);

  // Zero is a legal (if pointless) charge; the rule rejects only negatives.
  ecg::Tariff free = ValidTariff();
  free.demand_charges[0].charge_per_kw = ecg::MoneyMicros::FromRaw(0);
  ECG_CHECK_OK(ecg::ValidateTariff(free));
}

ECG_TEST("tariff.validate_rejects_a_non_positive_averaging_interval") {
  ecg::Tariff zero = ValidTariff();
  zero.demand_charges[0].averaging_interval = ecg::DurationSec::FromRaw(0);
  ECG_CHECK_ERR(ecg::ValidateTariff(zero), ecg::ErrorCode::kInvalidArgument);

  // One second is the smallest interval the type allows, and it is accepted.
  ecg::Tariff smallest = ValidTariff();
  smallest.demand_charges[0].averaging_interval = ecg::DurationSec::FromRaw(1);
  ECG_CHECK_OK(ecg::ValidateTariff(smallest));
}

ECG_TEST("tariff.validate_rejects_a_reserved_weekday_bit") {
  ecg::Tariff reserved = ValidTariff();
  reserved.demand_charges[0].recurrence.weekday_mask = 0x80u;
  ECG_CHECK_ERR(ecg::ValidateTariff(reserved), ecg::ErrorCode::kInvalidArgument);

  ecg::Tariff with_reserved_and_days = ValidTariff();
  with_reserved_and_days.demand_charges[0].recurrence.weekday_mask = 0xFFu;
  ECG_CHECK_ERR(ecg::ValidateTariff(with_reserved_and_days), ecg::ErrorCode::kInvalidArgument);

  // Every real weekday bit is accepted.
  ecg::Tariff all_days = ValidTariff();
  all_days.demand_charges[0].recurrence.weekday_mask = 0x7Fu;
  ECG_CHECK_OK(ecg::ValidateTariff(all_days));
}

ECG_TEST("tariff.validate_rejects_a_non_positive_recurrence_duration") {
  ecg::Tariff zero = ValidTariff();
  zero.demand_charges[0].recurrence.duration_seconds = 0;
  ECG_CHECK_ERR(ecg::ValidateTariff(zero), ecg::ErrorCode::kInvalidArgument);

  ecg::Tariff negative = ValidTariff();
  negative.demand_charges[0].recurrence.duration_seconds = -3600;
  ECG_CHECK_ERR(ecg::ValidateTariff(negative), ecg::ErrorCode::kInvalidArgument);
}

ECG_TEST("tariff.validate_rejects_overlapping_price_intervals") {
  ecg::Tariff overlapping = ValidTariff();
  overlapping.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T05:00:00Z", "2026-01-01T09:00:00Z", 200000),
  };
  ECG_CHECK_ERR(ecg::ValidateTariff(overlapping), ecg::ErrorCode::kStateConflict);

  // A containing interval overlaps the one inside it.
  ecg::Tariff nested = ValidTariff();
  nested.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T12:00:00Z", 100000),
      Priced("2026-01-01T05:00:00Z", "2026-01-01T06:00:00Z", 200000),
  };
  ECG_CHECK_ERR(ecg::ValidateTariff(nested), ecg::ErrorCode::kStateConflict);

  // Touching intervals and a gap between intervals are both valid.
  ecg::Tariff touching = ValidTariff();
  touching.prices = {
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
      Priced("2026-01-01T18:00:00Z", "2026-01-01T20:00:00Z", 300000),
  };
  ECG_CHECK_OK(ecg::ValidateTariff(touching));

  // Supplied out of order, the same schedule is still accepted.
  ecg::Tariff shuffled = ValidTariff();
  shuffled.prices = {
      Priced("2026-01-01T06:00:00Z", "2026-01-01T12:00:00Z", 200000),
      Priced("2026-01-01T00:00:00Z", "2026-01-01T06:00:00Z", 100000),
  };
  ECG_CHECK_OK(ecg::ValidateTariff(shuffled));
}

ECG_TEST("tariff.validate_rejects_more_intervals_and_rules_than_allowed") {
  ecg::Tariff many_prices = ValidTariff();
  many_prices.prices.clear();
  const std::size_t price_count = ecg::kMaxPriceIntervals + 1;
  many_prices.prices.reserve(price_count);
  const std::int64_t step = 60 * kMicrosPerSecond;
  for (std::size_t i = 0; i < price_count; ++i) {
    const std::int64_t begin = static_cast<std::int64_t>(i) * step;
    many_prices.prices.push_back(
        ecg::PriceInterval{RawWindow(begin, begin + step), Price(100000)});
  }
  ECG_CHECK_ERR(ecg::ValidateTariff(many_prices), ecg::ErrorCode::kResourceLimitExceeded);

  ecg::Tariff many_rules = ValidTariff();
  many_rules.demand_charges.clear();
  const std::size_t rule_count = ecg::kMaxDemandChargeRules + 1;
  many_rules.demand_charges.reserve(rule_count);
  for (std::size_t i = 0; i < rule_count; ++i) {
    many_rules.demand_charges.push_back(DemandRule("dw/" + std::to_string(i)));
  }
  ECG_CHECK_ERR(ecg::ValidateTariff(many_rules), ecg::ErrorCode::kResourceLimitExceeded);

  // Exactly the maximum number of rules is accepted.
  ecg::Tariff at_rule_limit = ValidTariff();
  at_rule_limit.demand_charges.clear();
  at_rule_limit.demand_charges.reserve(ecg::kMaxDemandChargeRules);
  for (std::size_t i = 0; i < ecg::kMaxDemandChargeRules; ++i) {
    at_rule_limit.demand_charges.push_back(DemandRule("dw/" + std::to_string(i)));
  }
  ECG_CHECK_OK(ecg::ValidateTariff(at_rule_limit));
}
