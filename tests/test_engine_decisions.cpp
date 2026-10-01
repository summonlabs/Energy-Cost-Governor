// Decision semantics: every outcome, every rule, and the deterministic trace
// that explains it.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;
using ecg::Outcome;
using ecg::ReasonCode;
using ecg::RequestKind;

namespace {

const char* kEveningBegin = "2026-03-02T18:30:00.000000Z";
const char* kEveningEnd = "2026-03-02T19:00:00.000000Z";

ecg::DecisionRequest Request(const Scenario& scenario, std::uint64_t ordinal, RequestKind kind,
                             ecg::PowerKw magnitude, ecg::DurationSec duration,
                             const char* begin, const char* end, const char* service_class,
                             ecg::Rationale rationale) {
  ecg::DecisionRequest request =
      MakeRequest(scenario, ordinal, kind, magnitude, duration, Span(begin, end), service_class);
  request.rationale = rationale;
  return request;
}

ecg::Decision Run(const Scenario& scenario, const ecg::DecisionRequest& request) {
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  if (!engine.ok()) {
    std::abort();
  }
  return EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
}

}  // namespace

ECG_TEST("decision.allowed_shift_produces_a_bounded_advisory_intent") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 1, RequestKind::kShiftFlexibleLoad, Kw(800), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kAllowed);
  ECG_REQUIRE(decision.intent.has_value());
  const ecg::BoundedIntent& intent = *decision.intent;
  ECG_CHECK(intent.advisory_only);
  ECG_CHECK(intent.magnitude_limit_kw <= ecg::PowerKw::FromRaw(800000));
  ECG_CHECK(intent.window.begin() == decision.intent->window.begin());
  ECG_CHECK(intent.expires_at == Span(kEveningBegin, kEveningEnd).end());
  ECG_CHECK_EQ(intent.target_authority, ecg::AuthorityKind::kCapacityAuthority);
  ECG_CHECK(intent.id.is_set());
  ECG_CHECK(decision.reconsider_at.has_value() == false);
  ECG_CHECK(decision.economics.evaluated);
}

ECG_TEST("decision.intent_duration_beyond_policy_is_refused_not_shortened") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision = Run(
      scenario, Request(scenario, 2, RequestKind::kShiftFlexibleLoad, Kw(800),
                        ecg::DurationSec::FromRaw(scenario.policy.max_intent_duration.raw() + 1),
                        kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kIntentDurationExceedsPolicy));
  ECG_CHECK(!decision.intent.has_value());
}

ECG_TEST("decision.magnitude_beyond_policy_is_bounded_and_annotated") {
  Scenario scenario = MakeScenario();
  scenario.policy.max_intent_magnitude_kw = Kw(500);
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 3, RequestKind::kShiftFlexibleLoad, Kw(1500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(HasReason(decision, ReasonCode::kIntentBounded));
  if (decision.outcome == Outcome::kAllowed) {
    ECG_REQUIRE(decision.intent.has_value());
    ECG_CHECK_EQ(decision.intent->magnitude_limit_kw, Kw(500));
  }
}

ECG_TEST("decision.reserve_floor_blocks_load_increases") {
  const Scenario scenario = MakeScenario();
  // Charging storage raises load and therefore consumes reserve headroom. The
  // required reserve is 1200 kW floor plus a 5% of 12000 kW margin: 1800 kW.
  // Current reserve is 2500 kW, so 800 kW is the largest safe increase.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 4, RequestKind::kChargeStorage, Kw(900), Secs(1800),
                            "2026-03-02T12:10:00.000000Z", "2026-03-02T12:40:00.000000Z", "",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(HasReason(decision, ReasonCode::kReserveFloorBreach));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
}

ECG_TEST("decision.reserve_floor_allows_a_bounded_load_increase") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 5, RequestKind::kChargeStorage, Kw(700), Secs(1800),
                            "2026-03-02T12:10:00.000000Z", "2026-03-02T12:40:00.000000Z", "",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(!HasReason(decision, ReasonCode::kReserveFloorBreach));
  ECG_CHECK(HasReason(decision, ReasonCode::kReserveHeadroomSufficient));
}

ECG_TEST("decision.load_reduction_adds_reserve_headroom") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 6, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(HasReason(decision, ReasonCode::kReserveHeadroomSufficient));
  ECG_CHECK(!HasReason(decision, ReasonCode::kReserveFloorBreach));
}

ECG_TEST("decision.protected_service_class_is_refused") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 7, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.critical", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceCurtailmentProhibited));
}

ECG_TEST("decision.unknown_service_class_is_refused_not_invented") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 8, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.unlisted", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceClassUnknown));
}

ECG_TEST("decision.notice_requirement_is_enforced") {
  const Scenario scenario = MakeScenario();
  // vm.batch requires 1800 s of notice; a window 600 s away cannot satisfy it.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 9, RequestKind::kCurtailServiceClass, Kw(500), Secs(600),
                            "2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z", "vm.batch",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceNoticeInsufficient));
}

ECG_TEST("decision.curtailment_duration_limit_is_enforced") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision = Run(
      scenario, Request(scenario, 10, RequestKind::kCurtailServiceClass, Kw(500), Secs(10800),
                        kEveningBegin, "2026-03-02T21:30:00.000000Z", "vm.batch",
                        ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceDurationExceeded));
}

ECG_TEST("decision.curtailment_ratio_limit_is_enforced") {
  const Scenario scenario = MakeScenario();
  // vm.batch permits 25% of a 4000 kW nominal load: 1000 kW.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 11, RequestKind::kCurtailServiceClass, Kw(1200), Secs(1800),
                            "2026-03-02T11:00:00.000000Z", "2026-03-02T11:30:00.000000Z", "vm.batch",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceRatioExceeded));
}

ECG_TEST("decision.flexible_capacity_limit_is_enforced") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 12, RequestKind::kShiftFlexibleLoad, Kw(2600), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kCapacityInsufficient));
}

ECG_TEST("decision.risk_posture_restricted_blocks_everything_but_essentials") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.posture = ecg::RiskPosture::kRestricted;
  const ecg::Decision shift =
      Run(scenario, Request(scenario, 13, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(shift.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(shift, ReasonCode::kRiskPostureRestricted));

  const ecg::Decision restoration =
      Run(scenario, Request(scenario, 14, RequestKind::kReserveRestoration, Kw(500), Secs(600),
                            "2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z", "",
                            ecg::Rationale::kReserveRestoration));
  ECG_CHECK(!HasReason(restoration, ReasonCode::kRiskPostureRestricted));
}

ECG_TEST("decision.risk_posture_elevated_blocks_load_increases_only") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.posture = ecg::RiskPosture::kElevated;
  const ecg::Decision increase =
      Run(scenario, Request(scenario, 15, RequestKind::kChargeStorage, Kw(200), Secs(1800),
                            "2026-03-02T12:10:00.000000Z", "2026-03-02T12:40:00.000000Z", "",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(increase.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(increase, ReasonCode::kRiskPostureRestricted));

  const ecg::Decision reduction =
      Run(scenario, Request(scenario, 16, RequestKind::kCurtailServiceClass, Kw(300), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(!HasReason(reduction, ReasonCode::kRiskPostureRestricted));
}

ECG_TEST("decision.ramp_limit_is_enforced_over_the_action_duration") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.max_ramp_kw_per_min = KwPerMin(10);
  // 500 kW over 600 s is 50 kW/min, above the published 10 kW/min.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 17, RequestKind::kCurtailServiceClass, Kw(500), Secs(600),
                            "2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kRampLimitExceeded));
}

ECG_TEST("decision.minimum_dwell_is_enforced") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.minimum_dwell = Secs(1800);
  scenario.evidence.risk->value.last_change_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() - 600LL * 1000000LL);
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 18, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kDwellNotElapsed));
}

ECG_TEST("decision.deferral_limit_is_enforced_for_batch_work") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.max_deferral = Secs(600);
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 19, RequestKind::kDeferBatchWork, Kw(500), Secs(3600),
                            kEveningBegin, "2026-03-02T19:30:00.000000Z", "vm.batch",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kDeferralLimitExceeded));
}

ECG_TEST("decision.critical_incident_refuses_cost_actions") {
  Scenario scenario = MakeScenario();
  scenario.evidence.incident->value.severity = ecg::IncidentSeverity::kCritical;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 20, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kIncidentCritical));
}

ECG_TEST("decision.major_incident_defers_and_names_the_reconsideration") {
  Scenario scenario = MakeScenario();
  scenario.evidence.incident->value.severity = ecg::IncidentSeverity::kMajor;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 21, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  ECG_CHECK(HasReason(decision, ReasonCode::kIncidentActive));
  ECG_REQUIRE(decision.reconsider_at.has_value());
  ECG_CHECK(decision.reconsider_at.value() > scenario.now);
  ECG_CHECK(!decision.intent.has_value());
}

ECG_TEST("decision.advisory_incident_on_an_affected_class_defers") {
  Scenario scenario = MakeScenario();
  scenario.evidence.incident->value.severity = ecg::IncidentSeverity::kAdvisory;
  scenario.evidence.incident->value.affected_classes.push_back(
      ecg::ServiceClassId::FromTrusted("vm.flex"));
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 22, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  ECG_CHECK(HasReason(decision, ReasonCode::kIncidentActive));
}

ECG_TEST("decision.window_in_the_past_is_refused") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 23, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            "2026-03-02T09:00:00.000000Z", "2026-03-02T09:30:00.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kWindowInPast));
}

ECG_TEST("decision.window_beyond_the_horizon_is_deferred") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 24, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            "2026-03-03T18:30:00.000000Z", "2026-03-03T19:00:00.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  ECG_CHECK(HasReason(decision, ReasonCode::kWindowBeyondHorizon));
  ECG_REQUIRE(decision.reconsider_at.has_value());
}

ECG_TEST("decision.price_gap_makes_the_question_unanswerable") {
  Scenario scenario = MakeScenario();
  scenario.evidence.tariff->value.prices.clear();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 25, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kPriceCoverageGap));
}

ECG_TEST("decision.ambiguous_price_schedule_is_reported_as_ambiguous") {
  Scenario scenario = MakeScenario();
  scenario.evidence.tariff->value.prices.push_back(scenario.evidence.tariff->value.prices.front());
  scenario.evidence.tariff->meta.payload_version = ecg::kMaxSupportedPayloadVersion;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 26, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceConflicting) ||
            HasReason(decision, ReasonCode::kPriceCoverageAmbiguous));
}

ECG_TEST("decision.economically_unfavourable_action_is_refused_on_policy") {
  Scenario scenario = MakeScenario();
  scenario.policy.defer_early_demand_requests = false;
  // A one-second window priced at the cheapest band of the day cannot recover
  // the policy's minimum benefit.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 27, RequestKind::kCurtailServiceClass, Kw(100), Secs(1),
                            "2026-03-02T22:00:00.000000Z", "2026-03-02T22:00:01.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(HasReason(decision, ReasonCode::kEconomicallyUnfavorable));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
}

ECG_TEST("decision.economic_requirement_can_be_relaxed_by_policy") {
  Scenario scenario = MakeScenario();
  scenario.policy.require_positive_economic_benefit = false;
  scenario.policy.defer_early_demand_requests = false;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 28, RequestKind::kCurtailServiceClass, Kw(100), Secs(1),
                            "2026-03-02T22:00:00.000000Z", "2026-03-02T22:00:01.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(!HasReason(decision, ReasonCode::kEconomicallyUnfavorable));
}

ECG_TEST("decision.reserve_restoration_is_never_refused_for_economics") {
  Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 29, RequestKind::kReserveRestoration, Kw(1), Secs(1),
                            "2026-03-02T12:00:01.000000Z", "2026-03-02T12:00:02.000000Z", "",
                            ecg::Rationale::kReserveRestoration));
  ECG_CHECK(!HasReason(decision, ReasonCode::kEconomicallyUnfavorable));
}

ECG_TEST("decision.negative_prices_are_representable_and_priced_exactly") {
  Scenario scenario = MakeScenario();
  const auto negative = PriceMilli(-250);
  for (ecg::PriceInterval& interval : scenario.evidence.tariff->value.prices) {
    if (interval.window.begin() == Instant("2026-03-02T18:00:00.000000Z")) {
      interval.price = negative;
    }
  }
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 30, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex",
                            ecg::Rationale::kObligationCompliance));
  ECG_CHECK_EQ(decision.economics.baseline_price, negative);
  ECG_CHECK(decision.economics.baseline_cost.is_negative());
}

ECG_TEST("decision.demand_charge_avoidance_is_economically_visible") {
  Scenario scenario = MakeScenario();
  // The rolling peak sits below the demand threshold and current demand sits
  // above it, so a reduction in the demand window genuinely lowers the billed
  // peak rather than merely trimming a peak that is already banked.
  scenario.evidence.demand->value.rolling_peak_kw = Kw(8000);
  scenario.evidence.demand->value.current_demand_kw = Kw(9800);
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 31, RequestKind::kCurtailServiceClass, Kw(800), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex",
                            ecg::Rationale::kDemandChargeAvoidance));
  ECG_CHECK(decision.economics.demand_charge_avoided.is_positive());
  ECG_CHECK(HasReason(decision, ReasonCode::kDemandChargeAvoided));
}

ECG_TEST("decision.demand_charge_increase_is_refused_when_prohibited") {
  Scenario scenario = MakeScenario();
  scenario.policy.defer_early_demand_requests = false;
  // Pre-cooling raises load during the peak window the demand rule covers.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 32, RequestKind::kPreCooling, Kw(900), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex",
                            ecg::Rationale::kDemandChargeAvoidance));
  ECG_CHECK(HasReason(decision, ReasonCode::kDemandChargeIncrease));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
}

ECG_TEST("decision.demand_charge_prohibition_can_be_disabled") {
  Scenario scenario = MakeScenario();
  scenario.policy.prohibit_demand_charge_increase = false;
  scenario.policy.defer_early_demand_requests = false;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 33, RequestKind::kPreCooling, Kw(900), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex",
                            ecg::Rationale::kDemandChargeAvoidance));
  ECG_CHECK(!HasReason(decision, ReasonCode::kDemandChargeIncrease));
}

ECG_TEST("decision.action_outside_every_demand_window_carries_no_demand_constraint") {
  Scenario scenario = MakeScenario();
  scenario.policy.defer_early_demand_requests = false;
  // Midday is outside the 18:00-22:00 weekday demand window entirely, so an
  // action there cannot raise a billed peak and must not be reported as an
  // unresolvable demand question.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 50, RequestKind::kPreCooling, Kw(500), Secs(1800),
                            "2026-03-02T12:30:00.000000Z", "2026-03-02T13:00:00.000000Z", "vm.flex",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(!HasReason(decision, ReasonCode::kDemandWindowUnresolved));
  ECG_CHECK(!HasReason(decision, ReasonCode::kDemandChargeIncrease));
}

ECG_TEST("decision.demand_avoidance_outside_every_demand_window_is_unanswerable") {
  Scenario scenario = MakeScenario();
  // The economic requirement is relaxed so that the demand indeterminacy is the
  // strongest reason in the trace and the outcome it produces is unambiguous.
  scenario.policy.require_positive_economic_benefit = false;
  // The stated justification cannot be established from the tariff, so the
  // question is returned as unanswerable rather than guessed at.
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 51, RequestKind::kPreCooling, Kw(500), Secs(1800),
                            "2026-03-02T12:30:00.000000Z", "2026-03-02T13:00:00.000000Z", "vm.flex",
                            ecg::Rationale::kDemandChargeAvoidance));
  ECG_CHECK(HasReason(decision, ReasonCode::kDemandWindowUnresolved));
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
}

ECG_TEST("decision.demand_rule_without_zone_rules_is_indeterminate") {
  Scenario scenario = MakeScenario();
  scenario.evidence.zone.reset();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 34, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex",
                            ecg::Rationale::kDemandChargeAvoidance));
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kZoneRulesMissing));
}

ECG_TEST("decision.storage_requires_a_published_round_trip_efficiency") {
  Scenario scenario = MakeScenario();
  scenario.evidence.efficiency->value.storage_round_trip_ratio.reset();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 35, RequestKind::kChargeStorage, Kw(500), Secs(1800),
                            "2026-03-02T23:00:00.000000Z", "2026-03-02T23:30:00.000000Z", "",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK(HasReason(decision, ReasonCode::kStorageEfficiencyMissing));
}

ECG_TEST("decision.storage_energy_limit_is_enforced") {
  Scenario scenario = MakeScenario();
  scenario.evidence.capacity->value.storage_usable_energy_kwh = Kwh(10);
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 36, RequestKind::kChargeStorage, Kw(500), Secs(1800),
                            "2026-03-02T23:00:00.000000Z", "2026-03-02T23:30:00.000000Z", "",
                            ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
  ECG_CHECK(HasReason(decision, ReasonCode::kStorageEnergyInsufficient));
}

ECG_TEST("decision.stale_policy_generation_is_indeterminate") {
  Scenario scenario = MakeScenario();
  ecg::DecisionRequest request =
      Request(scenario, 37, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800), kEveningBegin,
              kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  request.policy_generation = ecg::PolicyGeneration::FromRaw(scenario.policy.generation.raw() + 1);
  const ecg::Decision decision = Run(scenario, request);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kPolicyGenerationStale));
}

ECG_TEST("decision.stale_request_epoch_is_indeterminate") {
  Scenario scenario = MakeScenario();
  ecg::DecisionRequest request =
      Request(scenario, 38, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800), kEveningBegin,
              kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  request.epoch = ecg::Epoch::FromRaw(scenario.policy.epoch.raw() + 1);
  const ecg::Decision decision = Run(scenario, request);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kRequestEpochMismatch));
}

ECG_TEST("decision.trace_is_deterministically_ordered") {
  Scenario scenario = MakeScenario();
  scenario.evidence.capacity.reset();
  scenario.evidence.incident->value.severity = ecg::IncidentSeverity::kCritical;
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 39, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_REQUIRE(decision.reasons.size() >= 2);
  for (std::size_t i = 1; i < decision.reasons.size(); ++i) {
    ECG_CHECK(!ecg::ReasonLess(decision.reasons[i], decision.reasons[i - 1]));
  }
}

ECG_TEST("decision.identical_inputs_produce_identical_decisions") {
  const Scenario scenario = MakeScenario();
  const ecg::DecisionRequest request =
      Request(scenario, 40, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800), kEveningBegin,
              kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  const ecg::Decision first = Run(scenario, request);
  const ecg::Decision second = Run(scenario, request);
  ECG_CHECK(first.digest == second.digest);
  ECG_CHECK(first.id == second.id);
  ECG_CHECK_EQ(ecg::ToJson(first).Dump(), ecg::ToJson(second).Dump());
  if (first.intent.has_value() && second.intent.has_value()) {
    ECG_CHECK(first.intent->id == second.intent->id);
  }
}

ECG_TEST("decision.different_evidence_generations_change_the_identity") {
  const Scenario scenario = MakeScenario();
  const ecg::DecisionRequest request =
      Request(scenario, 41, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800), kEveningBegin,
              kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  const ecg::Decision first = Run(scenario, request);
  Scenario updated = scenario;
  updated.evidence.demand->meta.generation = ecg::EvidenceGeneration::FromRaw(99);
  const ecg::Decision second = Run(updated, request);
  ECG_CHECK(first.digest != second.digest);
}

ECG_TEST("decision.evidence_dependencies_name_every_consulted_source") {
  const Scenario scenario = MakeScenario();
  const ecg::Decision decision =
      Run(scenario, Request(scenario, 42, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  std::set<std::string> sources;
  for (const ecg::EvidenceRef& ref : decision.evidence_dependencies) {
    sources.insert(ref.source.value());
  }
  for (const char* expected : {"tariff.feed", "meter.main", "reserve.authority", "service.catalog",
                               "capacity.authority", "risk.authority", "incident.authority",
                               "tz.authority"}) {
    ECG_CHECK_MSG(sources.count(expected) == 1, expected);
  }
}

ECG_TEST("decision.batch_evaluation_matches_individual_evaluation") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  std::vector<ecg::DecisionRequest> requests;
  requests.push_back(Request(scenario, 43, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                             kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  requests.push_back(Request(scenario, 44, RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                             kEveningBegin, kEveningEnd, "vm.critical", ecg::Rationale::kPriceArbitrage));
  requests.push_back(Request(scenario, 45, RequestKind::kChargeStorage, Kw(900), Secs(1800),
                             "2026-03-02T12:10:00.000000Z", "2026-03-02T12:40:00.000000Z", "",
                             ecg::Rationale::kPriceArbitrage));
  const auto batch = engine.value().EvaluateBatch(requests, scenario.evidence, scenario.now);
  ECG_REQUIRE(batch.ok());
  ECG_CHECK_EQ(batch.value().size(), requests.size());
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const ecg::Decision single =
        EvaluateOrDie(engine.value(), requests[i], scenario.evidence, scenario.now);
    ECG_CHECK(batch.value()[i].id == single.id);
    ECG_CHECK_EQ(ecg::ToJson(batch.value()[i]).Dump(), ecg::ToJson(single).Dump());
  }
}

ECG_TEST("decision.structurally_invalid_requests_are_errors_not_outcomes") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());

  ecg::DecisionRequest zero_magnitude =
      Request(scenario, 46, RequestKind::kShiftFlexibleLoad, ecg::PowerKw::FromRaw(0), Secs(1800),
              kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  ECG_CHECK_ERR(engine.value().Evaluate(zero_magnitude, scenario.evidence, scenario.now),
                ecg::ErrorCode::kInvalidArgument);

  ecg::DecisionRequest no_class =
      Request(scenario, 47, RequestKind::kCurtailServiceClass, Kw(100), Secs(1800), kEveningBegin,
              kEveningEnd, "", ecg::Rationale::kPriceArbitrage);
  ECG_CHECK_ERR(engine.value().Evaluate(no_class, scenario.evidence, scenario.now),
                ecg::ErrorCode::kMissingRequiredField);

  ecg::DecisionRequest unset_id =
      Request(scenario, 48, RequestKind::kShiftFlexibleLoad, Kw(100), Secs(1800), kEveningBegin,
              kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage);
  unset_id.request_id = ecg::RequestId::FromRaw(0);
  ECG_CHECK_ERR(engine.value().Evaluate(unset_id, scenario.evidence, scenario.now),
                ecg::ErrorCode::kMissingRequiredField);
}

ECG_TEST("decision.every_reason_code_has_a_stable_name_and_class") {
  // Guards the reason table against a half-added code: a code whose name is
  // "unknown" or whose class silently defaults would produce a trace nobody can
  // audit.
  const std::vector<ecg::ReasonCode> codes = {
      ReasonCode::kAuthorized, ReasonCode::kIntentBounded, ReasonCode::kEconomicsFavorable,
      ReasonCode::kDemandChargeAvoided, ReasonCode::kPriceCoverageVerified,
      ReasonCode::kShiftTargetSelected, ReasonCode::kServiceTermsSatisfied,
      ReasonCode::kReserveHeadroomSufficient, ReasonCode::kIncidentClear,
      ReasonCode::kPolicyGenerationCurrent, ReasonCode::kEvidenceMissing,
      ReasonCode::kEvidenceStale, ReasonCode::kEvidenceFutureDated, ReasonCode::kEvidenceRecovered,
      ReasonCode::kEvidenceEpochMismatch, ReasonCode::kEvidenceUnsupportedVersion,
      ReasonCode::kEvidenceConflicting, ReasonCode::kEvidenceInvalid,
      ReasonCode::kPolicyGenerationStale, ReasonCode::kRequestEpochMismatch,
      ReasonCode::kPriceCoverageGap, ReasonCode::kPriceCoverageAmbiguous,
      ReasonCode::kZoneRulesMissing, ReasonCode::kEconomicsOverflow,
      ReasonCode::kResourceLimitExceeded, ReasonCode::kTariffValidityMiss,
      ReasonCode::kStorageEfficiencyMissing, ReasonCode::kDemandWindowUnresolved,
      ReasonCode::kReserveFloorBreach, ReasonCode::kServiceClassUnknown,
      ReasonCode::kServiceCurtailmentProhibited, ReasonCode::kServiceNoticeInsufficient,
      ReasonCode::kServiceDurationExceeded, ReasonCode::kServiceRatioExceeded,
      ReasonCode::kCapacityInsufficient, ReasonCode::kRiskPostureRestricted,
      ReasonCode::kRampLimitExceeded, ReasonCode::kDwellNotElapsed, ReasonCode::kIncidentCritical,
      ReasonCode::kWindowInPast, ReasonCode::kIntentDurationExceedsPolicy,
      ReasonCode::kDemandChargeIncrease, ReasonCode::kEconomicallyUnfavorable,
      ReasonCode::kStorageEnergyInsufficient, ReasonCode::kStoragePowerInsufficient,
      ReasonCode::kDeferralLimitExceeded, ReasonCode::kMagnitudeExceedsPolicy,
      ReasonCode::kIncidentActive, ReasonCode::kWindowBeyondHorizon,
      ReasonCode::kDemandThresholdNotProjected, ReasonCode::kShiftTargetNotYetAvailable};
  for (const ReasonCode code : codes) {
    const std::string name = ecg::ReasonCodeName(code);
    ECG_CHECK_MSG(name != "unknown" && !name.empty(), std::to_string(static_cast<int>(code)));
  }
  // Every Deferred outcome must carry a reconsideration instant. This is checked
  // structurally by the engine's finalisation step, and here for the incident and
  // horizon cases that actually defer.
  const Scenario scenario = MakeScenario();
  Scenario deferred = scenario;
  deferred.evidence.incident->value.severity = ecg::IncidentSeverity::kMajor;
  const ecg::Decision decision =
      Run(deferred, Request(deferred, 49, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                            kEveningBegin, kEveningEnd, "vm.flex", ecg::Rationale::kPriceArbitrage));
  ECG_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  ECG_CHECK(decision.reconsider_at.has_value());
}
