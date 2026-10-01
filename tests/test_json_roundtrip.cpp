// Serialisation round-trips and the self-verifying decision encoding.

#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json.hpp"
#include "ecg/json_io.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

std::string DumpAndReparse(const ecg::JsonValue& value) {
  const std::string text = value.Dump();
  const auto parsed = ecg::JsonValue::Parse(text);
  if (!parsed.ok()) {
    return {};
  }
  return parsed.value().Dump();
}

}  // namespace

ECG_TEST("json_io.tariff_round_trips_exactly") {
  const Scenario scenario = MakeScenario();
  const ecg::JsonValue json = ecg::ToJson(scenario.evidence.tariff->value);
  const auto parsed = ecg::ParseTariff(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
  ECG_CHECK_EQ(parsed.value().id.value(), scenario.evidence.tariff->value.id.value());
  ECG_CHECK_EQ(parsed.value().prices.size(), scenario.evidence.tariff->value.prices.size());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), ecg::ToJson(scenario.evidence.tariff->value).Dump());
}

ECG_TEST("json_io.evidence_set_round_trips_with_metadata_intact") {
  const Scenario scenario = MakeScenario();
  const ecg::JsonValue json = ecg::ToJson(scenario.evidence);
  const auto parsed = ecg::ParseEvidenceSet(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK(parsed.value().epoch == scenario.evidence.epoch);
  ECG_REQUIRE(parsed.value().reserve.has_value());
  ECG_CHECK(parsed.value().reserve->meta.generation == scenario.evidence.reserve->meta.generation);
  ECG_CHECK_EQ(parsed.value().reserve->meta.provenance, scenario.evidence.reserve->meta.provenance);
  ECG_CHECK(parsed.value().reserve->meta.observed_at == scenario.evidence.reserve->meta.observed_at);
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
}

ECG_TEST("json_io.absent_evidence_is_an_explicit_null") {
  const Scenario scenario = MakeScenario();
  ecg::EvidenceSet sparse = scenario.evidence;
  sparse.tariff.reset();
  sparse.zone.reset();
  const std::string text = ecg::ToJson(sparse).Dump();
  ECG_CHECK(text.find("\"tariff\":null") != std::string::npos);
  const auto parsed = ecg::ParseEvidenceSet(ecg::JsonValue::Parse(text).value());
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK(!parsed.value().tariff.has_value());
  ECG_CHECK(!parsed.value().zone.has_value());
  ECG_CHECK(parsed.value().reserve.has_value());
}

ECG_TEST("json_io.policy_round_trips_exactly") {
  ecg::PolicySet policy = ecg::DefaultPolicy();
  policy.generation = ecg::PolicyGeneration::FromRaw(9);
  policy.max_reasons = 128;
  policy.defer_early_demand_requests = false;
  policy.reserve_safety_margin = Ppm(123456);
  const ecg::JsonValue json = ecg::ToJson(policy);
  const auto parsed = ecg::ParsePolicySet(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
  ECG_CHECK_EQ(parsed.value().max_reasons, policy.max_reasons);
  ECG_CHECK_EQ(parsed.value().defer_early_demand_requests, false);
  ECG_CHECK(parsed.value().reserve_safety_margin == policy.reserve_safety_margin);
}

ECG_TEST("json_io.request_round_trips_exactly") {
  const Scenario scenario = MakeScenario();
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 77, ecg::RequestKind::kCurtailServiceClass, Kw(750), Secs(1200),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T18:50:00.000000Z"), "vm.batch");
  const ecg::JsonValue json = ecg::ToJson(request);
  const auto parsed = ecg::ParseDecisionRequest(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
  ECG_CHECK(parsed.value().request_id == request.request_id);
  ECG_CHECK(parsed.value().magnitude_kw == request.magnitude_kw);
}

ECG_TEST("json_io.decision_round_trips_and_verifies_its_digest") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 78, ecg::RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
  const ecg::JsonValue json = ecg::ToJson(decision);
  const auto parsed = ecg::ParseDecision(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK(parsed.value().digest == decision.digest);
  ECG_CHECK(parsed.value().id == decision.id);
  ECG_CHECK_EQ(parsed.value().outcome, decision.outcome);
  ECG_CHECK_EQ(parsed.value().reasons.size(), decision.reasons.size());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
  if (decision.intent.has_value()) {
    ECG_REQUIRE(parsed.value().intent.has_value());
    ECG_CHECK(parsed.value().intent->id == decision.intent->id);
    ECG_CHECK(parsed.value().intent->advisory_only);
  }
}

ECG_TEST("json_io.tampered_decision_is_refused_by_the_digest_check") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 79, ecg::RequestKind::kCurtailServiceClass, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
  const std::string honest = ecg::ToJson(decision).Dump();

  // Flip a detail string in the trace without touching the digest.
  std::string tampered = honest;
  const std::size_t position = tampered.find("reserve after the action");
  ECG_REQUIRE(position != std::string::npos);
  tampered.replace(position, 5, "XXXXX");
  const auto parsed = ecg::JsonValue::Parse(tampered);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_ERR(ecg::ParseDecision(parsed.value()), ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("json_io.tampered_outcome_is_refused_by_the_digest_check") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 80, ecg::RequestKind::kChargeStorage, Kw(900), Secs(1800),
                  Span("2026-03-02T12:10:00.000000Z", "2026-03-02T12:40:00.000000Z"), "");
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
  ECG_CHECK_EQ(decision.outcome, ecg::Outcome::kRefused);
  std::string text = ecg::ToJson(decision).Dump();
  const std::size_t position = text.find("\"outcome\":\"refused\"");
  ECG_REQUIRE(position != std::string::npos);
  text.replace(position, 19, "\"outcome\":\"allowed\"");
  const auto parsed = ecg::JsonValue::Parse(text);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_ERR(ecg::ParseDecision(parsed.value()), ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("json_io.unknown_members_are_rejected") {
  const Scenario scenario = MakeScenario();
  ecg::JsonValue json = ecg::ToJson(scenario.evidence);
  ECG_REQUIRE(json.is_object());
  ECG_CHECK_OK(json.Set("not_a_real_member", ecg::JsonValue::Bool(true)));
  ECG_CHECK_ERR(ecg::ParseEvidenceSet(json), ecg::ErrorCode::kUnsupportedValue);
}

ECG_TEST("json_io.state_document_round_trips") {
  const Scenario scenario = MakeScenario();
  ecg::StateDocument state;
  state.epoch = scenario.policy.epoch;
  state.policy = scenario.policy;
  state.evidence = scenario.evidence;
  state.updated_at = scenario.now;
  const ecg::JsonValue json = ecg::ToJson(state);
  const auto parsed = ecg::ParseStateDocument(json);
  ECG_REQUIRE(parsed.ok());
  ECG_CHECK_EQ(ecg::ToJson(parsed.value()).Dump(), json.Dump());
}

ECG_TEST("json_io.marking_recovered_touches_every_present_payload") {
  const Scenario scenario = MakeScenario();
  ecg::EvidenceSet evidence = scenario.evidence;
  ecg::MarkEvidenceRecovered(&evidence);
  ECG_REQUIRE(evidence.tariff.has_value());
  ECG_REQUIRE(evidence.demand.has_value());
  ECG_REQUIRE(evidence.reserve.has_value());
  ECG_REQUIRE(evidence.efficiency.has_value());
  ECG_REQUIRE(evidence.services.has_value());
  ECG_REQUIRE(evidence.capacity.has_value());
  ECG_REQUIRE(evidence.risk.has_value());
  ECG_REQUIRE(evidence.incident.has_value());
  ECG_REQUIRE(evidence.zone.has_value());
  ECG_CHECK_EQ(evidence.tariff->meta.provenance, ecg::Provenance::kRecoveredPersistence);
  ECG_CHECK_EQ(evidence.zone->meta.provenance, ecg::Provenance::kRecoveredPersistence);
}

ECG_TEST("json_io.formatting_is_stable_across_calls") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 81, ecg::RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
  ECG_CHECK_EQ(ecg::ToJson(decision).Dump(), ecg::ToJson(decision).Dump());
  ECG_CHECK_EQ(DumpAndReparse(ecg::ToJson(decision)), ecg::ToJson(decision).Dump());

  const std::string report = ecg::FormatDecisionReport(decision);
  ECG_CHECK(report.find(ecg::OutcomeName(decision.outcome)) != std::string::npos);
  for (const ecg::Reason& reason : decision.reasons) {
    ECG_CHECK_MSG(report.find(ecg::ReasonCodeName(reason.code)) != std::string::npos,
                  ecg::ReasonCodeName(reason.code));
  }
}
