#ifndef ECG_POLICY_HPP
#define ECG_POLICY_HPP

// The policy this runtime owns: thresholds, bounds, and rule enablement.
//
// A policy is *this* runtime's own authority. Everything else in the decision
// input is somebody else's evidence. The split matters: a tariff publishes a
// demand threshold, but only policy decides how much safety margin to keep above
// a reserve floor, how large an intent may be, and whether an economically
// unfavourable action may proceed at all.

#include <cstddef>
#include <cstdint>

#include "ecg/evidence.hpp"
#include "ecg/quantity.hpp"
#include "ecg/result.hpp"
#include "ecg/strong.hpp"

namespace ecg {

/// Stable identifiers for the ordered rule pipeline. The order is part of the
/// contract: reason traces are emitted in this order, so two runs over the same
/// inputs produce identical traces.
enum class RuleId : std::uint16_t {
  kRequestContract = 1,
  kPolicyGeneration = 2,
  kEpochConsistency = 3,
  kEvidencePresence = 4,
  kEvidenceFreshness = 5,
  kEvidenceValidity = 6,
  kWindowCoverage = 7,
  kServiceTerms = 8,
  kReserveFloor = 9,
  kCapacity = 10,
  kRisk = 11,
  kIncident = 12,
  kDemandCharge = 13,
  kEconomics = 14,
  kIntentBounding = 15,
  kOutcome = 16,
};

[[nodiscard]] const char* RuleIdName(RuleId rule) noexcept;

/// Everything policy governs.
struct PolicySet {
  PolicyId id;
  PolicyGeneration generation;
  /// Authority epoch this policy revision was published in.
  Epoch epoch;
  EvidenceGeneration evidence_generation;

  FreshnessLimits limits;

  /// How far ahead of "now" an action may be authorised.
  DurationSec decision_horizon;
  /// How early before a window this runtime is willing to authorise it.
  DurationSec reaction_lead_time;
  /// How far beyond a source window the engine may look for a cheaper window.
  DurationSec shift_search_horizon;
  /// Re-evaluation delay attached to deferrals caused by an active incident.
  DurationSec incident_recheck_interval;

  /// Bound on any intent this runtime will emit.
  PowerKw max_intent_magnitude_kw;
  DurationSec max_intent_duration;

  /// Extra headroom kept above the reserve floor.
  RatioPpm reserve_safety_margin;

  /// Economic desirability threshold, kept separate from authorisation.
  MoneyMicros minimum_economic_benefit;
  /// When true, an action whose projected savings fall below the threshold is
  /// refused on policy grounds even though nothing operational forbids it.
  bool require_positive_economic_benefit{true};
  /// When true, an action that would raise a metered peak above a demand-charge
  /// threshold is refused.
  bool prohibit_demand_charge_increase{true};
  /// When true, a request whose stated rationale is demand-charge avoidance is
  /// deferred until the reaction lead time before its window, because a peak
  /// projection made hours early is not evidence of a peak. A rationale can only
  /// ever narrow authorisation, never widen it.
  bool defer_early_demand_requests{true};
  /// Policy ceiling on curtailment, applied in addition to the service terms.
  RatioPpm max_service_curtailment_ratio;

  /// Bounds, so a hostile or mistaken input cannot produce an unbounded trace.
  std::size_t max_reasons{64};
  std::size_t max_price_slices{4096};

  [[nodiscard]] DurationSec MaxAgeFor(EvidenceCategory category) const noexcept {
    return limits.MaxAgeFor(category);
  }
};

/// Validates a policy set: bounds, non-negative durations, ratio ranges, and the
/// presence of an identity and generation.
[[nodiscard]] Status ValidatePolicy(const PolicySet& policy);

/// A documented, conservative default policy. Intended for the CLI, examples,
/// and tests; a production deployment supplies its own revision.
[[nodiscard]] PolicySet DefaultPolicy();

}  // namespace ecg

#endif  // ECG_POLICY_HPP
