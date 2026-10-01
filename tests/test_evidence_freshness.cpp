// Freshness, provenance, and the rule that absent or unusable evidence never
// becomes permission.

#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;
using ecg::EvidenceCategory;
using ecg::Freshness;
using ecg::Outcome;
using ecg::ReasonCode;
using ecg::RequestKind;

namespace {

/// A request whose window sits inside the evening demand window on the fixture
/// Monday, so a healthy run has every input it needs.
ecg::DecisionRequest EveningShift(const Scenario& scenario, std::uint64_t ordinal) {
  ecg::DecisionRequest request =
      MakeRequest(scenario, ordinal, RequestKind::kShiftFlexibleLoad, Kw(800), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  request.rationale = ecg::Rationale::kPriceArbitrage;
  return request;
}

}  // namespace

ECG_TEST("freshness.fresh_evidence_is_usable") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 1), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kAllowed);
}

ECG_TEST("freshness.stale_evidence_is_indeterminate") {
  Scenario scenario = MakeScenario();
  // Push the reserve observation well beyond its policy age limit.
  scenario.evidence.reserve->meta.observed_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() - 400LL * 1000000LL);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 2), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceStale));
}

ECG_TEST("freshness.age_boundary_is_exact") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DurationSec limit = scenario.policy.MaxAgeFor(EvidenceCategory::kReserve);

  Scenario at_limit = scenario;
  at_limit.evidence.reserve->meta.observed_at = ecg::UtcInstant::FromRaw(
      scenario.now.raw() - limit.raw() * 1000000LL);
  const ecg::Decision boundary =
      EvaluateOrDie(engine.value(), EveningShift(at_limit, 3), at_limit.evidence, at_limit.now);
  ECG_CHECK(!HasReason(boundary, ReasonCode::kEvidenceStale));

  Scenario one_microsecond_late = scenario;
  one_microsecond_late.evidence.reserve->meta.observed_at = ecg::UtcInstant::FromRaw(
      scenario.now.raw() - limit.raw() * 1000000LL - 1);
  const ecg::Decision late = EvaluateOrDie(engine.value(), EveningShift(one_microsecond_late, 4),
                                           one_microsecond_late.evidence, one_microsecond_late.now);
  ECG_CHECK(HasReason(late, ReasonCode::kEvidenceStale));
}

ECG_TEST("freshness.future_dated_evidence_is_not_fresh") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->meta.observed_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() + 3600LL * 1000000LL);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 5), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceFutureDated));
}

ECG_TEST("freshness.small_clock_skew_is_tolerated") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->meta.observed_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() + 10LL * 1000000LL);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 6), scenario.evidence, scenario.now);
  ECG_CHECK(!HasReason(decision, ReasonCode::kEvidenceFutureDated));
}

ECG_TEST("freshness.recovered_evidence_is_never_fresh") {
  Scenario scenario = MakeScenario();
  scenario.evidence.reserve->meta.provenance = ecg::Provenance::kRecoveredPersistence;
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 7), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceRecovered));
}

ECG_TEST("freshness.recovered_evidence_never_authorises_any_kind") {
  // Recovered dynamic state must not become permission for any request kind.
  // Every present payload is marked recovered here, so no kind can find an input
  // it is allowed to rely on.
  for (const RequestKind kind : {RequestKind::kShiftFlexibleLoad, RequestKind::kCurtailServiceClass,
                                 RequestKind::kDeferBatchWork, RequestKind::kChargeStorage,
                                 RequestKind::kDischargeStorage, RequestKind::kPreCooling,
                                 RequestKind::kReserveRestoration}) {
    Scenario scenario = MakeScenario();
    ecg::MarkEvidenceRecovered(&scenario.evidence);
    const auto engine = ecg::GovernorEngine::Make(scenario.policy);
    ECG_REQUIRE(engine.ok());
    const std::string class_name =
        (kind == RequestKind::kCurtailServiceClass || kind == RequestKind::kDeferBatchWork ||
         kind == RequestKind::kPreCooling || kind == RequestKind::kShiftFlexibleLoad)
            ? "vm.flex"
            : "";
    const ecg::DecisionRequest request =
        MakeRequest(scenario, 100 + static_cast<std::uint64_t>(kind), kind, Kw(500), Secs(1800),
                    Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"),
                    class_name.c_str());
    const ecg::Decision decision =
        EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
    ECG_CHECK_MSG(decision.outcome != Outcome::kAllowed, ecg::RequestKindName(kind));
  }
}

ECG_TEST("freshness.epoch_mismatch_is_indeterminate") {
  Scenario scenario = MakeScenario();
  scenario.evidence.tariff->meta.epoch = ecg::Epoch::FromRaw(2);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 8), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceEpochMismatch));
}

ECG_TEST("freshness.unsupported_payload_version_is_indeterminate") {
  Scenario scenario = MakeScenario();
  scenario.evidence.capacity->meta.payload_version = ecg::kMaxSupportedPayloadVersion + 1;
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 9), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceUnsupportedVersion));
}

ECG_TEST("freshness.missing_evidence_is_named_per_category") {
  Scenario scenario = MakeScenario();
  scenario.evidence.capacity.reset();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 10), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceMissing));
  bool named = false;
  for (const ecg::Reason& reason : decision.reasons) {
    if (reason.code == ReasonCode::kEvidenceMissing && reason.subject == "evidence.capacity") {
      named = true;
    }
  }
  ECG_CHECK(named);
}

ECG_TEST("freshness.self_contradictory_tariff_is_conflicting") {
  Scenario scenario = MakeScenario();
  // Two overlapping price intervals make the price ambiguous, which is a defect
  // in the tariff itself rather than in its age.
  ecg::PriceInterval duplicate = scenario.evidence.tariff->value.prices.front();
  scenario.evidence.tariff->value.prices.push_back(duplicate);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 11), scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceConflicting));
}

ECG_TEST("freshness.refusal_outranks_indeterminacy") {
  // A definitive violation on fresh evidence stays definitive when an unrelated
  // input is stale, so Refused must win over Indeterminate. The service catalog
  // below is fresh and prohibits the curtailment; the incident feed is stale.
  Scenario scenario = MakeScenario();
  scenario.evidence.incident->meta.observed_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() - 7200LL * 1000000LL);
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 12, RequestKind::kCurtailServiceClass, Kw(3000), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.critical");
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
  ECG_CHECK(HasReason(decision, ReasonCode::kServiceCurtailmentProhibited));
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceStale));
  ECG_CHECK_EQ(decision.outcome, Outcome::kRefused);
}

ECG_TEST("freshness.category_age_limits_are_independent") {
  Scenario scenario = MakeScenario();
  // Incident evidence has the tightest default limit; aging it past its own
  // limit must not be excused by the much looser tariff limit.
  scenario.evidence.incident->meta.observed_at =
      ecg::UtcInstant::FromRaw(scenario.now.raw() - 3600LL * 1000000LL);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), EveningShift(scenario, 13), scenario.evidence, scenario.now);
  ECG_CHECK(HasReason(decision, ReasonCode::kEvidenceStale));
  bool named = false;
  for (const ecg::Reason& reason : decision.reasons) {
    if (reason.code == ReasonCode::kEvidenceStale && reason.subject == "evidence.incident") {
      named = true;
    }
  }
  ECG_CHECK(named);
}
