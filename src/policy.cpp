#include "ecg/policy.hpp"

namespace ecg {

const char* RuleIdName(RuleId rule) noexcept {
  switch (rule) {
    case RuleId::kRequestContract: return "request_contract";
    case RuleId::kPolicyGeneration: return "policy_generation";
    case RuleId::kEpochConsistency: return "epoch_consistency";
    case RuleId::kEvidencePresence: return "evidence_presence";
    case RuleId::kEvidenceFreshness: return "evidence_freshness";
    case RuleId::kEvidenceValidity: return "evidence_validity";
    case RuleId::kWindowCoverage: return "window_coverage";
    case RuleId::kServiceTerms: return "service_terms";
    case RuleId::kReserveFloor: return "reserve_floor";
    case RuleId::kCapacity: return "capacity";
    case RuleId::kRisk: return "risk";
    case RuleId::kIncident: return "incident";
    case RuleId::kDemandCharge: return "demand_charge";
    case RuleId::kEconomics: return "economics";
    case RuleId::kIntentBounding: return "intent_bounding";
    case RuleId::kOutcome: return "outcome";
  }
  return "unknown";
}

Status ValidatePolicy(const PolicySet& policy) {
  if (policy.id.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, "policy", "policy id is empty");
  }
  if (policy.generation.is_zero()) {
    return MakeError(ErrorCode::kMissingRequiredField, "policy:" + policy.id.ToString(),
                     "policy generation must be non-zero");
  }
  if (policy.max_intent_magnitude_kw.is_negative()) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "max intent magnitude must not be negative");
  }
  if (policy.max_intent_magnitude_kw.raw() == 0) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "max intent magnitude must be positive, otherwise no intent can ever be emitted");
  }
  if (policy.max_intent_duration.raw() <= 0) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "max intent duration must be positive");
  }
  if (policy.decision_horizon.raw() <= 0) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "decision horizon must be positive");
  }
  if (policy.reserve_safety_margin.raw() < 0 || policy.reserve_safety_margin.raw() > 1000000) {
    return MakeError(ErrorCode::kOutOfRange, "policy:" + policy.id.ToString(),
                     "reserve safety margin must be within [0, 1]");
  }
  if (policy.max_service_curtailment_ratio.raw() < 0 ||
      policy.max_service_curtailment_ratio.raw() > 1000000) {
    return MakeError(ErrorCode::kOutOfRange, "policy:" + policy.id.ToString(),
                     "max service curtailment ratio must be within [0, 1]");
  }
  if (policy.minimum_economic_benefit.is_negative()) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "minimum economic benefit must not be negative");
  }
  if (policy.limits.future_skew.is_negative()) {
    return MakeError(ErrorCode::kInvalidArgument, "policy:" + policy.id.ToString(),
                     "future skew must not be negative");
  }
  if (policy.max_reasons == 0 || policy.max_reasons > 4096) {
    return MakeError(ErrorCode::kOutOfRange, "policy:" + policy.id.ToString(),
                     "max_reasons must be within [1, 4096]");
  }
  if (policy.max_price_slices == 0 || policy.max_price_slices > 1000000) {
    return MakeError(ErrorCode::kOutOfRange, "policy:" + policy.id.ToString(),
                     "max_price_slices must be within [1, 1000000]");
  }
  return OkStatus();
}

PolicySet DefaultPolicy() {
  PolicySet policy;
  policy.id = PolicyId::FromTrusted("ecg.default");
  policy.generation = PolicyGeneration::FromRaw(1);
  policy.epoch = Epoch::FromRaw(1);
  policy.evidence_generation = EvidenceGeneration::FromRaw(1);

  policy.limits.tariff_max_age = DurationSec::FromRaw(6 * 3600);
  policy.limits.demand_max_age = DurationSec::FromRaw(300);
  policy.limits.reserve_max_age = DurationSec::FromRaw(300);
  policy.limits.efficiency_max_age = DurationSec::FromRaw(3600);
  policy.limits.services_max_age = DurationSec::FromRaw(24 * 3600);
  policy.limits.capacity_max_age = DurationSec::FromRaw(900);
  policy.limits.risk_max_age = DurationSec::FromRaw(900);
  policy.limits.incident_max_age = DurationSec::FromRaw(120);
  policy.limits.zone_max_age = DurationSec::FromRaw(365LL * 24 * 3600);
  policy.limits.future_skew = DurationSec::FromRaw(60);

  policy.decision_horizon = DurationSec::FromRaw(6 * 3600);
  policy.reaction_lead_time = DurationSec::FromRaw(15 * 60);
  policy.shift_search_horizon = DurationSec::FromRaw(4 * 3600);
  policy.incident_recheck_interval = DurationSec::FromRaw(15 * 60);

  policy.max_intent_magnitude_kw = PowerKw::FromRaw(20000000);  // 20 MW
  policy.max_intent_duration = DurationSec::FromRaw(4 * 3600);

  policy.reserve_safety_margin = RatioPpm::FromRaw(50000);  // 5%
  policy.minimum_economic_benefit = MoneyMicros::FromRaw(1000000);  // 1.0 currency unit
  policy.require_positive_economic_benefit = true;
  policy.prohibit_demand_charge_increase = true;
  policy.max_service_curtailment_ratio = RatioPpm::FromRaw(500000);  // 50%

  policy.max_reasons = 64;
  policy.max_price_slices = 4096;
  return policy;
}

}  // namespace ecg
