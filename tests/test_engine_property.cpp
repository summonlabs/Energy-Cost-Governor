// Seeded randomised and adversarial testing.
//
// These tests exist because the interesting failures are in the combinations
// nobody wrote a hand-written case for: an action that is simultaneously
// uneconomic, over the reserve floor, and inside a demand window, evaluated
// against evidence that is fresh in one category and stale in another.

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

const RequestKind kAllKinds[] = {
    RequestKind::kShiftFlexibleLoad, RequestKind::kCurtailServiceClass,
    RequestKind::kDeferBatchWork,    RequestKind::kChargeStorage,
    RequestKind::kDischargeStorage,  RequestKind::kPreCooling,
    RequestKind::kReserveRestoration};

const char* ServiceClassFor(RequestKind kind) {
  switch (kind) {
    case RequestKind::kShiftFlexibleLoad:
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kDeferBatchWork:
    case RequestKind::kPreCooling:
      return "vm.flex";
    default:
      return "";
  }
}

/// True when the decision's trace is in canonical order and free of duplicates.
bool TraceIsCanonical(const ecg::Decision& decision) {
  for (std::size_t i = 1; i < decision.reasons.size(); ++i) {
    if (ecg::ReasonLess(decision.reasons[i], decision.reasons[i - 1])) {
      return false;
    }
    if (decision.reasons[i].code == decision.reasons[i - 1].code &&
        decision.reasons[i].subject == decision.reasons[i - 1].subject) {
      return false;
    }
  }
  return true;
}

}  // namespace

ECG_TEST("property.randomised_requests_hold_every_invariant") {
  // Fixed seeds: a failure here is reproducible forever.
  const std::uint64_t seeds[] = {1, 0xC0FFEE, 20260302, 0xDEADBEEF, 99991};
  int evaluated = 0;
  for (const std::uint64_t seed : seeds) {
    ecgtest::Random random(seed);
    for (int iteration = 0; iteration < 400; ++iteration) {
      Scenario scenario = MakeScenario();

      // Perturb the evidence within its own plausible envelope.
      if (random.Below(4) == 0) {
        scenario.evidence.demand.reset();
      }
      if (random.Below(4) == 0) {
        scenario.evidence.tariff->value.prices.pop_back();
      }
      if (random.Below(6) == 0) {
        scenario.evidence.reserve->meta.observed_at =
            ecg::UtcInstant::FromRaw(scenario.now.raw() - random.Range(0, 4000) * 1000000LL);
      }
      if (random.Below(6) == 0) {
        scenario.evidence.incident->value.severity =
            static_cast<ecg::IncidentSeverity>(random.Below(4));
      }
      if (random.Below(6) == 0) {
        scenario.evidence.risk->value.posture = static_cast<ecg::RiskPosture>(random.Below(3));
      }
      scenario.evidence.risk->value.minimum_dwell = Secs(random.Range(0, 3600));

      const RequestKind kind = kAllKinds[random.Below(7)];
      const std::int64_t magnitude_kw = random.Range(1, 3000);
      const std::int64_t duration_s = random.Range(1, 4 * 3600);
      const std::int64_t start_offset = random.Range(0, 10 * 3600);
      const ecg::UtcInstant begin =
          ecg::UtcInstant::FromRaw(scenario.now.raw() + start_offset * 1000000LL);
      const ecg::UtcInstant end = ecg::UtcInstant::FromRaw(begin.raw() + duration_s * 1000000LL);
      const auto window = ecg::Interval::Make(begin, end);
      ECG_REQUIRE(window.ok());

      ecg::DecisionRequest request =
          MakeRequest(scenario, static_cast<std::uint64_t>(iteration) + 1000, kind,
                      ecg::PowerKw::FromRaw(magnitude_kw * 1000), ecg::DurationSec::FromRaw(duration_s),
                      window.value(), ServiceClassFor(kind));
      request.rationale = static_cast<ecg::Rationale>(random.Below(5));

      const auto engine = ecg::GovernorEngine::Make(scenario.policy);
      ECG_REQUIRE(engine.ok());
      const auto result = engine.value().Evaluate(request, scenario.evidence, scenario.now);
      if (!result.ok()) {
        ECG_CHECK_MSG(false, ecg::ErrorCodeName(result.error().code()));
        continue;
      }
      const ecg::Decision& decision = result.value();
      ++evaluated;

      // 1. The trace is canonical.
      ECG_CHECK_MSG(TraceIsCanonical(decision), std::to_string(seed) + "/" + std::to_string(iteration));

      // 2. Deferred always names when to ask again, and never carries an intent.
      if (decision.outcome == Outcome::kDeferred) {
        ECG_CHECK(decision.reconsider_at.has_value());
        ECG_CHECK(!decision.intent.has_value());
      }

      // 3. Allowed implies an intent whose bounds never exceed the request.
      if (decision.outcome == Outcome::kAllowed) {
        ECG_REQUIRE(decision.intent.has_value());
        ECG_CHECK(decision.intent->magnitude_limit_kw <= request.magnitude_kw);
        ECG_CHECK(decision.intent->duration_limit <= request.duration);
        ECG_CHECK(decision.intent->magnitude_limit_kw <= scenario.policy.max_intent_magnitude_kw);
        ECG_CHECK(decision.intent->duration_limit <= scenario.policy.max_intent_duration);
        ECG_CHECK(decision.intent->advisory_only);
      } else {
        ECG_CHECK(!decision.intent.has_value());
      }

      // 4. Every non-Allowed outcome explains itself at the right volume: a
      //    refusal or an inability to decide is a blocker, a deferral is a
      //    notice, and neither can be empty.
      if (decision.outcome == Outcome::kRefused ||
          decision.outcome == Outcome::kIndeterminate) {
        bool blocker = false;
        for (const ecg::Reason& reason : decision.reasons) {
          if (reason.severity == ecg::ReasonSeverity::kBlocker) {
            blocker = true;
          }
        }
        ECG_CHECK(blocker);
      } else if (decision.outcome == Outcome::kDeferred) {
        bool notice = false;
        for (const ecg::Reason& reason : decision.reasons) {
          if (reason.severity == ecg::ReasonSeverity::kNotice) {
            notice = true;
          }
        }
        ECG_CHECK(notice);
      }

      // 5. Replay determinism: the same inputs produce the same identity.
      const ecg::Decision replay =
          EvaluateOrDie(engine.value(), request, scenario.evidence, scenario.now);
      ECG_CHECK(replay.digest == decision.digest);
      ECG_CHECK_EQ(ecg::ToJson(replay).Dump(), ecg::ToJson(decision).Dump());
    }
  }
  ECG_CHECK_MSG(evaluated > 1500, std::to_string(evaluated));
}

/// Clears one evidence category by index.
void RemoveCategory(Scenario* scenario, int index) {
  switch (index) {
    case 0: scenario->evidence.tariff.reset(); break;
    case 1: scenario->evidence.demand.reset(); break;
    case 2: scenario->evidence.reserve.reset(); break;
    case 3: scenario->evidence.efficiency.reset(); break;
    case 4: scenario->evidence.services.reset(); break;
    case 5: scenario->evidence.capacity.reset(); break;
    case 6: scenario->evidence.risk.reset(); break;
    case 7: scenario->evidence.incident.reset(); break;
    default: scenario->evidence.zone.reset(); break;
  }
}

/// The evidence categories each request kind is documented to require. A kind
/// that needs no tariff can legitimately be authorised while the tariff feed is
/// down; what must never happen is authorisation while a required input is
/// absent.
std::vector<int> RequiredCategoriesFor(RequestKind kind) {
  switch (kind) {
    case RequestKind::kShiftFlexibleLoad:
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kPreCooling:
      return {0, 2, 4, 5, 6, 7};
    case RequestKind::kDeferBatchWork:
      return {0, 4, 6, 7};
    case RequestKind::kChargeStorage:
    case RequestKind::kDischargeStorage:
      return {0, 2, 3, 5, 6, 7};
    case RequestKind::kReserveRestoration:
      return {2, 5, 6, 7};
  }
  return {};
}

ECG_TEST("property.missing_evidence_never_produces_an_allowed_outcome") {
  ecgtest::Random random(0x5EED);
  for (int iteration = 0; iteration < 300; ++iteration) {
    Scenario scenario = MakeScenario();
    const RequestKind kind = kAllKinds[random.Below(7)];
    // Remove exactly one category that this kind actually depends on.
    const std::vector<int> required = RequiredCategoriesFor(kind);
    ECG_REQUIRE(!required.empty());
    RemoveCategory(&scenario, required[static_cast<std::size_t>(random.Below(required.size()))]);
    const auto window = Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z");
    const ecg::DecisionRequest request = MakeRequest(
        scenario, 5000 + static_cast<std::uint64_t>(iteration), kind, Kw(500), Secs(1800), window,
        ServiceClassFor(kind));
    const auto engine = ecg::GovernorEngine::Make(scenario.policy);
    ECG_REQUIRE(engine.ok());
    const auto result = engine.value().Evaluate(request, scenario.evidence, scenario.now);
    ECG_REQUIRE(result.ok());
    ECG_CHECK_MSG(result.value().outcome != Outcome::kAllowed,
                  ecg::RequestKindName(kind));
  }
}

ECG_TEST("property.extreme_price_magnitudes_are_reported_not_wrapped") {
  Scenario scenario = MakeScenario();
  // The largest representable price on a window large enough that the exact
  // product cannot fit in the money domain. The engine must report the numeric
  // limit instead of wrapping into a plausible-looking number.
  for (ecg::PriceInterval& interval : scenario.evidence.tariff->value.prices) {
    interval.price = ecg::PriceMicrosPerKwh::FromRaw(1000000000000LL);
  }
  // A 20 MW action for an hour, priced at the largest representable price, makes
  // the exact cost exceed the money domain. The capacity and service terms are
  // widened so that nothing else refuses the request first and the numeric limit
  // is what the test actually observes.
  scenario.policy.require_positive_economic_benefit = true;
  scenario.policy.max_intent_magnitude_kw = Kw(20000);
  scenario.evidence.capacity->value.flexible_load_kw = Kw(20000);
  scenario.evidence.capacity->value.shiftable_headroom_kw = Kw(20000);
  scenario.evidence.services->value.classes[2].nominal_load_kw = Kw(20000);
  scenario.evidence.services->value.classes[2].max_curtailment_ratio = Ppm(1000000);
  scenario.policy.max_service_curtailment_ratio = Ppm(1000000);
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 6001, RequestKind::kCurtailServiceClass, Kw(20000), Secs(3600),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:30:00.000000Z"), "vm.flex");
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const auto result = engine.value().Evaluate(request, scenario.evidence, scenario.now);
  if (!result.ok()) {
    ECG_CHECK(result.error().code() == ecg::ErrorCode::kNumericOverflow ||
              result.error().code() == ecg::ErrorCode::kOutOfRange);
  } else {
    ECG_CHECK(HasReason(result.value(), ReasonCode::kEconomicsOverflow));
    ECG_CHECK_EQ(result.value().outcome, Outcome::kIndeterminate);
  }
}

ECG_TEST("property.malformed_evidence_is_rejected_before_evaluation") {
  Scenario scenario = MakeScenario();
  // A reserve payload whose ratio floor is outside [0, 1].
  scenario.evidence.reserve->value.reserve_floor_ratio = Ppm(2000000);
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 6002, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  ECG_CHECK_ERR(engine.value().Evaluate(request, scenario.evidence, scenario.now),
                ecg::ErrorCode::kOutOfRange);
}

ECG_TEST("property.risk_evidence_without_any_constraint_is_rejected") {
  Scenario scenario = MakeScenario();
  scenario.evidence.risk->value.max_ramp_kw_per_min.reset();
  scenario.evidence.risk->value.max_deferral.reset();
  scenario.evidence.risk->value.minimum_dwell.reset();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 6003, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  // An empty risk feed must not read as "no constraints apply".
  ECG_CHECK_ERR(engine.value().Evaluate(request, scenario.evidence, scenario.now),
                ecg::ErrorCode::kInvalidArgument);
}

ECG_TEST("property.duplicate_service_classes_are_rejected") {
  Scenario scenario = MakeScenario();
  scenario.evidence.services->value.classes.push_back(
      scenario.evidence.services->value.classes.front());
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 6004, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  ECG_CHECK_ERR(engine.value().Evaluate(request, scenario.evidence, scenario.now),
                ecg::ErrorCode::kAlreadyExists);
}

ECG_TEST("property.invalid_policy_is_rejected_at_construction") {
  ecg::PolicySet policy = ecg::DefaultPolicy();
  policy.max_intent_magnitude_kw = ecg::PowerKw::FromRaw(0);
  ECG_CHECK_ERR(ecg::GovernorEngine::Make(policy), ecg::ErrorCode::kInvalidArgument);

  ecg::PolicySet second = ecg::DefaultPolicy();
  second.decision_horizon = Secs(0);
  ECG_CHECK_ERR(ecg::GovernorEngine::Make(second), ecg::ErrorCode::kInvalidArgument);

  ecg::PolicySet third = ecg::DefaultPolicy();
  third.reserve_safety_margin = Ppm(2000000);
  ECG_CHECK_ERR(ecg::GovernorEngine::Make(third), ecg::ErrorCode::kOutOfRange);
}

ECG_TEST("property.reason_trace_stays_within_the_policy_bound") {
  Scenario scenario = MakeScenario();
  scenario.policy.max_reasons = 1;
  scenario.evidence.tariff.reset();
  scenario.evidence.reserve.reset();
  scenario.evidence.capacity.reset();
  scenario.evidence.risk.reset();
  scenario.evidence.incident.reset();
  scenario.evidence.services.reset();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 6005, RequestKind::kShiftFlexibleLoad, Kw(500), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  // The trace cannot be silently truncated: exceeding the bound is an error.
  ECG_CHECK_ERR(engine.value().Evaluate(request, scenario.evidence, scenario.now),
                ecg::ErrorCode::kResourceLimitExceeded);
}

ECG_TEST("property.adversarial_json_documents_are_rejected") {
  const char* documents[] = {
      "",
      "   ",
      "{",
      "[1,,2]",
      "[1,2",
      "{\"a\":1,}",
      "{\"a\":1}{\"b\":2}",
      "{\"a\":01}",
      "{\"a\":1e5}",
      "{\"a\":\"\\uD800\"}",
      "{\"a\":\"\\uDC00\"}",
      "{\"a\":\"\\x41\"}",
      "{\"a\":\"\t\"}",
      "{\"a\":[1,2,3,]}",
      "{\"a\":{\"b\":{\"c\":{",
  };
  for (const char* document : documents) {
    const auto parsed = ecg::JsonValue::Parse(document);
    ECG_CHECK_MSG(!parsed.ok(), document);
  }
  const auto valid = ecg::JsonValue::Parse("{\"a\":[1,2,3],\"b\":{\"c\":null}}");
  ECG_CHECK(valid.ok());
}

ECG_TEST("property.deeply_nested_json_is_refused_without_stack_exhaustion") {
  std::string document;
  const std::size_t depth = ecg::JsonValue::kMaxDepth + 16;
  for (std::size_t i = 0; i < depth; ++i) {
    document += "[";
  }
  for (std::size_t i = 0; i < depth; ++i) {
    document += "]";
  }
  const auto parsed = ecg::JsonValue::Parse(document);
  ECG_CHECK(!parsed.ok());
}
