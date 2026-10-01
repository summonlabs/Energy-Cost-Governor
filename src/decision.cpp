#include "ecg/decision.hpp"

#include <algorithm>

#include "ecg/canonical.hpp"
#include "ecg/version.hpp"

namespace ecg {

const char* OutcomeName(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kAllowed: return "allowed";
    case Outcome::kRefused: return "refused";
    case Outcome::kDeferred: return "deferred";
    case Outcome::kIndeterminate: return "indeterminate";
  }
  return "unknown";
}

const char* ReasonSeverityName(ReasonSeverity severity) noexcept {
  switch (severity) {
    case ReasonSeverity::kInfo: return "info";
    case ReasonSeverity::kNotice: return "notice";
    case ReasonSeverity::kBlocker: return "blocker";
  }
  return "unknown";
}

const char* ReasonCodeName(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::kNone: return "none";

    case ReasonCode::kAuthorized: return "authorized";
    case ReasonCode::kIntentBounded: return "intent_bounded";
    case ReasonCode::kEconomicsFavorable: return "economics_favorable";
    case ReasonCode::kDemandChargeAvoided: return "demand_charge_avoided";
    case ReasonCode::kPriceCoverageVerified: return "price_coverage_verified";
    case ReasonCode::kShiftTargetSelected: return "shift_target_selected";
    case ReasonCode::kServiceTermsSatisfied: return "service_terms_satisfied";
    case ReasonCode::kReserveHeadroomSufficient: return "reserve_headroom_sufficient";
    case ReasonCode::kIncidentClear: return "incident_clear";
    case ReasonCode::kPolicyGenerationCurrent: return "policy_generation_current";

    case ReasonCode::kEvidenceMissing: return "evidence_missing";
    case ReasonCode::kEvidenceStale: return "evidence_stale";
    case ReasonCode::kEvidenceFutureDated: return "evidence_future_dated";
    case ReasonCode::kEvidenceRecovered: return "evidence_recovered";
    case ReasonCode::kEvidenceEpochMismatch: return "evidence_epoch_mismatch";
    case ReasonCode::kEvidenceUnsupportedVersion: return "evidence_unsupported_version";
    case ReasonCode::kEvidenceConflicting: return "evidence_conflicting";
    case ReasonCode::kEvidenceInvalid: return "evidence_invalid";
    case ReasonCode::kPolicyGenerationStale: return "policy_generation_stale";
    case ReasonCode::kRequestEpochMismatch: return "request_epoch_mismatch";
    case ReasonCode::kPriceCoverageGap: return "price_coverage_gap";
    case ReasonCode::kPriceCoverageAmbiguous: return "price_coverage_ambiguous";
    case ReasonCode::kZoneRulesMissing: return "zone_rules_missing";
    case ReasonCode::kEconomicsOverflow: return "economics_overflow";
    case ReasonCode::kResourceLimitExceeded: return "resource_limit_exceeded";
    case ReasonCode::kTariffValidityMiss: return "tariff_validity_miss";
    case ReasonCode::kStorageEfficiencyMissing: return "storage_efficiency_missing";
    case ReasonCode::kDemandWindowUnresolved: return "demand_window_unresolved";

    case ReasonCode::kReserveFloorBreach: return "reserve_floor_breach";
    case ReasonCode::kServiceClassUnknown: return "service_class_unknown";
    case ReasonCode::kServiceCurtailmentProhibited: return "service_curtailment_prohibited";
    case ReasonCode::kServiceNoticeInsufficient: return "service_notice_insufficient";
    case ReasonCode::kServiceDurationExceeded: return "service_duration_exceeded";
    case ReasonCode::kServiceRatioExceeded: return "service_ratio_exceeded";
    case ReasonCode::kCapacityInsufficient: return "capacity_insufficient";
    case ReasonCode::kRiskPostureRestricted: return "risk_posture_restricted";
    case ReasonCode::kRampLimitExceeded: return "ramp_limit_exceeded";
    case ReasonCode::kDwellNotElapsed: return "dwell_not_elapsed";
    case ReasonCode::kIncidentCritical: return "incident_critical";
    case ReasonCode::kWindowInPast: return "window_in_past";
    case ReasonCode::kIntentDurationExceedsPolicy: return "intent_duration_exceeds_policy";
    case ReasonCode::kDemandChargeIncrease: return "demand_charge_increase";
    case ReasonCode::kEconomicallyUnfavorable: return "economically_unfavorable";
    case ReasonCode::kStorageEnergyInsufficient: return "storage_energy_insufficient";
    case ReasonCode::kStoragePowerInsufficient: return "storage_power_insufficient";
    case ReasonCode::kDeferralLimitExceeded: return "deferral_limit_exceeded";
    case ReasonCode::kMagnitudeExceedsPolicy: return "magnitude_exceeds_policy";

    case ReasonCode::kIncidentActive: return "incident_active";
    case ReasonCode::kWindowBeyondHorizon: return "window_beyond_horizon";
    case ReasonCode::kDemandThresholdNotProjected: return "demand_threshold_not_projected";
    case ReasonCode::kShiftTargetNotYetAvailable: return "shift_target_not_yet_available";
  }
  return "unknown";
}

ReasonClass ClassifyReasonCode(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::kNone:
    case ReasonCode::kAuthorized:
    case ReasonCode::kIntentBounded:
    case ReasonCode::kEconomicsFavorable:
    case ReasonCode::kDemandChargeAvoided:
    case ReasonCode::kPriceCoverageVerified:
    case ReasonCode::kShiftTargetSelected:
    case ReasonCode::kServiceTermsSatisfied:
    case ReasonCode::kReserveHeadroomSufficient:
    case ReasonCode::kIncidentClear:
    case ReasonCode::kPolicyGenerationCurrent:
      return ReasonClass::kSupporting;

    case ReasonCode::kIncidentActive:
    case ReasonCode::kWindowBeyondHorizon:
    case ReasonCode::kDemandThresholdNotProjected:
    case ReasonCode::kShiftTargetNotYetAvailable:
      return ReasonClass::kDeferred;

    case ReasonCode::kEvidenceMissing:
    case ReasonCode::kEvidenceStale:
    case ReasonCode::kEvidenceFutureDated:
    case ReasonCode::kEvidenceRecovered:
    case ReasonCode::kEvidenceEpochMismatch:
    case ReasonCode::kEvidenceUnsupportedVersion:
    case ReasonCode::kEvidenceConflicting:
    case ReasonCode::kEvidenceInvalid:
    case ReasonCode::kPolicyGenerationStale:
    case ReasonCode::kRequestEpochMismatch:
    case ReasonCode::kPriceCoverageGap:
    case ReasonCode::kPriceCoverageAmbiguous:
    case ReasonCode::kZoneRulesMissing:
    case ReasonCode::kEconomicsOverflow:
    case ReasonCode::kResourceLimitExceeded:
    case ReasonCode::kTariffValidityMiss:
    case ReasonCode::kStorageEfficiencyMissing:
    case ReasonCode::kDemandWindowUnresolved:
      return ReasonClass::kIndeterminate;

    case ReasonCode::kReserveFloorBreach:
    case ReasonCode::kServiceClassUnknown:
    case ReasonCode::kServiceCurtailmentProhibited:
    case ReasonCode::kServiceNoticeInsufficient:
    case ReasonCode::kServiceDurationExceeded:
    case ReasonCode::kServiceRatioExceeded:
    case ReasonCode::kCapacityInsufficient:
    case ReasonCode::kRiskPostureRestricted:
    case ReasonCode::kRampLimitExceeded:
    case ReasonCode::kDwellNotElapsed:
    case ReasonCode::kIncidentCritical:
    case ReasonCode::kWindowInPast:
    case ReasonCode::kIntentDurationExceedsPolicy:
    case ReasonCode::kDemandChargeIncrease:
    case ReasonCode::kEconomicallyUnfavorable:
    case ReasonCode::kStorageEnergyInsufficient:
    case ReasonCode::kStoragePowerInsufficient:
    case ReasonCode::kDeferralLimitExceeded:
    case ReasonCode::kMagnitudeExceedsPolicy:
      return ReasonClass::kRefused;
  }
  return ReasonClass::kIndeterminate;
}

ReasonSeverity DefaultSeverityFor(ReasonCode code) noexcept {
  switch (ClassifyReasonCode(code)) {
    case ReasonClass::kSupporting: return ReasonSeverity::kInfo;
    case ReasonClass::kDeferred: return ReasonSeverity::kNotice;
    case ReasonClass::kIndeterminate:
    case ReasonClass::kRefused: return ReasonSeverity::kBlocker;
  }
  return ReasonSeverity::kBlocker;
}

EvidenceRef MissingEvidenceRef(AuthorityKind authority, const SourceId& source) {
  EvidenceRef ref;
  ref.authority = authority;
  ref.source = source;
  ref.generation = EvidenceGeneration::FromRaw(0);
  ref.observed_at = UtcInstant::FromRaw(0);
  ref.freshness = Freshness::kMissing;
  return ref;
}

bool ReasonLess(const Reason& a, const Reason& b) noexcept {
  if (a.severity != b.severity) {
    return a.severity > b.severity;
  }
  if (a.code != b.code) {
    return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  if (a.detail != b.detail) {
    return a.detail < b.detail;
  }
  const std::uint64_t a_generation =
      a.evidence.has_value() ? a.evidence->generation.raw() : 0u;
  const std::uint64_t b_generation =
      b.evidence.has_value() ? b.evidence->generation.raw() : 0u;
  return a_generation < b_generation;
}

void CanonicalizeReasons(std::vector<Reason>* reasons) {
  std::stable_sort(reasons->begin(), reasons->end(), ReasonLess);
  reasons->erase(std::unique(reasons->begin(), reasons->end(),
                             [](const Reason& a, const Reason& b) {
                               return a.code == b.code && a.subject == b.subject &&
                                      a.detail == b.detail &&
                                      a.evidence.has_value() == b.evidence.has_value() &&
                                      (!a.evidence.has_value() ||
                                       a.evidence->generation == b.evidence->generation);
                             }),
                 reasons->end());
}

Outcome OutcomeFromReasons(const std::vector<Reason>& reasons) noexcept {
  ReasonClass strongest = ReasonClass::kSupporting;
  for (const Reason& reason : reasons) {
    const ReasonClass reason_class = ClassifyReasonCode(reason.code);
    if (static_cast<std::uint8_t>(reason_class) > static_cast<std::uint8_t>(strongest)) {
      strongest = reason_class;
    }
  }
  switch (strongest) {
    case ReasonClass::kRefused: return Outcome::kRefused;
    case ReasonClass::kIndeterminate: return Outcome::kIndeterminate;
    case ReasonClass::kDeferred: return Outcome::kDeferred;
    case ReasonClass::kSupporting: return Outcome::kAllowed;
  }
  return Outcome::kIndeterminate;
}

AuthorityKind TargetAuthorityFor(RequestKind kind) noexcept {
  switch (kind) {
    case RequestKind::kReserveRestoration:
      return AuthorityKind::kReserveAuthority;
    case RequestKind::kShiftFlexibleLoad:
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kDeferBatchWork:
    case RequestKind::kChargeStorage:
    case RequestKind::kDischargeStorage:
    case RequestKind::kPreCooling:
      return AuthorityKind::kCapacityAuthority;
  }
  return AuthorityKind::kCapacityAuthority;
}

namespace {

void EncodeEvidenceRef(const EvidenceRef& ref, Encoder* encoder) {
  encoder->U8(static_cast<std::uint8_t>(ref.authority));
  const Status source_status = encoder->String(ref.source.value());
  (void)source_status;  // Sources are bounded to 64 bytes, far below the encoder limit.
  encoder->U64(ref.generation.raw());
  encoder->I64(ref.observed_at.raw());
}

}  // namespace

Digest64 ComputeDecisionDigest(const Decision& decision) {
  Encoder encoder;
  encoder.U32(kEncodingVersion);

  // Request identity and content.
  encoder.U64(decision.request_id.raw());
  (void)encoder.String(decision.client_id.value());
  encoder.U64(decision.epoch.raw());
  encoder.U8(static_cast<std::uint8_t>(decision.kind));
  (void)encoder.String(decision.service_class.value());

  // Policy identity.
  (void)encoder.String(decision.policy_id.value());
  encoder.U64(decision.policy_generation.raw());

  // Evidence dependencies, already in canonical order.
  encoder.U32(static_cast<std::uint32_t>(decision.evidence_dependencies.size()));
  for (const EvidenceRef& ref : decision.evidence_dependencies) {
    EncodeEvidenceRef(ref, &encoder);
  }

  // Outcome and trace.
  encoder.U8(static_cast<std::uint8_t>(decision.outcome));
  encoder.U32(static_cast<std::uint32_t>(decision.reasons.size()));
  for (const Reason& reason : decision.reasons) {
    encoder.U16(static_cast<std::uint16_t>(reason.code));
    encoder.U8(static_cast<std::uint8_t>(reason.severity));
    (void)encoder.String(reason.subject);
    (void)encoder.String(reason.detail);
    encoder.Bool(reason.evidence.has_value());
    if (reason.evidence.has_value()) {
      EncodeEvidenceRef(*reason.evidence, &encoder);
    }
  }

  // Bounded intent, when one was produced.
  encoder.Bool(decision.intent.has_value());
  if (decision.intent.has_value()) {
    const BoundedIntent& intent = *decision.intent;
    // intent.id is derived from this digest and is therefore excluded from it.
    encoder.U8(static_cast<std::uint8_t>(intent.kind));
    (void)encoder.String(intent.service_class.value());
    encoder.I64(intent.magnitude_limit_kw.raw());
    encoder.I64(intent.duration_limit.raw());
    encoder.I64(intent.window.begin().raw());
    encoder.I64(intent.window.end().raw());
    encoder.I64(intent.expires_at.raw());
    encoder.U8(static_cast<std::uint8_t>(intent.target_authority));
    encoder.Bool(intent.advisory_only);
  }

  encoder.Bool(decision.reconsider_at.has_value());
  if (decision.reconsider_at.has_value()) {
    encoder.I64(decision.reconsider_at->raw());
  }

  // Economics.
  encoder.Bool(decision.economics.evaluated);
  encoder.I64(decision.economics.baseline_cost.raw());
  encoder.I64(decision.economics.candidate_cost.raw());
  encoder.I64(decision.economics.gross_savings.raw());
  encoder.I64(decision.economics.demand_charge_avoided.raw());
  encoder.I64(decision.economics.baseline_price.raw());
  encoder.I64(decision.economics.candidate_price.raw());
  encoder.Bool(decision.economics.shift_target.has_value());
  if (decision.economics.shift_target.has_value()) {
    encoder.I64(decision.economics.shift_target->begin().raw());
    encoder.I64(decision.economics.shift_target->end().raw());
  }
  encoder.U32(decision.economics.price_slices);
  encoder.Bool(decision.economics.favorable);

  DigestBuilder builder;
  builder.Update(encoder.bytes());
  return builder.digest();
}

std::string FormatDecisionSummary(const Decision& decision) {
  std::string out = std::string(OutcomeName(decision.outcome));
  out += " request=";
  out += decision.request_id.ToString();
  out += " kind=";
  out += RequestKindName(decision.kind);
  out += " code=";
  out += ReasonCodeName(decision.reasons.empty() ? ReasonCode::kNone : decision.reasons.front().code);
  out += " reasons=";
  out += std::to_string(decision.reasons.size());
  out += " digest=";
  out += decision.digest.Hex();
  return out;
}

}  // namespace ecg
