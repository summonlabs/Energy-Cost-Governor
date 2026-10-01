#ifndef ECG_DECISION_HPP
#define ECG_DECISION_HPP

// Decisions: the only thing this runtime produces.
//
// A decision is one of four outcomes plus an ordered reason trace that shows
// exactly which evidence, generations, and policy produced it:
//
//   Allowed       -- every required condition held on fresh evidence. A bounded
//                    intent is emitted. This is an authorisation, not an effect:
//                    nothing has changed in the facility.
//   Refused       -- fresh evidence shows a hard constraint is violated. The
//                    answer is a definitive no, and it does not depend on any
//                    missing input.
//   Deferred      -- nothing forbids it, but now is not the moment. Always
//                    carries the instant at which the decision is worth asking
//                    again.
//   Indeterminate -- a required input is missing, stale, recovered, conflicting,
//                    or from a different epoch. The answer is "cannot decide",
//                    which is never a permissive answer.
//
// Outcome precedence is total and documented: Refused > Indeterminate >
// Deferred > Allowed. Refusal wins over indeterminacy because a definitive
// violation does not become less definitive when some unrelated input is stale;
// indeterminacy wins over deferral because "not now" is a weaker claim than
// "cannot say".
//
// Economic desirability never leaks into authorisation implicitly. The
// assessment is reported separately, and only an explicit policy switch turns an
// unfavourable assessment into a refusal.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/evidence.hpp"
#include "ecg/hash.hpp"
#include "ecg/quantity.hpp"
#include "ecg/request.hpp"
#include "ecg/strong.hpp"
#include "ecg/time.hpp"

namespace ecg {

enum class Outcome : std::uint8_t {
  kAllowed = 0,
  kRefused = 1,
  kDeferred = 2,
  kIndeterminate = 3,
};

[[nodiscard]] const char* OutcomeName(Outcome outcome) noexcept;

/// What a reason does to the outcome.
enum class ReasonClass : std::uint8_t {
  kSupporting = 0,
  kDeferred = 1,
  kIndeterminate = 2,
  kRefused = 3,
};

/// How loudly a reason speaks. Derived from the class; carried explicitly so a
/// consumer does not have to re-derive policy.
enum class ReasonSeverity : std::uint8_t {
  kInfo = 0,
  kNotice = 1,
  kBlocker = 2,
};

[[nodiscard]] const char* ReasonSeverityName(ReasonSeverity severity) noexcept;

/// Stable reason codes. Values are part of the public contract and are persisted
/// in the journal, so they must never be renumbered.
enum class ReasonCode : std::uint16_t {
  kNone = 0,

  // Supporting.
  kAuthorized = 1,
  kIntentBounded = 2,
  kEconomicsFavorable = 3,
  kDemandChargeAvoided = 4,
  kPriceCoverageVerified = 5,
  kShiftTargetSelected = 6,
  kServiceTermsSatisfied = 7,
  kReserveHeadroomSufficient = 8,
  kIncidentClear = 9,
  kPolicyGenerationCurrent = 10,

  // Indeterminate.
  kEvidenceMissing = 100,
  kEvidenceStale = 101,
  kEvidenceFutureDated = 102,
  kEvidenceRecovered = 103,
  kEvidenceEpochMismatch = 104,
  kEvidenceUnsupportedVersion = 105,
  kEvidenceConflicting = 106,
  kEvidenceInvalid = 107,
  kPolicyGenerationStale = 108,
  kRequestEpochMismatch = 109,
  kPriceCoverageGap = 110,
  kPriceCoverageAmbiguous = 111,
  kZoneRulesMissing = 112,
  kEconomicsOverflow = 113,
  kResourceLimitExceeded = 114,
  kTariffValidityMiss = 115,
  kStorageEfficiencyMissing = 116,
  kDemandWindowUnresolved = 117,

  // Refused.
  kReserveFloorBreach = 200,
  kServiceClassUnknown = 201,
  kServiceCurtailmentProhibited = 202,
  kServiceNoticeInsufficient = 203,
  kServiceDurationExceeded = 204,
  kServiceRatioExceeded = 205,
  kCapacityInsufficient = 206,
  kRiskPostureRestricted = 207,
  kRampLimitExceeded = 208,
  kDwellNotElapsed = 209,
  kIncidentCritical = 210,
  kWindowInPast = 211,
  kIntentDurationExceedsPolicy = 212,
  kDemandChargeIncrease = 213,
  kEconomicallyUnfavorable = 214,
  kStorageEnergyInsufficient = 215,
  kStoragePowerInsufficient = 216,
  kDeferralLimitExceeded = 217,
  kMagnitudeExceedsPolicy = 218,

  // Deferred.
  kIncidentActive = 300,
  kWindowBeyondHorizon = 301,
  kDemandThresholdNotProjected = 302,
  kShiftTargetNotYetAvailable = 303,
};

[[nodiscard]] const char* ReasonCodeName(ReasonCode code) noexcept;
[[nodiscard]] ReasonClass ClassifyReasonCode(ReasonCode code) noexcept;
[[nodiscard]] ReasonSeverity DefaultSeverityFor(ReasonCode code) noexcept;

/// Which evidence a reason depends on. Carried so an operator can see exactly
/// which observation and which generation produced an outcome.
struct EvidenceRef {
  AuthorityKind authority{AuthorityKind::kSyntheticModel};
  SourceId source;
  EvidenceGeneration generation;
  UtcInstant observed_at;
  Freshness freshness{Freshness::kMissing};

  friend bool operator==(const EvidenceRef& a, const EvidenceRef& b) noexcept {
    return a.authority == b.authority && a.source == b.source && a.generation == b.generation &&
           a.observed_at == b.observed_at;
  }
};

/// Builds a reference from an evidence envelope.
template <class Payload>
[[nodiscard]] EvidenceRef MakeEvidenceRef(const Evidence<Payload>& evidence, Freshness freshness) {
  EvidenceRef ref;
  ref.authority = evidence.meta.authority;
  ref.source = evidence.meta.source;
  ref.generation = evidence.meta.generation;
  ref.observed_at = evidence.meta.observed_at;
  ref.freshness = freshness;
  return ref;
}

/// A reference for evidence that is absent. The generation is unset, which is
/// how "we never saw this" is distinguished from "we saw generation 0".
[[nodiscard]] EvidenceRef MissingEvidenceRef(AuthorityKind authority, const SourceId& source);

struct Reason {
  ReasonCode code{ReasonCode::kNone};
  ReasonSeverity severity{ReasonSeverity::kInfo};
  /// What the reason is about, for example "service_class=vm.batch" or
  /// "reserve". Stable and machine-readable.
  std::string subject;
  /// Human-readable explanation. Never contains pointers, addresses, or
  /// timestamps that would break replay determinism.
  std::string detail;
  std::optional<EvidenceRef> evidence;
};

/// Canonical reason ordering: severity descending, then code, then subject, then
/// detail, then evidence generation. Two runs over the same inputs therefore
/// produce identical traces.
[[nodiscard]] bool ReasonLess(const Reason& a, const Reason& b) noexcept;

/// Sorts a trace into canonical order and removes exact duplicates.
void CanonicalizeReasons(std::vector<Reason>* reasons);

/// Outcome implied by a set of reasons, by the documented precedence rule.
[[nodiscard]] Outcome OutcomeFromReasons(const std::vector<Reason>& reasons) noexcept;

/// A bounded intent: what this runtime is willing to ask another authority to
/// do. An intent is advisory. Nothing here is a command, an actuator message, or
/// evidence that anything happened.
struct BoundedIntent {
  IntentId id;
  RequestKind kind{RequestKind::kShiftFlexibleLoad};
  ServiceClassId service_class;
  /// Upper bound on load movement; never above the requested magnitude or the
  /// policy limit.
  PowerKw magnitude_limit_kw;
  /// Upper bound on duration.
  DurationSec duration_limit;
  Interval window;
  /// The intent is void after this instant even if nothing acts on it.
  UtcInstant expires_at;
  /// Authority that owns the effect. This runtime only proposes.
  AuthorityKind target_authority{AuthorityKind::kCapacityAuthority};
  /// Always true, and asserted by tests. Present so a consumer cannot mistake an
  /// intent for an effect.
  bool advisory_only{true};
};

/// Which authority owns the effect a request kind would produce.
[[nodiscard]] AuthorityKind TargetAuthorityFor(RequestKind kind) noexcept;

/// Projected economics of a request, computed from tariff evidence with exact
/// integer arithmetic. Reported separately from authorisation.
struct EconomicAssessment {
  /// False when economics could not be computed (missing or uncovered prices).
  bool evaluated{false};
  MoneyMicros baseline_cost;
  MoneyMicros candidate_cost;
  /// baseline - candidate. Positive means the action saves money.
  MoneyMicros gross_savings;
  MoneyMicros demand_charge_avoided;
  /// Energy-weighted average price over the source window.
  PriceMicrosPerKwh baseline_price;
  /// Energy-weighted average price the action would pay instead.
  PriceMicrosPerKwh candidate_price;
  std::optional<Interval> shift_target;
  std::uint32_t price_slices{0};
  bool favorable{false};
};

/// A complete decision with its trace.
struct Decision {
  DecisionId id;
  RequestId request_id;
  ClientId client_id;
  Epoch epoch;
  PolicyId policy_id;
  PolicyGeneration policy_generation;
  RequestKind kind{RequestKind::kShiftFlexibleLoad};
  ServiceClassId service_class;
  Outcome outcome{Outcome::kIndeterminate};
  std::vector<Reason> reasons;
  /// Every piece of evidence the evaluation consulted, in canonical order.
  std::vector<EvidenceRef> evidence_dependencies;
  std::optional<BoundedIntent> intent;
  /// Set exactly when the outcome is Deferred.
  std::optional<UtcInstant> reconsider_at;
  EconomicAssessment economics;
  /// Content digest of this decision, excluding the identity and the wall-clock
  /// instant it was produced. Equal inputs always produce an equal digest, which
  /// is what makes replay verification possible.
  Digest64 digest;
  UtcInstant decided_at;
};

/// Computes the content digest. Deterministic across processes and platforms.
[[nodiscard]] Digest64 ComputeDecisionDigest(const Decision& decision);

/// Human-readable one-line summary used by the CLI and by test diagnostics.
[[nodiscard]] std::string FormatDecisionSummary(const Decision& decision);

}  // namespace ecg

#endif  // ECG_DECISION_HPP
