#include "ecg/time.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <optional>

namespace ecg {
namespace {

constexpr std::int64_t kMicrosPerSecond = 1000000;
constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kMicrosPerDay = kSecondsPerDay * kMicrosPerSecond;
constexpr std::int32_t kSecondsPerWeek = 7 * 86400;

/// Floor division and modulus, so negative instants (before 1970) decompose the
/// same way positive ones do.
[[nodiscard]] std::int64_t FloorDiv(std::int64_t numerator, std::int64_t denominator) noexcept {
  std::int64_t quotient = numerator / denominator;
  if ((numerator % denominator != 0) && ((numerator < 0) != (denominator < 0))) {
    --quotient;
  }
  return quotient;
}

[[nodiscard]] std::int64_t FloorMod(std::int64_t numerator, std::int64_t denominator) noexcept {
  return numerator - FloorDiv(numerator, denominator) * denominator;
}

[[nodiscard]] bool IsLeapYear(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] int DaysInMonth(int year, int month) noexcept {
  static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kDays[static_cast<std::size_t>(month - 1)];
}

/// ISO-8601 weekday, 1 = Monday .. 7 = Sunday, for a day number since epoch.
[[nodiscard]] int IsoWeekdayFromDays(std::int64_t days) noexcept {
  // 1970-01-01 was a Thursday (4).
  return static_cast<int>(FloorMod(days + 3, 7)) + 1;
}

[[nodiscard]] bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool ReadFixedDigits(std::string_view text, std::size_t offset, std::size_t count,
                                   int* out) noexcept {
  if (offset + count > text.size()) {
    return false;
  }
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (!IsDigit(c)) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

[[nodiscard]] std::int64_t CivilToLocalMicros(const CivilTime& civil) noexcept {
  const std::int64_t days = DaysFromCivil(civil.year, static_cast<unsigned>(civil.month),
                                          static_cast<unsigned>(civil.day));
  const std::int64_t seconds = days * kSecondsPerDay + civil.hour * 3600 + civil.minute * 60 +
                               civil.second;
  return seconds * kMicrosPerSecond + civil.microsecond;
}

void MicrosToCivil(std::int64_t micros, CivilTime* civil) noexcept {
  const std::int64_t days = FloorDiv(micros, kMicrosPerDay);
  std::int64_t remainder = FloorMod(micros, kMicrosPerDay);
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  civil->year = year;
  civil->month = static_cast<int>(month);
  civil->day = static_cast<int>(day);
  civil->hour = static_cast<int>(remainder / (3600 * kMicrosPerSecond));
  remainder %= 3600 * kMicrosPerSecond;
  civil->minute = static_cast<int>(remainder / (60 * kMicrosPerSecond));
  remainder %= 60 * kMicrosPerSecond;
  civil->second = static_cast<int>(remainder / kMicrosPerSecond);
  civil->microsecond = static_cast<int>(remainder % kMicrosPerSecond);
}

}  // namespace

bool operator==(const CivilTime& a, const CivilTime& b) noexcept {
  return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
         a.minute == b.minute && a.second == b.second && a.microsecond == b.microsecond;
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - static_cast<int>(era) * 400);
  const unsigned day_of_year =
      (153u * (month + (month > 2 ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u + day - 1u;
  const unsigned day_of_era =
      year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

void CivilFromDays(std::int64_t days, int* year, unsigned* month, unsigned* day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
  const std::int64_t y = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year = day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
  const unsigned month_prime = (5u * day_of_year + 2u) / 153u;
  const unsigned day_prime = day_of_year - (153u * month_prime + 2u) / 5u + 1u;
  const unsigned m = month_prime + (month_prime < 10u ? 3u : static_cast<unsigned>(-9));
  *year = static_cast<int>(y + (m <= 2u ? 1 : 0));
  *month = m;
  *day = day_prime;
}

bool IsValidCivilTime(const CivilTime& civil) noexcept {
  if (civil.year < 1 || civil.year > 9999) {
    return false;
  }
  if (civil.month < 1 || civil.month > 12) {
    return false;
  }
  if (civil.day < 1 || civil.day > DaysInMonth(civil.year, civil.month)) {
    return false;
  }
  if (civil.hour < 0 || civil.hour > 23) {
    return false;
  }
  if (civil.minute < 0 || civil.minute > 59) {
    return false;
  }
  if (civil.second < 0 || civil.second > 59) {
    return false;
  }
  if (civil.microsecond < 0 || civil.microsecond > 999999) {
    return false;
  }
  return true;
}

Result<UtcInstant> MakeUtcInstant(const CivilTime& civil, std::int32_t offset_seconds) {
  if (!IsValidCivilTime(civil)) {
    return MakeError(ErrorCode::kOutOfRange, "civil_time", "calendar fields are not a valid date or time");
  }
  if (offset_seconds < -ZoneRules::kMaxOffsetSeconds || offset_seconds > ZoneRules::kMaxOffsetSeconds) {
    return MakeError(ErrorCode::kOutOfRange, "utc_offset",
                     "offset " + std::to_string(offset_seconds) + "s outside +/-18h");
  }
  const std::int64_t days = DaysFromCivil(civil.year, static_cast<unsigned>(civil.month),
                                          static_cast<unsigned>(civil.day));
  std::int64_t micros = 0;
  if (TryMulInt64(days, kMicrosPerDay, &micros)) {
    return MakeError(ErrorCode::kNumericOverflow, "civil_time", "date is outside the representable range");
  }
  const std::int64_t time_of_day =
      (static_cast<std::int64_t>(civil.hour) * 3600 + civil.minute * 60 + civil.second) * kMicrosPerSecond +
      civil.microsecond;
  const std::int64_t offset_micros = static_cast<std::int64_t>(offset_seconds) * kMicrosPerSecond;
  std::int64_t shifted = 0;
  if (TryAddInt64(micros, time_of_day, &shifted) || TrySubInt64(shifted, offset_micros, &shifted)) {
    return MakeError(ErrorCode::kNumericOverflow, "civil_time", "instant is outside the representable range");
  }
  return QuantityFromRaw<UtcInstantTag>(shifted);
}

std::string FormatRfc3339(UtcInstant instant) {
  CivilTime civil;
  MicrosToCivil(instant.raw(), &civil);
  std::array<char, 40> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
                                    civil.year, civil.month, civil.day, civil.hour, civil.minute,
                                    civil.second, civil.microsecond);
  if (written <= 0) {
    return {};
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

Result<UtcInstant> ParseRfc3339(std::string_view text) {
  if (text.size() < 20) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "timestamp shorter than 20 characters");
  }
  if (text.size() > 40) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "timestamp longer than 40 characters");
  }
  CivilTime civil;
  if (!ReadFixedDigits(text, 0, 4, &civil.year) || text[4] != '-' ||
      !ReadFixedDigits(text, 5, 2, &civil.month) || text[7] != '-' ||
      !ReadFixedDigits(text, 8, 2, &civil.day)) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "expected YYYY-MM-DD");
  }
  if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "expected 'T' between date and time");
  }
  if (!ReadFixedDigits(text, 11, 2, &civil.hour) || text[13] != ':' ||
      !ReadFixedDigits(text, 14, 2, &civil.minute) || text[16] != ':' ||
      !ReadFixedDigits(text, 17, 2, &civil.second)) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "expected HH:MM:SS");
  }

  std::size_t index = 19;
  if (index < text.size() && text[index] == '.') {
    ++index;
    std::size_t fraction_digits = 0;
    int micros = 0;
    while (index < text.size() && IsDigit(text[index])) {
      if (fraction_digits >= 6) {
        return MakeError(ErrorCode::kOutOfRange, "rfc3339",
                         "fractional seconds beyond microsecond precision are rejected, not rounded");
      }
      micros = micros * 10 + (text[index] - '0');
      ++fraction_digits;
      ++index;
    }
    if (fraction_digits == 0) {
      return MakeError(ErrorCode::kMalformedInput, "rfc3339", "decimal point without fractional digits");
    }
    for (std::size_t i = fraction_digits; i < 6; ++i) {
      micros *= 10;
    }
    civil.microsecond = micros;
  }

  if (index >= text.size()) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "missing UTC offset designator");
  }
  std::int32_t offset_seconds = 0;
  const char designator = text[index];
  if (designator == 'Z' || designator == 'z') {
    ++index;
  } else if (designator == '+' || designator == '-') {
    const std::size_t offset_start = index + 1;
    int hours = 0;
    int minutes = 0;
    if (!ReadFixedDigits(text, offset_start, 2, &hours)) {
      return MakeError(ErrorCode::kMalformedInput, "rfc3339", "expected offset hours");
    }
    std::size_t cursor = offset_start + 2;
    if (cursor < text.size() && text[cursor] == ':') {
      ++cursor;
    }
    if (cursor < text.size()) {
      if (!ReadFixedDigits(text, cursor, 2, &minutes)) {
        return MakeError(ErrorCode::kMalformedInput, "rfc3339", "expected offset minutes");
      }
      cursor += 2;
    }
    if (hours > 18 || minutes > 59) {
      return MakeError(ErrorCode::kOutOfRange, "rfc3339", "UTC offset outside +/-18:00");
    }
    offset_seconds = static_cast<std::int32_t>(hours * 3600 + minutes * 60);
    if (designator == '-') {
      offset_seconds = -offset_seconds;
    }
    index = cursor;
  } else {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "missing UTC offset designator");
  }
  if (index != text.size()) {
    return MakeError(ErrorCode::kMalformedInput, "rfc3339", "trailing characters after the timestamp");
  }
  return MakeUtcInstant(civil, offset_seconds);
}

Result<UtcInstant> AddSeconds(UtcInstant instant, DurationSec seconds) {
  std::int64_t delta = 0;
  if (TryMulInt64(seconds.raw(), kMicrosPerSecond, &delta)) {
    return MakeError(ErrorCode::kNumericOverflow, "duration_s", "duration too large to apply to an instant");
  }
  std::int64_t sum = 0;
  if (TryAddInt64(instant.raw(), delta, &sum)) {
    return MakeError(ErrorCode::kNumericOverflow, "utc_instant_us", "instant arithmetic overflow");
  }
  return QuantityFromRaw<UtcInstantTag>(sum);
}

Result<UtcInstant> SubSeconds(UtcInstant instant, DurationSec seconds) {
  std::int64_t delta = 0;
  if (TryMulInt64(seconds.raw(), kMicrosPerSecond, &delta)) {
    return MakeError(ErrorCode::kNumericOverflow, "duration_s", "duration too large to apply to an instant");
  }
  std::int64_t difference = 0;
  if (TrySubInt64(instant.raw(), delta, &difference)) {
    return MakeError(ErrorCode::kNumericOverflow, "utc_instant_us", "instant arithmetic overflow");
  }
  return QuantityFromRaw<UtcInstantTag>(difference);
}

Result<bool> ExceedsBy(UtcInstant base, UtcInstant other, DurationSec limit) {
  std::int64_t delta = 0;
  if (TrySubInt64(other.raw(), base.raw(), &delta)) {
    return MakeError(ErrorCode::kNumericOverflow, "utc_instant_us", "instant difference overflow");
  }
  if (delta <= 0) {
    return false;
  }
  std::int64_t limit_micros = 0;
  if (TryMulInt64(limit.raw(), kMicrosPerSecond, &limit_micros)) {
    // A limit this large is never exceeded by any representable difference.
    return false;
  }
  return delta > limit_micros;
}

Result<DurationSec> ElapsedBetween(UtcInstant earlier, UtcInstant later) {
  std::int64_t micros = 0;
  if (TrySubInt64(later.raw(), earlier.raw(), &micros)) {
    return MakeError(ErrorCode::kNumericOverflow, "utc_instant_us", "instant difference overflow");
  }
  if (micros < 0) {
    return MakeError(ErrorCode::kInvalidArgument, "utc_instant_us",
                     "duration from " + FormatRfc3339(earlier) + " to " + FormatRfc3339(later) +
                         " is negative");
  }
  return QuantityFromRaw<DurationTag>(micros / kMicrosPerSecond);
}

Result<Interval> Interval::Make(UtcInstant begin, UtcInstant end) {
  if (!(begin < end)) {
    return MakeError(ErrorCode::kInvalidArgument, "interval",
                     "begin " + FormatRfc3339(begin) + " is not before end " + FormatRfc3339(end));
  }
  Interval interval;
  interval.begin_ = begin;
  interval.end_ = end;
  return interval;
}

DurationSec Interval::duration() const noexcept {
  return DurationSec::FromRaw((end_.raw() - begin_.raw()) / kMicrosPerSecond);
}

bool Interval::contains(UtcInstant instant) const noexcept {
  return !(instant < begin_) && instant < end_;
}

bool Interval::contains(const Interval& other) const noexcept {
  return begin_ <= other.begin_ && other.end_ <= end_;
}

bool Interval::overlaps(const Interval& other) const noexcept {
  return begin_ < other.end_ && other.begin_ < end_;
}

Result<std::optional<Interval>> Interval::Intersect(const Interval& other) const {
  if (!overlaps(other)) {
    return std::optional<Interval>{};
  }
  const UtcInstant begin = begin_ < other.begin_ ? other.begin_ : begin_;
  const UtcInstant end = end_ < other.end_ ? end_ : other.end_;
  if (!(begin < end)) {
    return std::optional<Interval>{};
  }
  return std::optional<Interval>{Interval::Make(begin, end).value()};
}

std::vector<Interval> CoalesceIntervals(std::vector<Interval> intervals) {
  if (intervals.size() < 2) {
    return intervals;
  }
  std::sort(intervals.begin(), intervals.end());
  std::vector<Interval> merged;
  merged.reserve(intervals.size());
  merged.push_back(intervals.front());
  for (std::size_t i = 1; i < intervals.size(); ++i) {
    if (intervals[i].begin() <= merged.back().end()) {
      if (merged.back().end() < intervals[i].end()) {
        merged.back() = Interval::Make(merged.back().begin(), intervals[i].end()).value();
      }
    } else {
      merged.push_back(intervals[i]);
    }
  }
  return merged;
}

std::vector<Interval> SubtractIntervals(const std::vector<Interval>& base,
                                        const std::vector<Interval>& holes) {
  std::vector<Interval> result = CoalesceIntervals(base);
  const std::vector<Interval> coalescedHoles = CoalesceIntervals(holes);
  for (const Interval& hole : coalescedHoles) {
    std::vector<Interval> next;
    next.reserve(result.size() + 1);
    for (const Interval& piece : result) {
      if (!piece.overlaps(hole)) {
        next.push_back(piece);
        continue;
      }
      if (piece.begin() < hole.begin()) {
        next.push_back(Interval::Make(piece.begin(), hole.begin()).value());
      }
      if (hole.end() < piece.end()) {
        next.push_back(Interval::Make(hole.end(), piece.end()).value());
      }
    }
    result = std::move(next);
  }
  return CoalesceIntervals(std::move(result));
}

std::optional<std::pair<Interval, Interval>> FindFirstOverlap(const std::vector<Interval>& intervals) {
  if (intervals.size() < 2) {
    return std::nullopt;
  }
  std::vector<Interval> sorted = intervals;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i - 1].overlaps(sorted[i])) {
      return std::make_pair(sorted[i - 1], sorted[i]);
    }
  }
  return std::nullopt;
}

bool IntervalsAreDisjoint(const std::vector<Interval>& intervals) {
  return !FindFirstOverlap(intervals).has_value();
}

std::vector<Interval> ClipIntervals(const std::vector<Interval>& intervals, const Interval& horizon) {
  std::vector<Interval> clipped;
  clipped.reserve(intervals.size());
  for (const Interval& interval : intervals) {
    const auto intersection = interval.Intersect(horizon);
    if (intersection.ok() && intersection.value().has_value()) {
      clipped.push_back(*intersection.value());
    }
  }
  return clipped;
}

Result<ZoneRules> ZoneRules::Make(ZoneId id, std::int32_t initial_offset_seconds,
                                  std::vector<ZoneTransition> transitions) {
  if (initial_offset_seconds < -kMaxOffsetSeconds || initial_offset_seconds > kMaxOffsetSeconds) {
    return MakeError(ErrorCode::kOutOfRange, "zone_rules", "initial offset outside +/-18h");
  }
  if (transitions.size() > kMaxTransitions) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "zone_rules",
                     "more than " + std::to_string(kMaxTransitions) + " offset transitions");
  }
  for (std::size_t i = 0; i < transitions.size(); ++i) {
    if (transitions[i].offset_seconds < -kMaxOffsetSeconds ||
        transitions[i].offset_seconds > kMaxOffsetSeconds) {
      return MakeError(ErrorCode::kOutOfRange, "zone_rules", "transition offset outside +/-18h");
    }
    if (i > 0 && !(transitions[i - 1].at < transitions[i].at)) {
      return MakeError(ErrorCode::kInvalidArgument, "zone_rules",
                       "offset transitions must be strictly increasing in time");
    }
  }
  ZoneRules rules;
  rules.id_ = std::move(id);
  rules.initial_offset_ = initial_offset_seconds;
  rules.transitions_ = std::move(transitions);
  return rules;
}

std::int32_t ZoneRules::OffsetAt(UtcInstant instant) const noexcept {
  std::int32_t offset = initial_offset_;
  // Linear scan is intentional: transition tables are tiny (tens of entries for
  // decades of rules) and a scan has no branch-prediction or bounds surprises.
  for (const ZoneTransition& transition : transitions_) {
    if (transition.at <= instant) {
      offset = transition.offset_seconds;
    } else {
      break;
    }
  }
  return offset;
}

Result<UtcInstant> ZoneRules::ResolveLocal(const CivilTime& local) const {
  if (!IsValidCivilTime(local)) {
    return MakeError(ErrorCode::kOutOfRange, "civil_time", "calendar fields are not a valid date or time");
  }
  const std::int64_t local_micros = CivilToLocalMicros(local);
  std::optional<std::int64_t> best;
  // Candidate offsets: the initial offset plus every offset this table ever
  // adopts. For each candidate, the mapping is valid only when the resulting
  // instant actually observes that offset.
  std::vector<std::int32_t> candidates;
  candidates.reserve(transitions_.size() + 1);
  candidates.push_back(initial_offset_);
  for (const ZoneTransition& transition : transitions_) {
    if (std::find(candidates.begin(), candidates.end(), transition.offset_seconds) == candidates.end()) {
      candidates.push_back(transition.offset_seconds);
    }
  }
  for (const std::int32_t candidate : candidates) {
    const std::int64_t utc_micros = local_micros - static_cast<std::int64_t>(candidate) * kMicrosPerSecond;
    const UtcInstant instant = UtcInstant::FromRaw(utc_micros);
    if (OffsetAt(instant) == candidate) {
      if (!best.has_value() || utc_micros < *best) {
        best = utc_micros;
      }
    }
  }
  if (!best.has_value()) {
    return MakeError(ErrorCode::kNotFound, "local_time",
                     "local time does not exist in this zone (daylight-saving gap)");
  }
  return UtcInstant::FromRaw(*best);
}

CivilTime ZoneRules::ToLocal(UtcInstant instant) const noexcept {
  const std::int32_t offset = OffsetAt(instant);
  CivilTime civil;
  MicrosToCivil(instant.raw() + static_cast<std::int64_t>(offset) * kMicrosPerSecond, &civil);
  return civil;
}

namespace {

/// Resolves a recurring window's local start time. When the local time falls in
/// a daylight-saving gap, the windows starts at the instant the new offset takes
/// effect; when it occurs twice, the first occurrence wins.
[[nodiscard]] UtcInstant ResolveWindowStart(const ZoneRules& zone, const CivilTime& local) {
  const auto strict = zone.ResolveLocal(local);
  if (strict.ok()) {
    return strict.value();
  }
  const std::int64_t local_micros = CivilToLocalMicros(local);
  const std::int64_t naive = local_micros - static_cast<std::int64_t>(zone.initial_offset_seconds()) *
                                                kMicrosPerSecond;
  for (const ZoneTransition& transition : zone.transitions()) {
    const std::int64_t delta = transition.at.raw() - naive;
    if (delta < -2 * kMicrosPerDay || delta > 2 * kMicrosPerDay) {
      continue;
    }
    const std::int32_t previous = zone.OffsetAt(UtcInstant::FromRaw(transition.at.raw() - 1));
    if (transition.offset_seconds <= previous) {
      continue;  // Not a forward gap.
    }
    const std::int64_t gap_begin_local =
        transition.at.raw() + static_cast<std::int64_t>(previous) * kMicrosPerSecond;
    const std::int64_t gap_end_local =
        transition.at.raw() + static_cast<std::int64_t>(transition.offset_seconds) * kMicrosPerSecond;
    if (local_micros >= gap_begin_local && local_micros < gap_end_local) {
      return transition.at;
    }
  }
  return UtcInstant::FromRaw(naive);
}

}  // namespace

Result<std::vector<Interval>> ExpandRecurring(const RecurringWindow& window, const ZoneRules& zone,
                                              const Interval& horizon) {
  if (window.duration_seconds <= 0) {
    return MakeError(ErrorCode::kInvalidArgument, "recurring_window", "duration must be positive");
  }
  if (window.start_second_of_day < 0 || window.start_second_of_day >= kSecondsPerDay) {
    return MakeError(ErrorCode::kOutOfRange, "recurring_window",
                     "start second of day must be in [0, 86399]");
  }
  if ((window.weekday_mask & 0x80u) != 0u) {
    return MakeError(ErrorCode::kInvalidArgument, "recurring_window", "weekday mask has a reserved bit set");
  }
  if (window.weekday_mask == 0u) {
    return std::vector<Interval>{};
  }

  const CivilTime local_begin = zone.ToLocal(horizon.begin());
  const CivilTime local_end = zone.ToLocal(horizon.end());
  const std::int64_t first_day =
      DaysFromCivil(local_begin.year, static_cast<unsigned>(local_begin.month),
                    static_cast<unsigned>(local_begin.day)) - 1;
  const std::int64_t last_day =
      DaysFromCivil(local_end.year, static_cast<unsigned>(local_end.month),
                    static_cast<unsigned>(local_end.day)) + 1;
  const std::int64_t day_count = last_day - first_day + 1;
  if (day_count <= 0 || static_cast<std::uint64_t>(day_count) > kMaxExpandedWindows) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "recurring_window",
                     "horizon spans " + std::to_string(day_count) + " days, above the expansion bound");
  }

  std::vector<Interval> occurrences;
  for (std::int64_t day = first_day; day <= last_day; ++day) {
    if (!window.IncludesWeekday(IsoWeekdayFromDays(day))) {
      continue;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day_of_month = 0;
    CivilFromDays(day, &year, &month, &day_of_month);
    CivilTime local;
    local.year = year;
    local.month = static_cast<int>(month);
    local.day = static_cast<int>(day_of_month);
    local.hour = window.start_second_of_day / 3600;
    local.minute = (window.start_second_of_day % 3600) / 60;
    local.second = window.start_second_of_day % 60;

    const UtcInstant start = ResolveWindowStart(zone, local);
    const auto finish = AddSeconds(start, DurationSec::FromRaw(window.duration_seconds));
    if (!finish.ok()) {
      return finish.error();
    }
    const auto occurrence = Interval::Make(start, finish.value());
    if (!occurrence.ok()) {
      return occurrence.error();
    }
    const auto clipped = occurrence.value().Intersect(horizon);
    if (!clipped.ok()) {
      return clipped.error();
    }
    if (clipped.value().has_value()) {
      occurrences.push_back(*clipped.value());
      if (occurrences.size() > kMaxExpandedWindows) {
        return MakeError(ErrorCode::kResourceLimitExceeded, "recurring_window",
                         "expansion produced more than " + std::to_string(kMaxExpandedWindows) +
                             " occurrences");
      }
    }
  }
  return CoalesceIntervals(std::move(occurrences));
}

Result<DurationSec> ParseIsoDurationSeconds(std::string_view text) {
  if (text.empty() || text.size() > 32) {
    return MakeError(ErrorCode::kMalformedInput, "iso_duration", "duration must be 1..32 characters");
  }
  std::size_t index = 0;
  if (text[index] != 'P' && text[index] != 'p') {
    return MakeError(ErrorCode::kMalformedInput, "iso_duration", "duration must start with 'P'");
  }
  ++index;
  bool in_time = false;
  bool any = false;
  std::int64_t total_seconds = 0;
  while (index < text.size()) {
    if (text[index] == 'T' || text[index] == 't') {
      if (in_time) {
        return MakeError(ErrorCode::kMalformedInput, "iso_duration", "duplicate time designator");
      }
      in_time = true;
      ++index;
      continue;
    }
    std::size_t digits = 0;
    std::int64_t value = 0;
    while (index < text.size() && IsDigit(text[index])) {
      if (digits >= 9) {
        return MakeError(ErrorCode::kOutOfRange, "iso_duration", "component longer than 9 digits");
      }
      value = value * 10 + (text[index] - '0');
      ++digits;
      ++index;
    }
    if (digits == 0 || index >= text.size()) {
      return MakeError(ErrorCode::kMalformedInput, "iso_duration", "expected digits followed by a designator");
    }
    const char designator = text[index];
    ++index;
    std::int64_t multiplier = 0;
    switch (designator) {
      case 'W': case 'w': multiplier = kSecondsPerWeek; break;
      case 'D': case 'd': multiplier = kSecondsPerDay; break;
      case 'H': case 'h': multiplier = 3600; break;
      case 'M': case 'm': multiplier = in_time ? 60 : 0; break;
      case 'S': case 's': multiplier = in_time ? 1 : 0; break;
      default:
        return MakeError(ErrorCode::kMalformedInput, "iso_duration",
                         std::string("unsupported duration designator '") + designator + "'");
    }
    if (multiplier == 0) {
      return MakeError(ErrorCode::kMalformedInput, "iso_duration",
                       "month/year components are not supported; use weeks, days, hours, minutes, seconds");
    }
    std::int64_t component = 0;
    if (TryMulInt64(value, multiplier, &component)) {
      return MakeError(ErrorCode::kNumericOverflow, "iso_duration", "component overflow");
    }
    if (TryAddInt64(total_seconds, component, &total_seconds)) {
      return MakeError(ErrorCode::kNumericOverflow, "iso_duration", "duration overflow");
    }
    any = true;
  }
  if (!any) {
    return MakeError(ErrorCode::kMalformedInput, "iso_duration", "duration has no components");
  }
  return QuantityFromRaw<DurationTag>(total_seconds);
}

std::string FormatIsoDurationSeconds(DurationSec duration) {
  std::int64_t remaining = duration.raw();
  const std::int64_t days = remaining / kSecondsPerDay;
  remaining %= kSecondsPerDay;
  const std::int64_t hours = remaining / 3600;
  remaining %= 3600;
  const std::int64_t minutes = remaining / 60;
  const std::int64_t seconds = remaining % 60;
  std::string out = "P";
  if (days > 0) {
    out += std::to_string(days);
    out += "D";
  }
  if (hours > 0 || minutes > 0 || seconds > 0 || days == 0) {
    out += "T";
    if (hours > 0) {
      out += std::to_string(hours);
      out += "H";
    }
    if (minutes > 0) {
      out += std::to_string(minutes);
      out += "M";
    }
    if (seconds > 0 || (hours == 0 && minutes == 0)) {
      out += std::to_string(seconds);
      out += "S";
    }
  }
  return out;
}

}  // namespace ecg
