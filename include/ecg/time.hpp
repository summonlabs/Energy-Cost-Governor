#ifndef ECG_TIME_HPP
#define ECG_TIME_HPP

// Deterministic time semantics.
//
// Energy prices, demand windows, and reserve obligations are all defined over
// time, and getting time wrong is the classic source of silent energy-cost
// error: a demand window that shifts by an hour across a daylight-saving
// transition charges for a peak that never happened.
//
// Rules enforced by this header:
//   * Every instant is UTC microseconds since the Unix epoch. There is no local
//     time anywhere in the decision arithmetic.
//   * Local (wall clock) definitions are resolved to UTC only through an
//     explicit ZoneRules value supplied as evidence. The host operating system
//     time zone database is never consulted, so results cannot change with the
//     machine that runs them.
//   * Intervals are half-open [begin, end). A zero-length interval is invalid
//     rather than empty-but-accepted, so "no window" and "empty window" can
//     never be confused.
//   * Nonexistent local times (spring-forward gaps) and ambiguous local times
//     (fall-back overlaps) are resolved by documented, deterministic rules.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/result.hpp"
#include "ecg/strong.hpp"

namespace ecg {

/// Calendar fields in a stated UTC offset. Used only at the boundary between
/// wall-clock definitions and UTC instants.
struct CivilTime {
  int year{1970};
  int month{1};
  int day{1};
  int hour{0};
  int minute{0};
  int second{0};
  int microsecond{0};

  friend bool operator==(const CivilTime& a, const CivilTime& b) noexcept;
};

/// Days since 1970-01-01 for a proleptic Gregorian date. Valid for the full
/// range of the algorithm; out-of-range field values yield a defined value and
/// callers validate fields first.
[[nodiscard]] std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept;

/// Inverse of DaysFromCivil.
void CivilFromDays(std::int64_t days, int* year, unsigned* month, unsigned* day) noexcept;

/// True when the fields form a real proleptic Gregorian date and a real time of
/// day. Leap seconds (second == 60) are rejected: this runtime never operates on
/// a clock that can produce one.
[[nodiscard]] bool IsValidCivilTime(const CivilTime& civil) noexcept;

/// Builds a UTC instant from calendar fields interpreted at offset_seconds east
/// of UTC.
[[nodiscard]] Result<UtcInstant> MakeUtcInstant(const CivilTime& civil, std::int32_t offset_seconds);

/// Formats an instant as RFC 3339 in UTC with fixed microsecond precision, for
/// example "2026-03-01T04:30:00.000000Z". Fixed width and fixed precision keep
/// persisted and CLI output byte-stable.
[[nodiscard]] std::string FormatRfc3339(UtcInstant instant);

/// Parses the RFC 3339 subset this runtime emits and accepts from operators:
/// "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM|+HHMM|-HHMM|+HH|-HH)". A
/// space may separate date and time. Fractional digits beyond microseconds are
/// rejected rather than rounded.
[[nodiscard]] Result<UtcInstant> ParseRfc3339(std::string_view text);

/// Adds a whole-second duration to an instant, rejecting overflow.
[[nodiscard]] Result<UtcInstant> AddSeconds(UtcInstant instant, DurationSec seconds);

/// Subtracts a whole-second duration from an instant, rejecting overflow.
[[nodiscard]] Result<UtcInstant> SubSeconds(UtcInstant instant, DurationSec seconds);

/// True when other is later than base by strictly more than limit, evaluated at
/// full microsecond precision. A sub-second overrun is still an overrun, which
/// matters because evidence age limits are compared, not merely reported.
[[nodiscard]] Result<bool> ExceedsBy(UtcInstant base, UtcInstant other, DurationSec limit);

/// Duration from earlier to later. Fails when later precedes earlier, because a
/// negative duration is never a meaningful domain value here.
[[nodiscard]] Result<DurationSec> ElapsedBetween(UtcInstant earlier, UtcInstant later);

/// A half-open interval [begin, end) with begin < end.
class Interval {
 public:
  Interval() = default;

  /// Fails when end <= begin. There is no "empty interval" state: absence is
  /// represented by std::optional or by an empty vector, never by a degenerate
  /// interval that silently matches nothing.
  [[nodiscard]] static Result<Interval> Make(UtcInstant begin, UtcInstant end);

  [[nodiscard]] UtcInstant begin() const noexcept { return begin_; }
  [[nodiscard]] UtcInstant end() const noexcept { return end_; }

  /// Whole seconds spanned, truncated toward zero. Bounds are microsecond
  /// precise, so sub-second remainders are discarded by documented design.
  [[nodiscard]] DurationSec duration() const noexcept;

  /// True when begin <= instant < end.
  [[nodiscard]] bool contains(UtcInstant instant) const noexcept;
  /// True when other is entirely inside this interval.
  [[nodiscard]] bool contains(const Interval& other) const noexcept;
  /// True when the two half-open intervals share at least one instant.
  [[nodiscard]] bool overlaps(const Interval& other) const noexcept;
  /// Intersection, or nothing when the intervals are disjoint.
  [[nodiscard]] Result<std::optional<Interval>> Intersect(const Interval& other) const;

  friend bool operator==(const Interval& a, const Interval& b) noexcept {
    return a.begin_ == b.begin_ && a.end_ == b.end_;
  }
  friend bool operator<(const Interval& a, const Interval& b) noexcept {
    if (a.begin_ != b.begin_) {
      return a.begin_ < b.begin_;
    }
    return a.end_ < b.end_;
  }

 private:
  UtcInstant begin_;
  UtcInstant end_;
};

/// Sorts by begin and merges overlapping or touching intervals.
[[nodiscard]] std::vector<Interval> CoalesceIntervals(std::vector<Interval> intervals);

/// Returns the parts of the base intervals not covered by the holes, preserving
/// order and coalescing the result.
[[nodiscard]] std::vector<Interval> SubtractIntervals(const std::vector<Interval>& base,
                                                      const std::vector<Interval>& holes);

/// True when no two intervals in the list overlap. Used to validate tariff price
/// schedules, where an overlap would make the price ambiguous.
[[nodiscard]] bool IntervalsAreDisjoint(const std::vector<Interval>& intervals);

/// The first overlapping pair, if any. Returned instead of a bare bool so error
/// messages can name the exact conflict.
[[nodiscard]] std::optional<std::pair<Interval, Interval>> FindFirstOverlap(
    const std::vector<Interval>& intervals);

/// Clips every interval to the horizon and drops the ones that fall outside.
[[nodiscard]] std::vector<Interval> ClipIntervals(const std::vector<Interval>& intervals,
                                                  const Interval& horizon);

/// A single UTC offset transition published by the time-zone authority.
struct ZoneTransition {
  UtcInstant at;
  std::int32_t offset_seconds{0};

  friend bool operator==(const ZoneTransition& a, const ZoneTransition& b) noexcept {
    return a.at == b.at && a.offset_seconds == b.offset_seconds;
  }
};

/// An explicit, evidence-supplied UTC offset table for one zone.
///
/// This runtime deliberately does not read the host time zone database: the
/// rules that define a tariff window are part of the tariff's own evidence, and
/// a machine-local database would make decisions depend on the host.
class ZoneRules {
 public:
  static constexpr std::size_t kMaxTransitions = 4096;
  static constexpr std::int32_t kMaxOffsetSeconds = 18 * 3600;

  /// Validates and constructs. Transitions must be strictly increasing in time
  /// and offsets must be within +/- 18 hours of UTC.
  [[nodiscard]] static Result<ZoneRules> Make(ZoneId id, std::int32_t initial_offset_seconds,
                                              std::vector<ZoneTransition> transitions);

  [[nodiscard]] const ZoneId& id() const noexcept { return id_; }
  [[nodiscard]] std::int32_t initial_offset_seconds() const noexcept { return initial_offset_; }
  [[nodiscard]] const std::vector<ZoneTransition>& transitions() const noexcept { return transitions_; }

  /// Offset in effect at an instant.
  [[nodiscard]] std::int32_t OffsetAt(UtcInstant instant) const noexcept;

  /// Resolves wall-clock fields to a UTC instant. Fails when the local time does
  /// not exist (a spring-forward gap). When the local time occurs twice (a
  /// fall-back overlap) the earlier offset wins, deterministically.
  [[nodiscard]] Result<UtcInstant> ResolveLocal(const CivilTime& local) const;

  /// Wall-clock fields for an instant in this zone.
  [[nodiscard]] CivilTime ToLocal(UtcInstant instant) const noexcept;

 private:
  ZoneId id_;
  std::int32_t initial_offset_{0};
  std::vector<ZoneTransition> transitions_;
};

/// A wall-clock recurrence: "these weekdays, this local time of day, for this
/// long". This is how demand-charge windows are expressed by real tariffs.
struct RecurringWindow {
  /// Bit 0 is Monday through bit 6 Sunday; bit 7 must be zero.
  std::uint8_t weekday_mask{0};
  /// Local seconds after midnight, 0..86399.
  std::int32_t start_second_of_day{0};
  /// Positive length in seconds.
  std::int32_t duration_seconds{0};

  [[nodiscard]] bool IncludesWeekday(int iso_weekday) const noexcept {
    return (weekday_mask & static_cast<std::uint8_t>(1u << (iso_weekday - 1))) != 0u;
  }
};

/// Upper bound on the number of occurrences ExpandRecurring will produce. A
/// pathological window (one-second duration across a decade) fails with
/// kResourceLimitExceeded instead of allocating without bound.
inline constexpr std::size_t kMaxExpandedWindows = 100000;

/// Expands a recurring window into concrete UTC intervals clipped to a horizon.
///
/// Deterministic resolution rules:
///   * A local start that does not exist (spring-forward gap) starts at the
///     instant the new offset takes effect, so the window is never silently
///     dropped and never starts before the clock agrees.
///   * A local start that occurs twice starts at its first occurrence.
[[nodiscard]] Result<std::vector<Interval>> ExpandRecurring(const RecurringWindow& window,
                                                            const ZoneRules& zone,
                                                            const Interval& horizon);

/// Parses an RFC 3339 duration of whole seconds, for example "PT15M" or "PT1H30M".
[[nodiscard]] Result<DurationSec> ParseIsoDurationSeconds(std::string_view text);

/// Formats whole seconds as an ISO 8601 duration, for example "PT15M".
[[nodiscard]] std::string FormatIsoDurationSeconds(DurationSec duration);

}  // namespace ecg

#endif  // ECG_TIME_HPP
