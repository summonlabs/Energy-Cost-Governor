// Downstream consumer proof for Energy-Cost-Governor.
//
// This program is built OUT OF TREE, against an INSTALLED package, by its own
// CMake project. It deliberately uses nothing but the installed public headers
// and the exported target ecg::core: if a symbol it needs is not in the public
// API, this file will not compile, which is exactly the point.
//
// SYNTHETIC INPUT NOTICE
// ----------------------
// The evidence below is a hand-written model, published with
// ecg::AuthorityKind::kSyntheticModel and ecg::Provenance::kSyntheticModel. It is
// not facility telemetry, not a meter reading, and not BMS/DCIM data, and it
// must never be presented as any of those.
//
// What it proves:
//   1. A downstream project can find, link, and drive the installed runtime
//      through a complete decision.
//   2. The decision is exactly the one the modelled evidence implies: this
//      program asserts the expected outcome instead of printing whatever came
//      back.
//   3. The "missing or stale evidence never becomes permissive" rule holds for
//      a downstream consumer: the same request against a stale variant of the
//      same evidence must be Indeterminate, never Allowed.
//
// Exit code 0 means every assertion held. Any other exit code is a failure.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/engine.hpp"
#include "ecg/evidence.hpp"
#include "ecg/policy.hpp"
#include "ecg/request.hpp"
#include "ecg/time.hpp"
#include "ecg/version.hpp"

namespace {

constexpr std::int64_t kRawPerUnit = 1000;
constexpr std::int64_t kMicrosPerSecond = 1000000;

[[nodiscard]] ecg::PowerKw Kw(std::int64_t kilowatts) noexcept {
  return ecg::PowerKw::FromRaw(kilowatts * kRawPerUnit);
}
[[nodiscard]] ecg::PowerKwPerMin KwPerMin(std::int64_t kilowatts_per_minute) noexcept {
  return ecg::PowerKwPerMin::FromRaw(kilowatts_per_minute * kRawPerUnit);
}
[[nodiscard]] ecg::PriceMicrosPerKwh MicrosPerKwh(std::int64_t micros) noexcept {
  return ecg::PriceMicrosPerKwh::FromRaw(micros);
}
[[nodiscard]] ecg::RatioPpm Ratio(std::int64_t ppm) noexcept { return ecg::RatioPpm::FromRaw(ppm); }
[[nodiscard]] ecg::DurationSec Secs(std::int64_t seconds) noexcept {
  return ecg::DurationSec::FromRaw(seconds);
}
[[nodiscard]] ecg::UtcInstant Ago(ecg::UtcInstant now, std::int64_t seconds) noexcept {
  return ecg::UtcInstant::FromRaw(now.raw() - seconds * kMicrosPerSecond);
}

[[nodiscard]] ecg::EvidenceMeta SyntheticMeta(const char* source, ecg::UtcInstant observed_at,
                                              std::uint64_t generation, ecg::Epoch epoch) {
  ecg::EvidenceMeta meta;
  meta.authority = ecg::AuthorityKind::kSyntheticModel;
  meta.source = ecg::SourceId::FromTrusted(source);
  meta.generation = ecg::EvidenceGeneration::FromRaw(generation);
  meta.epoch = epoch;
  meta.observed_at = observed_at;
  meta.published_at = observed_at;
  meta.payload_version = 1;
  meta.provenance = ecg::Provenance::kSyntheticModel;
  return meta;
}

template <class Payload>
[[nodiscard]] ecg::Evidence<Payload> Wrap(Payload payload, const char* source,
                                          ecg::UtcInstant observed_at, std::uint64_t generation,
                                          ecg::Epoch epoch) {
  ecg::Evidence<Payload> evidence;
  evidence.meta = SyntheticMeta(source, observed_at, generation, epoch);
  evidence.value = std::move(payload);
  return evidence;
}

[[nodiscard]] ecg::Result<ecg::Interval> MakeInterval(std::string_view begin_text,
                                                      std::string_view end_text) {
  ECG_TRY(begin, ecg::ParseRfc3339(begin_text));
  ECG_TRY(end, ecg::ParseRfc3339(end_text));
  ECG_TRY(window, ecg::Interval::Make(begin, end));
  return window;
}

[[nodiscard]] int Fail(const std::string& message) {
  std::fprintf(stderr, "find_package_consumer: FAIL: %s\n", message.c_str());
  return 1;
}

// A deliberately small modelled world: one day, three price blocks, one service
// class. The request below asks to move 500 kW of flexible load out of the
// afternoon peak, and every published term permits exactly that.
[[nodiscard]] ecg::Result<ecg::EvidenceSet> BuildSyntheticEvidence(ecg::UtcInstant now,
                                                                   ecg::Epoch epoch) {
  ecg::EvidenceSet evidence;
  evidence.epoch = epoch;
  const ecg::UtcInstant observed = Ago(now, 60);

  ECG_TRY(day, MakeInterval("2026-03-02T00:00:00.000000Z", "2026-03-03T00:00:00.000000Z"));
  ECG_TRY(cheap, MakeInterval("2026-03-02T00:00:00.000000Z", "2026-03-02T12:00:00.000000Z"));
  ECG_TRY(peak, MakeInterval("2026-03-02T12:00:00.000000Z", "2026-03-02T18:00:00.000000Z"));
  ECG_TRY(evening, MakeInterval("2026-03-02T18:00:00.000000Z", "2026-03-03T00:00:00.000000Z"));

  ecg::Tariff tariff;
  tariff.id = ecg::TariffId::FromTrusted("synthetic.consumer.tariff");
  tariff.revision = ecg::TariffRevision::FromRaw(1);
  tariff.epoch = epoch;
  tariff.currency = "USD";
  tariff.published_at = Ago(now, 3600);
  tariff.validity = day;
  tariff.prices.push_back(ecg::PriceInterval{cheap, MicrosPerKwh(90000)});
  tariff.prices.push_back(ecg::PriceInterval{peak, MicrosPerKwh(300000)});
  tariff.prices.push_back(ecg::PriceInterval{evening, MicrosPerKwh(110000)});
  // No demand-charge rule: this request is a price-arbitrage load shift, and the
  // modelled tariff publishes no peak rule. The engine does not invent one.
  evidence.tariff = Wrap(tariff, "synthetic.consumer.tariff", observed, 1, epoch);

  ecg::ReserveState reserve;
  reserve.current_reserve_kw = Kw(900);
  reserve.reserve_floor_kw = Kw(400);
  reserve.capacity_kw = Kw(5000);
  reserve.reserve_floor_ratio = Ratio(100000);
  evidence.reserve = Wrap(reserve, "synthetic.consumer.reserve", observed, 2, epoch);

  ecg::CapacityState capacity;
  capacity.flexible_load_kw = Kw(2000);
  capacity.shiftable_headroom_kw = Kw(1200);
  capacity.storage_charge_kw = Kw(500);
  capacity.storage_discharge_kw = Kw(500);
  capacity.storage_usable_energy_kwh = ecg::EnergyKwh::FromRaw(1000 * kRawPerUnit);
  evidence.capacity = Wrap(capacity, "synthetic.consumer.capacity", observed, 3, epoch);

  ecg::ServiceCatalog catalog;
  ecg::ServiceClassTerms batch;
  batch.id = ecg::ServiceClassId::FromTrusted("vm.batch");
  batch.permission = ecg::CurtailmentPermission::kUnrestricted;
  batch.max_curtailment_ratio = Ratio(600000);
  batch.max_curtailment_duration = Secs(7200);
  batch.minimum_notice = Secs(0);
  batch.priority = 10;
  batch.nominal_load_kw = Kw(4000);
  catalog.classes.push_back(batch);
  evidence.services = Wrap(catalog, "synthetic.consumer.services", observed, 4, epoch);

  ecg::RiskState risk;
  risk.posture = ecg::RiskPosture::kNormal;
  risk.max_ramp_kw_per_min = KwPerMin(100);
  risk.minimum_dwell = Secs(900);
  risk.last_change_at = Ago(now, 7200);
  evidence.risk = Wrap(risk, "synthetic.consumer.risk", observed, 5, epoch);

  ecg::IncidentState incident;
  incident.severity = ecg::IncidentSeverity::kNone;
  incident.declared_at = Ago(now, 21600);
  evidence.incident = Wrap(incident, "synthetic.consumer.incident", observed, 6, epoch);

  return evidence;
}

}  // namespace

namespace {

[[nodiscard]] bool HasReason(const ecg::Decision& decision, ecg::ReasonCode code) {
  for (const ecg::Reason& reason : decision.reasons) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

void PrintDecision(const char* label, const ecg::Decision& decision) {
  std::printf("%s\n", label);
  std::printf("  outcome      : %s\n", ecg::OutcomeName(decision.outcome));
  std::printf("  decision id  : %s\n", decision.id.ToString().c_str());
  std::printf("  request id   : %s\n", decision.request_id.ToString().c_str());
  std::printf("  kind         : %s\n", ecg::RequestKindName(decision.kind));
  std::printf("  reason codes : %zu\n", decision.reasons.size());
  for (const ecg::Reason& reason : decision.reasons) {
    std::printf("      %-28s %-7s %s\n", ecg::ReasonCodeName(reason.code),
                ecg::ReasonSeverityName(reason.severity), reason.detail.c_str());
  }
  if (decision.intent.has_value()) {
    std::printf("  intent       : %s kW for %s s, advisory_only=%s, expires %s\n",
                ecg::FormatQuantity(decision.intent->magnitude_limit_kw).c_str(),
                ecg::FormatQuantity(decision.intent->duration_limit).c_str(),
                decision.intent->advisory_only ? "true" : "false",
                ecg::FormatRfc3339(decision.intent->expires_at).c_str());
  } else {
    std::printf("  intent       : none (an intent is emitted only for an authorisation)\n");
  }
  std::printf("  economics    : evaluated=%s favorable=%s savings=%s %s\n",
              decision.economics.evaluated ? "true" : "false",
              decision.economics.favorable ? "true" : "false",
              ecg::FormatQuantity(decision.economics.gross_savings).c_str(),
              decision.economics.evaluated ? "(modelled tariff)" : "(not evaluated)");
  std::printf("\n");
}

}  // namespace

int main() {
  std::printf("=== Energy-Cost-Governor downstream consumer (find_package -> ecg::core) ===\n");
  std::printf("library version : %s\n", ecg::VersionString());
  std::printf("evidence        : SYNTHETIC MODEL INPUT -- hand-written, not facility telemetry\n");
  std::printf("                  authority=synthetic_model, provenance=synthetic_model\n\n");

  const ecg::Result<ecg::UtcInstant> now_result = ecg::ParseRfc3339("2026-03-02T13:30:00.000000Z");
  if (!now_result.ok()) {
    return Fail("cannot parse the modelled instant: " + now_result.error().ToString());
  }
  const ecg::UtcInstant now = now_result.value();

  const ecg::PolicySet policy = ecg::DefaultPolicy();
  const ecg::Result<ecg::GovernorEngine> engine_result = ecg::GovernorEngine::Make(policy);
  if (!engine_result.ok()) {
    return Fail("GovernorEngine::Make rejected the default policy: " +
                engine_result.error().ToString());
  }
  const ecg::GovernorEngine& engine = engine_result.value();

  const ecg::Result<ecg::EvidenceSet> evidence_result = BuildSyntheticEvidence(now, policy.epoch);
  if (!evidence_result.ok()) {
    return Fail("cannot build the synthetic evidence: " + evidence_result.error().ToString());
  }
  const ecg::EvidenceSet& evidence = evidence_result.value();

  const ecg::Result<ecg::Interval> window_result =
      MakeInterval("2026-03-02T14:00:00.000000Z", "2026-03-02T15:00:00.000000Z");
  if (!window_result.ok()) {
    return Fail("cannot build the desired window: " + window_result.error().ToString());
  }

  ecg::DecisionRequest request;
  request.request_id = ecg::RequestId::FromRaw(1);
  request.client_id = ecg::ClientId::FromTrusted("downstream.consumer");
  request.epoch = policy.epoch;
  request.policy_generation = policy.generation;
  request.kind = ecg::RequestKind::kShiftFlexibleLoad;
  request.service_class = ecg::ServiceClassId::FromTrusted("vm.batch");
  request.magnitude_kw = Kw(500);
  request.duration = Secs(3600);
  request.desired_window = window_result.value();
  request.rationale = ecg::Rationale::kPriceArbitrage;
  request.requested_at = now;
  request.attempt = 0;

  // -------------------------------------------------------------------------
  // Case 1: fresh evidence. The modelled terms permit the shift, and the
  // modelled tariff makes it worth taking, so the only correct answer is
  // Allowed. This is asserted, not merely printed.
  // -------------------------------------------------------------------------
  const ecg::Result<ecg::Decision> decision_result = engine.Evaluate(request, evidence, now);
  if (!decision_result.ok()) {
    return Fail("Evaluated failed on fresh evidence: " + decision_result.error().ToString());
  }
  const ecg::Decision& decision = decision_result.value();
  PrintDecision("case 1: fresh synthetic evidence", decision);
  if (decision.outcome != ecg::Outcome::kAllowed) {
    return Fail(std::string("the modelled evidence implies 'allowed' but the engine answered '") +
                ecg::OutcomeName(decision.outcome) + "'");
  }
  if (!decision.intent.has_value()) {
    return Fail("an 'allowed' decision must carry a bounded intent, and this one carries none");
  }
  if (!decision.intent->advisory_only) {
    return Fail("the emitted intent claims to be more than advisory; that is a contract violation");
  }
  if (!HasReason(decision, ecg::ReasonCode::kEconomicsFavorable)) {
    return Fail("the 'allowed' decision does not name its economic basis");
  }

  // -------------------------------------------------------------------------
  // Case 2: the same request against the same evidence, except that the reserve
  // observation is one hour old and the policy allows five minutes. The answer
  // must be Indeterminate: stale evidence is never permissive, and this is the
  // rule a downstream consumer is most likely to get wrong by treating a
  // missing or aged input as "no constraint".
  // -------------------------------------------------------------------------
  ecg::EvidenceSet stale = evidence;
  if (!stale.reserve.has_value()) {
    return Fail("the stale variant lost its reserve observation");
  }
  stale.reserve->meta.observed_at = Ago(now, 3600);
  stale.reserve->meta.published_at = stale.reserve->meta.observed_at;

  const ecg::Result<ecg::Decision> stale_result = engine.Evaluate(request, stale, now);
  if (!stale_result.ok()) {
    return Fail("Evaluate failed on stale evidence: " + stale_result.error().ToString());
  }
  const ecg::Decision& stale_decision = stale_result.value();
  PrintDecision("case 2: identical request, reserve observation aged past its policy limit",
                stale_decision);
  if (stale_decision.outcome != ecg::Outcome::kIndeterminate) {
    return Fail(std::string("a stale required input must yield 'indeterminate', but the engine "
                            "answered '") +
                ecg::OutcomeName(stale_decision.outcome) + "'");
  }
  if (stale_decision.intent.has_value()) {
    return Fail("an 'indeterminate' decision emitted an intent; stale evidence became permissive");
  }
  if (!HasReason(stale_decision, ecg::ReasonCode::kEvidenceStale)) {
    return Fail("the 'indeterminate' decision does not name the stale evidence that caused it");
  }

  std::printf("=== consumer assertions ===\n");
  std::printf("  fresh evidence  -> %s (expected allowed)\n", ecg::OutcomeName(decision.outcome));
  std::printf("  stale evidence  -> %s (expected indeterminate)\n",
              ecg::OutcomeName(stale_decision.outcome));
  std::printf("\nfind_package consumer: PASS\n");
  return 0;
}
