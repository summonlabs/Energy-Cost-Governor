// Tests for deterministic time semantics: proleptic Gregorian calendar
// arithmetic, RFC 3339 parsing and formatting, half-open intervals, explicit
// zone rules, and the daylight-saving resolution rules of recurring windows.
//
// No test here consults the host clock or the host time zone database: every
// zone used below is constructed from an explicit transition table.

#include "test.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/time.hpp"

namespace {

constexpr std::int64_t kInt64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kInt64Min = (std::numeric_limits<std::int64_t>::min)();

/// Parses an instant, recording a failure when the timestamp itself is broken.
[[nodiscard]] ecg::UtcInstant At(std::string_view text) {
  const auto parsed = ecg::ParseRfc3339(text);
  ECG_CHECK_MSG(parsed.ok(), std::string("could not parse instant ") + std::string(text));
  if (!parsed.ok()) {
    return ecg::UtcInstant::FromRaw(0);
  }
  return parsed.value();
}

/// Builds a half-open interval from two RFC 3339 timestamps.
[[nodiscard]] ecg::Interval Window(std::string_view begin, std::string_view end) {
  const auto made = ecg::Interval::Make(At(begin), At(end));
  ECG_CHECK_MSG(made.ok(), std::string("interval ") + std::string(begin) + " .. " +
                               std::string(end) + " must be constructible");
  if (!made.ok()) {
    return ecg::Interval();
  }
  return made.value();
}

/// Asserts that an interval has exactly the given bounds.
void CheckInterval(const ecg::Interval& actual, std::string_view begin, std::string_view end,
                   const char* const what) {
  const ecg::UtcInstant expected_begin = At(begin);
  const ecg::UtcInstant expected_end = At(end);
  ECG_CHECK_MSG(actual.begin() == expected_begin && actual.end() == expected_end,
                std::string(what) + ": got [" + ecg::FormatRfc3339(actual.begin()) + ", " +
                    ecg::FormatRfc3339(actual.end()) + ")");
}

[[nodiscard]] ecg::ZoneTransition Transition(std::string_view at, std::int32_t offset_seconds) {
  return ecg::ZoneTransition{At(at), offset_seconds};
}

/// Builds zone rules, recording a failure when the table is rejected.
[[nodiscard]] ecg::ZoneRules MakeZone(std::int32_t initial_offset_seconds,
                                      std::vector<ecg::ZoneTransition> transitions) {
  const auto made = ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("test/zone"),
                                         initial_offset_seconds, std::move(transitions));
  ECG_CHECK_MSG(made.ok(), "zone rules must be constructible");
  if (!made.ok()) {
    return ecg::ZoneRules();
  }
  return made.value();
}

/// Central European style rules: +01:00 in winter, +02:00 in summer, changing at
/// 01:00 UTC on the last Sunday of March and of October 2026.
[[nodiscard]] ecg::ZoneRules CentralEuropeanZone() {
  std::vector<ecg::ZoneTransition> transitions;
  transitions.push_back(Transition("2026-03-29T01:00:00Z", 7200));
  transitions.push_back(Transition("2026-10-25T01:00:00Z", 3600));
  return MakeZone(3600, std::move(transitions));
}

[[nodiscard]] ecg::RecurringWindow Weekly(std::uint8_t weekday_mask,
                                          std::int32_t start_second_of_day,
                                          std::int32_t duration_seconds) {
  ecg::RecurringWindow window;
  window.weekday_mask = weekday_mask;
  window.start_second_of_day = start_second_of_day;
  window.duration_seconds = duration_seconds;
  return window;
}

}  // namespace

// ---------------------------------------------------------------------------
// Calendar arithmetic
// ---------------------------------------------------------------------------

ECG_TEST("time.days_from_civil_matches_known_epoch_days") {
  ECG_CHECK_EQ(ecg::DaysFromCivil(1970, 1, 1), std::int64_t{0});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1969, 12, 31), std::int64_t{-1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1970, 1, 2), std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1900, 1, 1), std::int64_t{-25567});
  ECG_CHECK_EQ(ecg::DaysFromCivil(2000, 1, 1), std::int64_t{10957});
  ECG_CHECK_EQ(ecg::DaysFromCivil(2000, 3, 1), std::int64_t{11017});

  // Consecutive days differ by one across every leap-day shape: an ordinary
  // leap year, a century that is not a leap year, and a 400-year leap year.
  ECG_CHECK_EQ(ecg::DaysFromCivil(1972, 2, 29) - ecg::DaysFromCivil(1972, 2, 28),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1972, 3, 1) - ecg::DaysFromCivil(1972, 2, 29),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(2000, 2, 29) - ecg::DaysFromCivil(2000, 2, 28),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(2000, 3, 1) - ecg::DaysFromCivil(2000, 2, 29),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1900, 3, 1) - ecg::DaysFromCivil(1900, 2, 28),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(2100, 3, 1) - ecg::DaysFromCivil(2100, 2, 28),
               std::int64_t{1});
  ECG_CHECK_EQ(ecg::DaysFromCivil(1971, 3, 1) - ecg::DaysFromCivil(1971, 2, 28),
               std::int64_t{1});
}

ECG_TEST("time.civil_from_days_is_the_inverse_of_days_from_civil") {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;

  ecg::CivilFromDays(0, &year, &month, &day);
  ECG_CHECK_EQ(year, 1970);
  ECG_CHECK_EQ(month, 1u);
  ECG_CHECK_EQ(day, 1u);

  ecg::CivilFromDays(-25567, &year, &month, &day);
  ECG_CHECK_EQ(year, 1900);
  ECG_CHECK_EQ(month, 1u);
  ECG_CHECK_EQ(day, 1u);

  ecg::CivilFromDays(11017, &year, &month, &day);
  ECG_CHECK_EQ(year, 2000);
  ECG_CHECK_EQ(month, 3u);
  ECG_CHECK_EQ(day, 1u);

  // A contiguous band of days around the epoch, including negative day numbers.
  for (std::int64_t days = -1000; days <= 1000; ++days) {
    ecg::CivilFromDays(days, &year, &month, &day);
    ECG_CHECK_EQ(ecg::DaysFromCivil(year, month, day), days);
  }

  // A seeded sample across the whole proleptic range the runtime accepts.
  ecgtest::Random random(0x7A11ED5EEDULL);
  const std::int64_t first_day = ecg::DaysFromCivil(1, 1, 1);
  const std::int64_t last_day = ecg::DaysFromCivil(9999, 12, 31);
  ECG_CHECK(first_day < 0);
  ECG_CHECK(last_day > 0);
  for (int i = 0; i < 2000; ++i) {
    const std::int64_t days = random.Range(first_day, last_day);
    ecg::CivilFromDays(days, &year, &month, &day);
    ECG_CHECK_EQ(ecg::DaysFromCivil(year, month, day), days);
  }
}

ECG_TEST("time.civil_time_validation_rejects_impossible_fields") {
  const auto valid = [](int y, int mo, int d, int h, int mi, int s, int us) {
    ecg::CivilTime civil;
    civil.year = y;
    civil.month = mo;
    civil.day = d;
    civil.hour = h;
    civil.minute = mi;
    civil.second = s;
    civil.microsecond = us;
    return ecg::IsValidCivilTime(civil);
  };

  ECG_CHECK(valid(1972, 2, 29, 0, 0, 0, 0));
  ECG_CHECK(valid(2000, 2, 29, 23, 59, 59, 999999));
  ECG_CHECK(!valid(1900, 2, 29, 0, 0, 0, 0));
  ECG_CHECK(!valid(2100, 2, 29, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 2, 30, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 4, 31, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 13, 1, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 0, 1, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 1, 0, 0, 0, 0, 0));
  ECG_CHECK(!valid(2026, 1, 1, 24, 0, 0, 0));
  ECG_CHECK(!valid(2026, 1, 1, 0, 60, 0, 0));
  ECG_CHECK(!valid(2026, 1, 1, 0, 0, 60, 0));  // Leap seconds are not a time of day here.
  ECG_CHECK(!valid(2026, 1, 1, 0, 0, 0, 1000000));
  ECG_CHECK(!valid(2026, 1, 1, -1, 0, 0, 0));
  ECG_CHECK(!valid(0, 1, 1, 0, 0, 0, 0));
  ECG_CHECK(!valid(10000, 1, 1, 0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// RFC 3339
// ---------------------------------------------------------------------------

ECG_TEST("time.format_rfc3339_is_fixed_width_utc") {
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(0)),
               std::string("1970-01-01T00:00:00.000000Z"));
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(1)),
               std::string("1970-01-01T00:00:00.000001Z"));
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(-1)),
               std::string("1969-12-31T23:59:59.999999Z"));
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(123456)),
               std::string("1970-01-01T00:00:00.123456Z"));
  // The Unix time of 1900-01-01T00:00:00Z, well before the epoch.
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(-2208988800000000LL)),
               std::string("1900-01-01T00:00:00.000000Z"));
  ECG_CHECK_EQ(ecg::FormatRfc3339(At("2026-03-01T04:30:00Z")),
               std::string("2026-03-01T04:30:00.000000Z"));
}

ECG_TEST("time.parse_and_format_rfc3339_round_trip") {
  const std::int64_t raws[] = {
      0,
      1,
      -1,
      999999,
      1000000,
      1234567890123456LL,
      -1234567890123456LL,
      -2208988800000000LL,
      253402300799999999LL,  // 9999-12-31T23:59:59.999999Z
  };
  for (const std::int64_t raw : raws) {
    const ecg::UtcInstant instant = ecg::UtcInstant::FromRaw(raw);
    const std::string text = ecg::FormatRfc3339(instant);
    ECG_CHECK_EQ(text.size(), static_cast<std::size_t>(27));
    const auto parsed = ecg::ParseRfc3339(text);
    ECG_CHECK_OK(parsed);
    if (parsed.ok()) {
      ECG_CHECK(parsed.value() == instant);
    }
  }
  ECG_CHECK_EQ(ecg::FormatRfc3339(ecg::UtcInstant::FromRaw(253402300799999999LL)),
               std::string("9999-12-31T23:59:59.999999Z"));
}

ECG_TEST("time.parse_rfc3339_applies_offsets_and_accepts_documented_separators") {
  const ecg::UtcInstant utc = At("2026-03-01T02:30:00Z");
  ECG_CHECK(At("2026-03-01T04:30:00+02:00") == utc);
  ECG_CHECK(At("2026-03-01T04:30:00+0200") == utc);
  ECG_CHECK(At("2026-03-01T04:30:00+02") == utc);
  ECG_CHECK(At("2026-03-01T02:30:00+00:00") == utc);
  ECG_CHECK(At("2026-03-01 04:30:00+02:00") == utc);
  ECG_CHECK(At("2026-03-01t04:30:00+02:00") == utc);
  // A lower-case z is the UTC designator, not an offset: 04:30 local is 04:30Z.
  ECG_CHECK(At("2026-03-01T04:30:00z") == At("2026-03-01T04:30:00Z"));
  ECG_CHECK(At("2026-03-01T04:30:00z") == ecg::UtcInstant::FromRaw(utc.raw() + 2 * 3600 * 1000000LL));
  ECG_CHECK(At("2026-03-01T04:30:00-05:30") == At("2026-03-01T10:00:00Z"));
  ECG_CHECK(At("2026-03-01T04:30:00-0530") == At("2026-03-01T10:00:00Z"));
  ECG_CHECK(At("2026-03-01T04:30:00-05") == At("2026-03-01T09:30:00Z"));

  // An instant before the epoch, reached from a negative local offset.
  ECG_CHECK(At("1969-12-31T19:00:00-05:00") == ecg::UtcInstant::FromRaw(0));
}

ECG_TEST("time.parse_rfc3339_keeps_microsecond_precision") {
  const auto precise = ecg::ParseRfc3339("2026-03-01T04:30:00.123456Z");
  ECG_CHECK_OK(precise);
  if (precise.ok()) {
    ECG_CHECK_EQ(ecg::FormatRfc3339(precise.value()), std::string("2026-03-01T04:30:00.123456Z"));
  }

  const auto padded = ecg::ParseRfc3339("2026-03-01T04:30:00.5Z");
  ECG_CHECK_OK(padded);
  if (padded.ok()) {
    ECG_CHECK_EQ(ecg::FormatRfc3339(padded.value()), std::string("2026-03-01T04:30:00.500000Z"));
    ECG_CHECK_EQ(padded.value().raw() - At("2026-03-01T04:30:00Z").raw(),
                 std::int64_t{500000});
  }

  const auto one_micro = ecg::ParseRfc3339("2026-03-01T04:30:00.000001Z");
  ECG_CHECK_OK(one_micro);
  if (one_micro.ok()) {
    ECG_CHECK_EQ(one_micro.value().raw() - At("2026-03-01T04:30:00Z").raw(), std::int64_t{1});
  }

  const auto millis = ecg::ParseRfc3339("2026-03-01T04:30:00.123Z");
  ECG_CHECK_OK(millis);
  if (millis.ok()) {
    ECG_CHECK_EQ(millis.value().raw() - At("2026-03-01T04:30:00Z").raw(), std::int64_t{123000});
  }

  // The offset applies to the microsecond-precise local time.
  ECG_CHECK(At("2026-03-01T06:30:00.123456+02:00") ==
            ecg::UtcInstant::FromRaw(At("2026-03-01T04:30:00Z").raw() + 123456));
}

ECG_TEST("time.parse_rfc3339_rejects_malformed_timestamps") {
  const char* const rejected[] = {
      "",
      "not a timestamp",
      "2026-03-01",
      "2026-03-01T04:30:00",             // no offset designator
      "2026-03-01T04:30:00.5",           // no offset after a fraction
      "2026-03-01T04:30:00.Z",           // decimal point without digits
      "2026-03-01T04:30:00.1234567Z",    // beyond microsecond precision
      "2026-03-01T04:30:00.12345678Z",   // beyond microsecond precision
      "2026-03-01T23:59:60Z",            // leap second
      "2026-03-01T24:00:00Z",            // hour 24
      "2026-03-01T04:60:00Z",            // minute 60
      "2026-03-01T04:30:61Z",            // second 61
      "2026-02-30T00:00:00Z",            // day that does not exist
      "2026-13-01T00:00:00Z",            // month 13
      "2026-03-01T04:30:00Zx",           // trailing character
      "2026-03-01T04:30:00Z ",           // trailing space
      "2026-03-01T04:30:00ZZ",           // two designators
      "2026-03-01X04:30:00Z",            // wrong separator
      "2026-3-01T04:30:00Z",             // short month field
      "26-03-01T04:30:00Z",              // short year field
      "2026-03-01T4:30:00Z",             // short hour field
      "2026-03-01T04:3:00Z",             // short minute field
      "2026-03-01T04:30:0Z",             // short second field
      "2026-03-01T04:30:00+2:00Z",       // short offset hours
      "2026-03-01T04:30:00+19:00",       // offset beyond 18 hours
      "2026-03-01T04:30:00+02:60",       // offset minute 60
      "2026-03-01T04:30:00+",            // offset sign with nothing after it
      "2026-03-01T04:30:00-",            // offset sign with nothing after it
  };
  for (const char* const text : rejected) {
    const auto parsed = ecg::ParseRfc3339(text);
    ECG_CHECK_MSG(!parsed.ok(), std::string("must be rejected: ") + text);
  }

  // The accepted edge of the offset range is exactly +/-18:00.
  ECG_CHECK_OK(ecg::ParseRfc3339("2026-03-01T04:30:00+18:00"));
  ECG_CHECK_OK(ecg::ParseRfc3339("2026-03-01T04:30:00-18:00"));
  // A leap day that really exists is accepted.
  ECG_CHECK_OK(ecg::ParseRfc3339("2000-02-29T00:00:00Z"));
  ECG_CHECK(!ecg::ParseRfc3339("1900-02-29T00:00:00Z").ok());
}

// ---------------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------------

ECG_TEST("time.interval_make_rejects_degenerate_bounds") {
  const ecg::UtcInstant begin = At("2026-01-01T00:00:00Z");
  const ecg::UtcInstant one_micro_later = ecg::UtcInstant::FromRaw(begin.raw() + 1);

  ECG_CHECK_ERR(ecg::Interval::Make(begin, begin), ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_ERR(ecg::Interval::Make(begin, ecg::UtcInstant::FromRaw(begin.raw() - 1)),
                ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_OK(ecg::Interval::Make(begin, one_micro_later));
  ECG_CHECK_OK(ecg::Interval::Make(ecg::UtcInstant::FromRaw(kInt64Min),
                                   ecg::UtcInstant::FromRaw(kInt64Max)));

  const ecg::Interval second = Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
  ECG_CHECK_EQ(second.duration().raw(), std::int64_t{3600});
  // Sub-second bounds are truncated toward zero, by documented design.
  const ecg::Interval fraction = Window("2026-01-01T00:00:00Z", "2026-01-01T00:00:01.500000Z");
  ECG_CHECK_EQ(fraction.duration().raw(), std::int64_t{1});
  ECG_CHECK_EQ(second.contains(second.begin()), true);
  ECG_CHECK_EQ(second.contains(second.end()), false);
}

ECG_TEST("time.interval_contains_is_half_open") {
  const ecg::Interval window = Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
  ECG_CHECK(window.contains(window.begin()));
  ECG_CHECK(window.contains(ecg::UtcInstant::FromRaw(window.begin().raw() + 1)));
  ECG_CHECK(window.contains(ecg::UtcInstant::FromRaw(window.end().raw() - 1)));
  ECG_CHECK(!window.contains(window.end()));
  ECG_CHECK(!window.contains(ecg::UtcInstant::FromRaw(window.begin().raw() - 1)));
  ECG_CHECK(!window.contains(ecg::UtcInstant::FromRaw(window.end().raw() + 1)));

  ECG_CHECK(window.contains(window));
  ECG_CHECK(window.contains(Window("2026-01-01T00:10:00Z", "2026-01-01T00:20:00Z")));
  ECG_CHECK(window.contains(Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")));
  ECG_CHECK(window.contains(Window("2026-01-01T00:50:00Z", "2026-01-01T01:00:00Z")));
  ECG_CHECK(!window.contains(Window("2026-01-01T00:50:00Z", "2026-01-01T01:00:00.000001Z")));
  ECG_CHECK(!window.contains(Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z")));
  ECG_CHECK(!window.contains(Window("2025-12-31T23:00:00Z", "2026-01-01T00:00:00Z")));

  ECG_CHECK(!window.overlaps(Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z")));
  ECG_CHECK(!window.overlaps(Window("2025-12-31T23:00:00Z", "2026-01-01T00:00:00Z")));
  ECG_CHECK(window.overlaps(Window("2026-01-01T00:59:59Z", "2026-01-01T02:00:00Z")));
  ECG_CHECK(window.overlaps(Window("2025-12-31T23:00:00Z", "2026-01-01T00:00:00.000001Z")));
}

ECG_TEST("time.interval_intersect_of_touching_intervals_is_empty") {
  const ecg::Interval first = Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
  const ecg::Interval touching = Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z");
  const ecg::Interval before = Window("2025-12-31T23:00:00Z", "2026-01-01T00:00:00Z");

  const auto no_overlap = first.Intersect(touching);
  ECG_CHECK_OK(no_overlap);
  if (no_overlap.ok()) {
    ECG_CHECK(!no_overlap.value().has_value());
  }
  const auto also_none = first.Intersect(before);
  ECG_CHECK_OK(also_none);
  if (also_none.ok()) {
    ECG_CHECK(!also_none.value().has_value());
  }

  const auto overlap = first.Intersect(Window("2026-01-01T00:30:00Z", "2026-01-01T02:00:00Z"));
  ECG_CHECK_OK(overlap);
  if (overlap.ok() && overlap.value().has_value()) {
    CheckInterval(*overlap.value(), "2026-01-01T00:30:00Z", "2026-01-01T01:00:00Z",
                  "clipped intersection");
  } else {
    ECG_CHECK_MSG(false, "expected an intersection");
  }

  const auto nested = first.Intersect(Window("2026-01-01T00:10:00Z", "2026-01-01T00:20:00Z"));
  ECG_CHECK_OK(nested);
  if (nested.ok() && nested.value().has_value()) {
    CheckInterval(*nested.value(), "2026-01-01T00:10:00Z", "2026-01-01T00:20:00Z",
                  "nested intersection");
  } else {
    ECG_CHECK_MSG(false, "expected a nested intersection");
  }

  const auto identical = first.Intersect(first);
  ECG_CHECK_OK(identical);
  if (identical.ok() && identical.value().has_value()) {
    CheckInterval(*identical.value(), "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z",
                  "self intersection");
  } else {
    ECG_CHECK_MSG(false, "expected a self intersection");
  }
}

ECG_TEST("time.coalesce_merges_touching_and_overlapping_intervals") {
  const std::vector<ecg::Interval> unsorted = {
      Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z"),
      Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"),
      Window("2026-01-01T03:00:00Z", "2026-01-01T04:00:00Z"),
      Window("2026-01-01T03:30:00Z", "2026-01-01T05:00:00Z"),
  };
  const std::vector<ecg::Interval> merged = ecg::CoalesceIntervals(unsorted);
  ECG_CHECK_EQ(merged.size(), static_cast<std::size_t>(2));
  if (merged.size() == 2u) {
    CheckInterval(merged[0], "2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z", "touching pair");
    CheckInterval(merged[1], "2026-01-01T03:00:00Z", "2026-01-01T05:00:00Z", "overlapping pair");
  }

  const std::vector<ecg::Interval> nested = {
      Window("2026-01-01T00:00:00Z", "2026-01-01T04:00:00Z"),
      Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z"),
  };
  const std::vector<ecg::Interval> one = ecg::CoalesceIntervals(nested);
  ECG_CHECK_EQ(one.size(), static_cast<std::size_t>(1));
  if (one.size() == 1u) {
    CheckInterval(one[0], "2026-01-01T00:00:00Z", "2026-01-01T04:00:00Z", "containing interval");
  }

  const std::vector<ecg::Interval> disjoint = {
      Window("2026-01-02T00:00:00Z", "2026-01-02T01:00:00Z"),
      Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"),
  };
  const std::vector<ecg::Interval> kept = ecg::CoalesceIntervals(disjoint);
  ECG_CHECK_EQ(kept.size(), static_cast<std::size_t>(2));
  if (kept.size() == 2u) {
    CheckInterval(kept[0], "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z", "sorted first");
    CheckInterval(kept[1], "2026-01-02T00:00:00Z", "2026-01-02T01:00:00Z", "sorted second");
  }

  ECG_CHECK(ecg::CoalesceIntervals({}).empty());
  ECG_CHECK_EQ(ecg::CoalesceIntervals({Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")})
                   .size(),
               static_cast<std::size_t>(1));
}

ECG_TEST("time.subtract_intervals_removes_holes_exactly") {
  // A hole in the middle splits one interval in two.
  const std::vector<ecg::Interval> split = ecg::SubtractIntervals(
      {Window("2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z")},
      {Window("2026-01-01T10:00:00Z", "2026-01-01T12:00:00Z")});
  ECG_CHECK_EQ(split.size(), static_cast<std::size_t>(2));
  if (split.size() == 2u) {
    CheckInterval(split[0], "2026-01-01T00:00:00Z", "2026-01-01T10:00:00Z", "left of the hole");
    CheckInterval(split[1], "2026-01-01T12:00:00Z", "2026-01-02T00:00:00Z", "right of the hole");
  }

  // A hole that covers everything leaves nothing.
  ECG_CHECK(ecg::SubtractIntervals({Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")},
                                   {Window("2025-12-31T00:00:00Z", "2026-01-02T00:00:00Z")})
                .empty());
  ECG_CHECK(ecg::SubtractIntervals({Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")},
                                   {Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")})
                .empty());

  // A hole that touches nothing leaves the base unchanged.
  const std::vector<ecg::Interval> untouched = ecg::SubtractIntervals(
      {Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")},
      {Window("2026-01-01T05:00:00Z", "2026-01-01T06:00:00Z")});
  ECG_CHECK_EQ(untouched.size(), static_cast<std::size_t>(1));
  if (untouched.size() == 1u) {
    CheckInterval(untouched[0], "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z",
                  "hole outside the base");
  }

  // A hole that touches only the end of a base interval trims it exactly.
  const std::vector<ecg::Interval> trimmed = ecg::SubtractIntervals(
      {Window("2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z")},
      {Window("2026-01-01T01:00:00Z", "2026-01-01T03:00:00Z")});
  ECG_CHECK_EQ(trimmed.size(), static_cast<std::size_t>(1));
  if (trimmed.size() == 1u) {
    CheckInterval(trimmed[0], "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z",
                  "hole overlapping the end");
  }

  // Several base intervals and several holes, supplied out of order.
  const std::vector<ecg::Interval> multiple = ecg::SubtractIntervals(
      {Window("2026-01-01T02:00:00Z", "2026-01-01T04:00:00Z"),
       Window("2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z"),
       Window("2026-01-01T06:00:00Z", "2026-01-01T08:00:00Z")},
      {Window("2026-01-01T07:00:00Z", "2026-01-01T09:00:00Z"),
       Window("2026-01-01T01:00:00Z", "2026-01-01T01:30:00Z")});
  ECG_CHECK_EQ(multiple.size(), static_cast<std::size_t>(3));
  if (multiple.size() == 3u) {
    CheckInterval(multiple[0], "2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z", "first piece");
    CheckInterval(multiple[1], "2026-01-01T01:30:00Z", "2026-01-01T04:00:00Z", "merged middle");
    CheckInterval(multiple[2], "2026-01-01T06:00:00Z", "2026-01-01T07:00:00Z", "last piece");
  }
}

ECG_TEST("time.find_first_overlap_and_disjointness_agree") {
  const std::vector<ecg::Interval> disjoint = {
      Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"),
      Window("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z"),
      Window("2026-01-01T03:00:00Z", "2026-01-01T04:00:00Z"),
  };
  ECG_CHECK(ecg::IntervalsAreDisjoint(disjoint));
  ECG_CHECK(!ecg::FindFirstOverlap(disjoint).has_value());

  const std::vector<ecg::Interval> overlapping = {
      Window("2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z"),
      Window("2026-01-01T03:00:00Z", "2026-01-01T04:00:00Z"),
      Window("2026-01-01T01:00:00Z", "2026-01-01T01:30:00Z"),
  };
  ECG_CHECK(!ecg::IntervalsAreDisjoint(overlapping));
  const auto found = ecg::FindFirstOverlap(overlapping);
  ECG_CHECK(found.has_value());
  if (found.has_value()) {
    ECG_CHECK(found->first.overlaps(found->second));
    CheckInterval(found->first, "2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z", "first of the pair");
    CheckInterval(found->second, "2026-01-01T01:00:00Z", "2026-01-01T01:30:00Z",
                  "second of the pair");
  }

  ECG_CHECK(ecg::IntervalsAreDisjoint({}));
  ECG_CHECK(!ecg::FindFirstOverlap({}).has_value());
  ECG_CHECK(ecg::IntervalsAreDisjoint({Window("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z")}));

  // Clipping keeps only the part inside the horizon.
  const std::vector<ecg::Interval> clipped = ecg::ClipIntervals(
      {Window("2025-12-31T23:00:00Z", "2026-01-01T00:30:00Z"),
       Window("2026-01-01T00:30:00Z", "2026-01-01T01:00:00Z"),
       Window("2026-01-01T05:00:00Z", "2026-01-01T06:00:00Z")},
      Window("2026-01-01T00:00:00Z", "2026-01-01T02:00:00Z"));
  ECG_CHECK_EQ(clipped.size(), static_cast<std::size_t>(2));
  if (clipped.size() == 2u) {
    CheckInterval(clipped[0], "2026-01-01T00:00:00Z", "2026-01-01T00:30:00Z", "clipped start");
    CheckInterval(clipped[1], "2026-01-01T00:30:00Z", "2026-01-01T01:00:00Z", "inside horizon");
  }
}

// ---------------------------------------------------------------------------
// Zone rules
// ---------------------------------------------------------------------------

ECG_TEST("time.zone_offset_at_uses_the_new_offset_at_the_transition") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  ECG_CHECK_EQ(zone.initial_offset_seconds(), 3600);
  ECG_CHECK_EQ(zone.transitions().size(), static_cast<std::size_t>(2));

  ECG_CHECK_EQ(zone.OffsetAt(At("2020-01-01T00:00:00Z")), 3600);
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-03-29T00:59:59.999999Z")), 3600);
  // The transition instant itself already carries the new offset.
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-03-29T01:00:00Z")), 7200);
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-03-29T01:00:00.000001Z")), 7200);
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-06-01T00:00:00Z")), 7200);
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-10-25T00:59:59.999999Z")), 7200);
  ECG_CHECK_EQ(zone.OffsetAt(At("2026-10-25T01:00:00Z")), 3600);
  ECG_CHECK_EQ(zone.OffsetAt(At("2030-01-01T00:00:00Z")), 3600);

  // A zone with no transitions keeps its initial offset forever.
  const ecg::ZoneRules fixed = MakeZone(-18000, {});
  ECG_CHECK_EQ(fixed.OffsetAt(At("1900-01-01T00:00:00Z")), -18000);
  ECG_CHECK_EQ(fixed.OffsetAt(At("2100-01-01T00:00:00Z")), -18000);
}

ECG_TEST("time.zone_resolve_local_prefers_the_earlier_occurrence_in_an_overlap") {
  const ecg::ZoneRules zone = CentralEuropeanZone();

  ecg::CivilTime local;
  local.year = 2026;
  local.month = 10;
  local.day = 25;
  local.hour = 2;
  local.minute = 30;
  local.second = 0;

  const auto ambiguous = zone.ResolveLocal(local);
  ECG_CHECK_OK(ambiguous);
  if (ambiguous.ok()) {
    // The earlier occurrence uses the +02:00 offset that was in force before.
    ECG_CHECK(ambiguous.value() == At("2026-10-25T00:30:00Z"));
  }

  local.minute = 0;
  const auto overlap_start = zone.ResolveLocal(local);
  ECG_CHECK_OK(overlap_start);
  if (overlap_start.ok()) {
    ECG_CHECK(overlap_start.value() == At("2026-10-25T00:00:00Z"));
  }

  // One second before the overlap the local reading is unambiguous.
  local.hour = 1;
  local.minute = 59;
  local.second = 59;
  const auto before_overlap = zone.ResolveLocal(local);
  ECG_CHECK_OK(before_overlap);
  if (before_overlap.ok()) {
    ECG_CHECK(before_overlap.value() == At("2026-10-24T23:59:59Z"));
  }

  local.hour = 3;
  local.minute = 0;
  local.second = 0;
  const auto after_overlap = zone.ResolveLocal(local);
  ECG_CHECK_OK(after_overlap);
  if (after_overlap.ok()) {
    ECG_CHECK(after_overlap.value() == At("2026-10-25T02:00:00Z"));
  }
}

ECG_TEST("time.zone_resolve_local_rejects_a_local_time_inside_a_gap") {
  const ecg::ZoneRules zone = CentralEuropeanZone();

  ecg::CivilTime local;
  local.year = 2026;
  local.month = 3;
  local.day = 29;
  local.hour = 2;
  local.minute = 30;

  const auto in_gap = zone.ResolveLocal(local);
  ECG_CHECK_ERR(in_gap, ecg::ErrorCode::kNotFound);

  local.minute = 0;
  ECG_CHECK_ERR(zone.ResolveLocal(local), ecg::ErrorCode::kNotFound);  // first instant of the gap
  local.hour = 2;
  local.minute = 59;
  local.second = 59;
  ECG_CHECK_ERR(zone.ResolveLocal(local), ecg::ErrorCode::kNotFound);  // last instant of the gap

  // The instant the new offset takes effect does exist, at 03:00 local.
  local.hour = 3;
  local.minute = 0;
  local.second = 0;
  const auto gap_end = zone.ResolveLocal(local);
  ECG_CHECK_OK(gap_end);
  if (gap_end.ok()) {
    ECG_CHECK(gap_end.value() == At("2026-03-29T01:00:00Z"));
  }

  local.hour = 1;
  local.minute = 59;
  local.second = 59;
  const auto before_gap = zone.ResolveLocal(local);
  ECG_CHECK_OK(before_gap);
  if (before_gap.ok()) {
    ECG_CHECK(before_gap.value() == At("2026-03-29T00:59:59Z"));
  }

  // An impossible calendar date is rejected before any zone reasoning happens.
  local.hour = 12;
  local.minute = 0;
  local.second = 0;
  local.month = 2;
  local.day = 30;
  ECG_CHECK_ERR(zone.ResolveLocal(local), ecg::ErrorCode::kOutOfRange);
}

ECG_TEST("time.zone_to_local_inverts_on_both_sides_of_a_transition") {
  const ecg::ZoneRules zone = CentralEuropeanZone();

  const ecg::CivilTime before = zone.ToLocal(At("2026-03-29T00:30:00Z"));
  ECG_CHECK_EQ(before.year, 2026);
  ECG_CHECK_EQ(before.month, 3);
  ECG_CHECK_EQ(before.day, 29);
  ECG_CHECK_EQ(before.hour, 1);
  ECG_CHECK_EQ(before.minute, 30);
  ECG_CHECK_EQ(before.second, 0);

  const ecg::CivilTime at_transition = zone.ToLocal(At("2026-03-29T01:00:00Z"));
  ECG_CHECK_EQ(at_transition.hour, 3);
  ECG_CHECK_EQ(at_transition.minute, 0);

  const ecg::CivilTime after = zone.ToLocal(At("2026-03-29T01:30:00Z"));
  ECG_CHECK_EQ(after.hour, 3);
  ECG_CHECK_EQ(after.minute, 30);

  // Resolving the local reading returns the original instant on both sides.
  const auto round_trip_before = zone.ResolveLocal(before);
  ECG_CHECK_OK(round_trip_before);
  if (round_trip_before.ok()) {
    ECG_CHECK(round_trip_before.value() == At("2026-03-29T00:30:00Z"));
  }
  const auto round_trip_after = zone.ResolveLocal(after);
  ECG_CHECK_OK(round_trip_after);
  if (round_trip_after.ok()) {
    ECG_CHECK(round_trip_after.value() == At("2026-03-29T01:30:00Z"));
  }

  // Across the fall-back both passes read 02:30 local, but only the first
  // resolves back; the second is the ambiguous occurrence.
  const ecg::CivilTime first_pass = zone.ToLocal(At("2026-10-25T00:30:00Z"));
  const ecg::CivilTime second_pass = zone.ToLocal(At("2026-10-25T01:30:00Z"));
  ECG_CHECK_EQ(first_pass.hour, 2);
  ECG_CHECK_EQ(first_pass.minute, 30);
  ECG_CHECK_EQ(second_pass.hour, 2);
  ECG_CHECK_EQ(second_pass.minute, 30);
  const auto resolved_first = zone.ResolveLocal(first_pass);
  ECG_CHECK_OK(resolved_first);
  if (resolved_first.ok()) {
    ECG_CHECK(resolved_first.value() == At("2026-10-25T00:30:00Z"));
  }
  const auto resolved_second = zone.ResolveLocal(second_pass);
  ECG_CHECK_OK(resolved_second);
  if (resolved_second.ok()) {
    ECG_CHECK(resolved_second.value() == At("2026-10-25T00:30:00Z"));
  }
}

ECG_TEST("time.zone_rules_reject_invalid_transition_tables") {
  ECG_CHECK_ERR(ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("z"), 3600,
                                     {Transition("2026-03-29T01:00:00Z", 7200),
                                      Transition("2026-03-29T01:00:00Z", 3600)}),
                ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_ERR(ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("z"), 3600,
                                     {Transition("2026-03-29T01:00:00Z", 7200),
                                      Transition("2026-03-29T00:00:00Z", 3600)}),
                ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_ERR(ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("z"), 3600,
                                     {Transition("2026-03-29T01:00:00Z", 19 * 3600)}),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("z"), 19 * 3600, {}),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_OK(ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("z"), -18 * 3600, {}));
}

// ---------------------------------------------------------------------------
// Recurring windows
// ---------------------------------------------------------------------------

ECG_TEST("time.expand_recurring_produces_the_right_utc_instants") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  // Mondays, 12:00 local, one hour long.
  const ecg::RecurringWindow window = Weekly(0x01u, 12 * 3600, 3600);
  const ecg::Interval horizon = Window("2026-01-05T00:00:00Z", "2026-01-13T00:00:00Z");

  const auto expanded = ecg::ExpandRecurring(window, zone, horizon);
  ECG_CHECK_OK(expanded);
  if (!expanded.ok()) {
    return;
  }
  const std::vector<ecg::Interval>& occurrences = expanded.value();
  ECG_CHECK_EQ(occurrences.size(), static_cast<std::size_t>(2));
  if (occurrences.size() == 2u) {
    // Winter: local 12:00 is UTC 11:00.
    CheckInterval(occurrences[0], "2026-01-05T11:00:00Z", "2026-01-05T12:00:00Z", "first Monday");
    CheckInterval(occurrences[1], "2026-01-12T11:00:00Z", "2026-01-12T12:00:00Z", "second Monday");
  }

  // A Sunday window uses Sunday's bit, not Monday's. 2026-01-04 is a Sunday but
  // its window ends before the horizon starts, so only 2026-01-11 survives.
  const ecg::RecurringWindow sundays = Weekly(0x40u, 12 * 3600, 3600);
  const auto sunday_occurrences = ecg::ExpandRecurring(sundays, zone, horizon);
  ECG_CHECK_OK(sunday_occurrences);
  if (sunday_occurrences.ok()) {
    ECG_CHECK_EQ(sunday_occurrences.value().size(), static_cast<std::size_t>(1));
    if (sunday_occurrences.value().size() == 1u) {
      CheckInterval(sunday_occurrences.value()[0], "2026-01-11T11:00:00Z", "2026-01-11T12:00:00Z",
                    "Sunday inside the horizon");
    }
  }
}

ECG_TEST("time.expand_recurring_starts_a_gap_window_at_the_transition") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  // Sundays at 02:30 local, the wall-clock time that does not exist on
  // 2026-03-29 because the clock jumps from 02:00 to 03:00.
  const ecg::RecurringWindow window = Weekly(0x40u, 2 * 3600 + 30 * 60, 3600);
  const ecg::Interval horizon = Window("2026-03-29T00:00:00Z", "2026-03-29T04:00:00Z");

  const auto expanded = ecg::ExpandRecurring(window, zone, horizon);
  ECG_CHECK_OK(expanded);
  if (!expanded.ok()) {
    return;
  }
  ECG_CHECK_EQ(expanded.value().size(), static_cast<std::size_t>(1));
  if (expanded.value().size() == 1u) {
    CheckInterval(expanded.value()[0], "2026-03-29T01:00:00Z", "2026-03-29T02:00:00Z",
                  "window starting inside the spring-forward gap");
  }
}

ECG_TEST("time.expand_recurring_starts_an_overlap_window_at_the_first_occurrence") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  // Sundays at 02:30 local, which occurs twice on 2026-10-25.
  const ecg::RecurringWindow window = Weekly(0x40u, 2 * 3600 + 30 * 60, 1800);
  const ecg::Interval horizon = Window("2026-10-25T00:00:00Z", "2026-10-25T04:00:00Z");

  const auto expanded = ecg::ExpandRecurring(window, zone, horizon);
  ECG_CHECK_OK(expanded);
  if (!expanded.ok()) {
    return;
  }
  ECG_CHECK_EQ(expanded.value().size(), static_cast<std::size_t>(1));
  if (expanded.value().size() == 1u) {
    CheckInterval(expanded.value()[0], "2026-10-25T00:30:00Z", "2026-10-25T01:00:00Z",
                  "window starting inside the fall-back overlap");
  }
}

ECG_TEST("time.expand_recurring_keeps_the_nominal_local_start_across_a_transition") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  // Sundays at 00:30 local for two hours: the window merely spans the 01:00 UTC
  // transition, so it starts at the ordinary local reading.
  const ecg::RecurringWindow window = Weekly(0x40u, 30 * 60, 7200);
  const ecg::Interval horizon = Window("2026-03-28T00:00:00Z", "2026-03-30T00:00:00Z");

  const auto expanded = ecg::ExpandRecurring(window, zone, horizon);
  ECG_CHECK_OK(expanded);
  if (!expanded.ok()) {
    return;
  }
  ECG_CHECK_EQ(expanded.value().size(), static_cast<std::size_t>(1));
  if (expanded.value().size() == 1u) {
    CheckInterval(expanded.value()[0], "2026-03-28T23:30:00Z", "2026-03-29T01:30:00Z",
                  "window spanning a transition");
  }
}

ECG_TEST("time.expand_recurring_clips_occurrences_to_the_horizon") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  // Every day at 00:00 local for three hours.
  const ecg::RecurringWindow window = Weekly(0x7Fu, 0, 10800);
  const ecg::Interval horizon = Window("2026-03-29T00:00:00Z", "2026-03-29T01:00:00Z");

  const auto expanded = ecg::ExpandRecurring(window, zone, horizon);
  ECG_CHECK_OK(expanded);
  if (!expanded.ok()) {
    return;
  }
  ECG_CHECK_EQ(expanded.value().size(), static_cast<std::size_t>(1));
  if (expanded.value().size() == 1u) {
    CheckInterval(expanded.value()[0], "2026-03-29T00:00:00Z", "2026-03-29T01:00:00Z",
                  "clipped to the horizon");
  }

  // A horizon that ends before any occurrence yields nothing.
  const auto empty = ecg::ExpandRecurring(window, zone,
                                          Window("2026-03-29T05:00:00Z", "2026-03-29T06:00:00Z"));
  ECG_CHECK_OK(empty);
  if (empty.ok()) {
    ECG_CHECK(empty.value().empty());
  }
}

ECG_TEST("time.expand_recurring_rejects_impossible_windows") {
  const ecg::ZoneRules zone = CentralEuropeanZone();
  const ecg::Interval horizon = Window("2026-03-29T00:00:00Z", "2026-03-30T00:00:00Z");

  const auto no_days = ecg::ExpandRecurring(Weekly(0x00u, 12 * 3600, 3600), zone, horizon);
  ECG_CHECK_OK(no_days);
  if (no_days.ok()) {
    ECG_CHECK(no_days.value().empty());
  }

  ECG_CHECK_ERR(ecg::ExpandRecurring(Weekly(0x01u, 12 * 3600, 0), zone, horizon),
                ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_ERR(ecg::ExpandRecurring(Weekly(0x01u, 12 * 3600, -1), zone, horizon),
                ecg::ErrorCode::kInvalidArgument);
  ECG_CHECK_ERR(ecg::ExpandRecurring(Weekly(0x01u, 86400, 3600), zone, horizon),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::ExpandRecurring(Weekly(0x01u, -1, 3600), zone, horizon),
                ecg::ErrorCode::kOutOfRange);
  ECG_CHECK_ERR(ecg::ExpandRecurring(Weekly(0x81u, 12 * 3600, 3600), zone, horizon),
                ecg::ErrorCode::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// Instant arithmetic
// ---------------------------------------------------------------------------

ECG_TEST("time.exceeds_by_compares_at_microsecond_precision") {
  const ecg::UtcInstant base = At("2026-01-01T00:00:00Z");
  const ecg::UtcInstant one_micro_later = ecg::UtcInstant::FromRaw(base.raw() + 1);
  const ecg::UtcInstant one_micro_earlier = ecg::UtcInstant::FromRaw(base.raw() - 1);

  const auto identical = ecg::ExceedsBy(base, base, ecg::DurationSec::FromRaw(0));
  ECG_CHECK_OK(identical);
  if (identical.ok()) {
    ECG_CHECK_EQ(identical.value(), false);
  }

  // A sub-second overrun is an overrun even when the limit is whole seconds.
  const auto sub_second = ecg::ExceedsBy(base, one_micro_later, ecg::DurationSec::FromRaw(0));
  ECG_CHECK_OK(sub_second);
  if (sub_second.ok()) {
    ECG_CHECK_EQ(sub_second.value(), true);
  }

  // An earlier instant never exceeds a non-negative limit.
  const auto backwards = ecg::ExceedsBy(base, one_micro_earlier, ecg::DurationSec::FromRaw(0));
  ECG_CHECK_OK(backwards);
  if (backwards.ok()) {
    ECG_CHECK_EQ(backwards.value(), false);
  }

  // Strictly more than the limit: exactly the limit is not an overrun.
  const auto exactly_one_second =
      ecg::ExceedsBy(base, ecg::UtcInstant::FromRaw(base.raw() + 1000000),
                     ecg::DurationSec::FromRaw(1));
  ECG_CHECK_OK(exactly_one_second);
  if (exactly_one_second.ok()) {
    ECG_CHECK_EQ(exactly_one_second.value(), false);
  }
  const auto one_micro_over =
      ecg::ExceedsBy(base, ecg::UtcInstant::FromRaw(base.raw() + 1000001),
                     ecg::DurationSec::FromRaw(1));
  ECG_CHECK_OK(one_micro_over);
  if (one_micro_over.ok()) {
    ECG_CHECK_EQ(one_micro_over.value(), true);
  }
  const auto one_micro_under =
      ecg::ExceedsBy(base, ecg::UtcInstant::FromRaw(base.raw() + 999999),
                     ecg::DurationSec::FromRaw(1));
  ECG_CHECK_OK(one_micro_under);
  if (one_micro_under.ok()) {
    ECG_CHECK_EQ(one_micro_under.value(), false);
  }

  const auto one_hour =
      ecg::ExceedsBy(base, At("2026-01-01T01:00:00Z"), ecg::DurationSec::FromRaw(3599));
  ECG_CHECK_OK(one_hour);
  if (one_hour.ok()) {
    ECG_CHECK_EQ(one_hour.value(), true);
  }
  const auto one_hour_exact =
      ecg::ExceedsBy(base, At("2026-01-01T01:00:00Z"), ecg::DurationSec::FromRaw(3600));
  ECG_CHECK_OK(one_hour_exact);
  if (one_hour_exact.ok()) {
    ECG_CHECK_EQ(one_hour_exact.value(), false);
  }

  // The largest representable difference is reported, not wrapped.
  ECG_CHECK_ERR(ecg::ExceedsBy(ecg::UtcInstant::FromRaw(kInt64Min),
                               ecg::UtcInstant::FromRaw(kInt64Max), ecg::DurationSec::FromRaw(0)),
                ecg::ErrorCode::kNumericOverflow);
}

ECG_TEST("time.elapsed_between_rejects_a_negative_span") {
  const ecg::UtcInstant earlier = At("2026-01-01T00:00:00Z");
  const ecg::UtcInstant later = At("2026-01-01T01:00:00Z");

  const auto forward = ecg::ElapsedBetween(earlier, later);
  ECG_CHECK_OK(forward);
  if (forward.ok()) {
    ECG_CHECK_EQ(forward.value().raw(), std::int64_t{3600});
  }
  const auto backwards = ecg::ElapsedBetween(later, earlier);
  ECG_CHECK_ERR(backwards, ecg::ErrorCode::kInvalidArgument);
  const auto same = ecg::ElapsedBetween(earlier, earlier);
  ECG_CHECK_OK(same);
  if (same.ok()) {
    ECG_CHECK_EQ(same.value().raw(), std::int64_t{0});
  }

  // Sub-second remainders truncate toward zero.
  const auto truncated = ecg::ElapsedBetween(earlier, ecg::UtcInstant::FromRaw(earlier.raw() + 1500000));
  ECG_CHECK_OK(truncated);
  if (truncated.ok()) {
    ECG_CHECK_EQ(truncated.value().raw(), std::int64_t{1});
  }

  // A difference that does not fit in 64 bits is reported, not wrapped.
  ECG_CHECK_ERR(
      ecg::ElapsedBetween(ecg::UtcInstant::FromRaw(kInt64Max), ecg::UtcInstant::FromRaw(kInt64Min)),
      ecg::ErrorCode::kNumericOverflow);
}

ECG_TEST("time.add_and_sub_seconds_report_overflow") {
  const ecg::UtcInstant epoch = ecg::UtcInstant::FromRaw(0);
  const auto plus = ecg::AddSeconds(epoch, ecg::DurationSec::FromRaw(3600));
  ECG_CHECK_OK(plus);
  if (plus.ok()) {
    ECG_CHECK_EQ(plus.value().raw(), std::int64_t{3600000000LL});
  }
  const auto minus = ecg::SubSeconds(epoch, ecg::DurationSec::FromRaw(3600));
  ECG_CHECK_OK(minus);
  if (minus.ok()) {
    ECG_CHECK_EQ(minus.value().raw(), std::int64_t{-3600000000LL});
  }
  const auto unchanged = ecg::AddSeconds(epoch, ecg::DurationSec::FromRaw(0));
  ECG_CHECK_OK(unchanged);
  if (unchanged.ok()) {
    ECG_CHECK_EQ(unchanged.value().raw(), std::int64_t{0});
  }

  ECG_CHECK_ERR(ecg::AddSeconds(ecg::UtcInstant::FromRaw(kInt64Max), ecg::DurationSec::FromRaw(1)),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(ecg::SubSeconds(ecg::UtcInstant::FromRaw(kInt64Min), ecg::DurationSec::FromRaw(1)),
                ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(
      ecg::AddSeconds(ecg::UtcInstant::FromRaw(kInt64Max), ecg::DurationSec::FromRaw(1000000000000LL)),
      ecg::ErrorCode::kNumericOverflow);
  ECG_CHECK_ERR(
      ecg::SubSeconds(ecg::UtcInstant::FromRaw(kInt64Min), ecg::DurationSec::FromRaw(1000000000000LL)),
      ecg::ErrorCode::kNumericOverflow);
}
