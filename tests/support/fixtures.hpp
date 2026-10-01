#ifndef ECG_TEST_FIXTURES_HPP
#define ECG_TEST_FIXTURES_HPP

// Synthetic evidence fixtures.
//
// EVERY value produced here is SYNTHETIC. No facility hardware, meter, BMS,
// DCIM, or market feed produced any of it. The numbers are chosen to be
// internally consistent and to exercise the documented semantics; they are not a
// claim about any real data centre.

#include <cstdlib>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"
#include "ecg/ledger.hpp"
#include "test.hpp"

namespace ecgtest {

using ecg::DurationSec;
using ecg::EnergyKwh;
using ecg::MoneyMicros;
using ecg::PowerKw;
using ecg::PowerKwPerMin;
using ecg::PriceMicrosPerKwh;
using ecg::RatioPpm;
using ecg::UtcInstant;

/// Whole kilowatts.
[[nodiscard]] inline PowerKw Kw(std::int64_t whole_kw) {
  return PowerKw::FromRaw(whole_kw * 1000);
}
/// Whole kilowatt-hours.
[[nodiscard]] inline EnergyKwh Kwh(std::int64_t whole_kwh) {
  return EnergyKwh::FromRaw(whole_kwh * 1000);
}
/// Whole kilowatts per minute.
[[nodiscard]] inline PowerKwPerMin KwPerMin(std::int64_t whole) {
  return PowerKwPerMin::FromRaw(whole * 1000);
}
/// Whole seconds.
[[nodiscard]] inline DurationSec Secs(std::int64_t seconds) {
  return DurationSec::FromRaw(seconds);
}
/// Micros of currency per kilowatt-hour, from thousandths of a currency unit.
[[nodiscard]] inline PriceMicrosPerKwh PriceMilli(std::int64_t milli_currency_per_kwh) {
  return PriceMicrosPerKwh::FromRaw(milli_currency_per_kwh * 1000);
}
/// Micros of currency, from thousandths of a currency unit.
[[nodiscard]] inline MoneyMicros MoneyMilli(std::int64_t milli_currency) {
  return MoneyMicros::FromRaw(milli_currency * 1000);
}
/// A ratio expressed in whole parts per million.
[[nodiscard]] inline RatioPpm Ppm(std::int64_t ppm) { return RatioPpm::FromRaw(ppm); }

/// Parses a timestamp, aborting the test process on a malformed literal. Test
/// literals are code, so a bad one is a defect in the test, not a runtime case.
[[nodiscard]] inline UtcInstant Instant(const char* text) {
  const auto parsed = ecg::ParseRfc3339(text);
  if (!parsed.ok()) {
    std::fprintf(stderr, "fixture timestamp '%s' is invalid: %s\n", text,
                 parsed.error().ToString().c_str());
    std::abort();
  }
  return parsed.value();
}

[[nodiscard]] inline ecg::Interval Span(const char* begin, const char* end) {
  const auto interval = ecg::Interval::Make(Instant(begin), Instant(end));
  if (!interval.ok()) {
    std::fprintf(stderr, "fixture interval [%s, %s) is invalid\n", begin, end);
    std::abort();
  }
  return interval.value();
}

/// Everything a test needs: the policy, one consistent evidence bundle, the
/// evaluation instant, and an engine built from that policy.
struct Scenario {
  ecg::PolicySet policy;
  ecg::EvidenceSet evidence;
  ecg::UtcInstant now;
  /// The first Monday in the fixture month, which is where the weekday-only
  /// demand-charge window applies.
  ecg::UtcInstant monday_noon;
};

/// Builds the synthetic scenario. The evaluation instant is Monday 2026-03-02
/// 12:00 UTC, inside the day the demand-charge rule applies to.
[[nodiscard]] inline Scenario MakeScenario() {
  Scenario scenario;
  scenario.policy = ecg::DefaultPolicy();
  scenario.now = Instant("2026-03-02T12:00:00.000000Z");
  scenario.monday_noon = scenario.now;
  // The fixture's evening demand window opens at 18:00, six hours after the
  // evaluation instant, so the default six-hour horizon would defer every
  // evening action before any rule could consider it. The scenario widens the
  // horizon to twelve hours explicitly, rather than hiding the reason in a
  // per-test override.
  scenario.policy.decision_horizon = Secs(12 * 3600);

  const ecg::Epoch epoch = scenario.policy.epoch;
  const UtcInstant observed = ecg::UtcInstant::FromRaw(scenario.now.raw() - 60LL * 1000000LL);

  auto& evidence = scenario.evidence;
  evidence.epoch = epoch;

  // Tariff: a four-band day repeated across the fixture month.
  ecg::Tariff tariff;
  tariff.id = ecg::TariffId::FromTrusted("tariff.primary");
  tariff.revision = ecg::TariffRevision::FromRaw(12);
  tariff.epoch = epoch;
  tariff.currency = "USD";
  tariff.validity = Span("2026-01-01T00:00:00.000000Z", "2027-01-01T00:00:00.000000Z");
  tariff.published_at = Instant("2026-02-01T00:00:00.000000Z");

  const char* day_starts[] = {"2026-02-27", "2026-02-28", "2026-03-01", "2026-03-02",
                              "2026-03-03", "2026-03-04", "2026-03-05"};
  for (const char* day : day_starts) {
    const std::string prefix(day);
    const auto add_band = [&tariff, &prefix](const char* from, const char* to,
                                             std::int64_t milli_price) {
      ecg::PriceInterval interval;
      interval.window = Span((prefix + "T" + from + ":00:00.000000Z").c_str(),
                             (prefix + "T" + to + ":00:00.000000Z").c_str());
      interval.price = PriceMilli(milli_price);
      tariff.prices.push_back(interval);
    };
    add_band("00", "06", 80);
    add_band("06", "18", 180);
    add_band("18", "22", 280);
    add_band("22", "23", 100);
    // The final hour is expressed with an explicit next-day end below.
    ecg::PriceInterval tail;
    tail.window = ecg::Interval::Make(Instant((prefix + "T23:00:00.000000Z").c_str()),
                                      Instant((prefix + "T23:59:59.000000Z").c_str()))
                      .value();
    tail.price = PriceMilli(100);
    tariff.prices.push_back(tail);
  }

  ecg::DemandChargeRule rule;
  rule.id = ecg::DemandWindowId::FromTrusted("peak.evening");
  rule.recurrence.weekday_mask = 0x1Fu;  // Monday through Friday.
  rule.recurrence.start_second_of_day = 18 * 3600;
  rule.recurrence.duration_seconds = 4 * 3600;
  rule.averaging_interval = Secs(900);
  rule.threshold_kw = Kw(9000);
  rule.charge_per_kw = MoneyMilli(12500);
  rule.basis = ecg::DemandBasis::kPeakIntervalAverage;
  rule.validity = tariff.validity;
  tariff.demand_charges.push_back(rule);

  ecg::Evidence<ecg::Tariff> tariff_evidence;
  tariff_evidence.meta.authority = ecg::AuthorityKind::kTariffAuthority;
  tariff_evidence.meta.source = ecg::SourceId::FromTrusted("tariff.feed");
  tariff_evidence.meta.generation = ecg::EvidenceGeneration::FromRaw(4);
  tariff_evidence.meta.epoch = epoch;
  tariff_evidence.meta.observed_at = observed;
  tariff_evidence.meta.published_at = tariff.published_at;
  tariff_evidence.meta.provenance = ecg::Provenance::kSyntheticModel;
  tariff_evidence.value = tariff;
  evidence.tariff = tariff_evidence;

  ecg::Evidence<ecg::DemandObservation> demand;
  demand.meta.authority = ecg::AuthorityKind::kMeteringAuthority;
  demand.meta.source = ecg::SourceId::FromTrusted("meter.main");
  demand.meta.generation = ecg::EvidenceGeneration::FromRaw(9);
  demand.meta.epoch = epoch;
  demand.meta.observed_at = observed;
  demand.meta.published_at = observed;
  demand.meta.provenance = ecg::Provenance::kSyntheticModel;
  demand.value.current_demand_kw = Kw(9200);
  demand.value.rolling_peak_kw = Kw(9500);
  demand.value.interval_length = Secs(900);
  demand.value.interval_end = scenario.now;
  evidence.demand = demand;

  ecg::Evidence<ecg::ReserveState> reserve;
  reserve.meta.authority = ecg::AuthorityKind::kReserveAuthority;
  reserve.meta.source = ecg::SourceId::FromTrusted("reserve.authority");
  reserve.meta.generation = ecg::EvidenceGeneration::FromRaw(5);
  reserve.meta.epoch = epoch;
  reserve.meta.observed_at = observed;
  reserve.meta.published_at = observed;
  reserve.meta.provenance = ecg::Provenance::kSyntheticModel;
  reserve.value.current_reserve_kw = Kw(2500);
  reserve.value.reserve_floor_kw = Kw(1200);
  reserve.value.capacity_kw = Kw(12000);
  reserve.value.reserve_floor_ratio = Ppm(100000);
  evidence.reserve = reserve;

  ecg::Evidence<ecg::EfficiencyState> efficiency;
  efficiency.meta.authority = ecg::AuthorityKind::kEfficiencyAuthority;
  efficiency.meta.source = ecg::SourceId::FromTrusted("efficiency.feed");
  efficiency.meta.generation = ecg::EvidenceGeneration::FromRaw(2);
  efficiency.meta.epoch = epoch;
  efficiency.meta.observed_at = observed;
  efficiency.meta.published_at = observed;
  efficiency.meta.provenance = ecg::Provenance::kSyntheticModel;
  efficiency.value.pue = Ppm(1350000);
  efficiency.value.design_pue = Ppm(1250000);
  efficiency.value.measurement_window = Span("2026-03-02T11:00:00.000000Z", "2026-03-02T12:00:00.000000Z");
  efficiency.value.storage_round_trip_ratio = Ppm(880000);
  evidence.efficiency = efficiency;

  ecg::Evidence<ecg::ServiceCatalog> services;
  services.meta.authority = ecg::AuthorityKind::kServiceCatalogAuthority;
  services.meta.source = ecg::SourceId::FromTrusted("service.catalog");
  services.meta.generation = ecg::EvidenceGeneration::FromRaw(7);
  services.meta.epoch = epoch;
  services.meta.observed_at = observed;
  services.meta.published_at = observed;
  services.meta.provenance = ecg::Provenance::kSyntheticModel;
  {
    ecg::ServiceClassTerms critical;
    critical.id = ecg::ServiceClassId::FromTrusted("vm.critical");
    critical.permission = ecg::CurtailmentPermission::kProhibited;
    critical.max_curtailment_ratio = Ppm(0);
    critical.max_curtailment_duration = Secs(0);
    critical.minimum_notice = Secs(0);
    critical.priority = 100;
    critical.nominal_load_kw = Kw(3000);
    services.value.classes.push_back(critical);

    ecg::ServiceClassTerms batch;
    batch.id = ecg::ServiceClassId::FromTrusted("vm.batch");
    batch.permission = ecg::CurtailmentPermission::kWithNotice;
    batch.max_curtailment_ratio = Ppm(250000);
    batch.max_curtailment_duration = Secs(7200);
    batch.minimum_notice = Secs(1800);
    batch.priority = 10;
    batch.nominal_load_kw = Kw(4000);
    services.value.classes.push_back(batch);

    ecg::ServiceClassTerms flex;
    flex.id = ecg::ServiceClassId::FromTrusted("vm.flex");
    flex.permission = ecg::CurtailmentPermission::kUnrestricted;
    flex.max_curtailment_ratio = Ppm(500000);
    flex.max_curtailment_duration = Secs(14400);
    flex.minimum_notice = Secs(0);
    flex.priority = 1;
    flex.nominal_load_kw = Kw(2000);
    services.value.classes.push_back(flex);
  }
  evidence.services = services;

  ecg::Evidence<ecg::CapacityState> capacity;
  capacity.meta.authority = ecg::AuthorityKind::kCapacityAuthority;
  capacity.meta.source = ecg::SourceId::FromTrusted("capacity.authority");
  capacity.meta.generation = ecg::EvidenceGeneration::FromRaw(6);
  capacity.meta.epoch = epoch;
  capacity.meta.observed_at = observed;
  capacity.meta.published_at = observed;
  capacity.meta.provenance = ecg::Provenance::kSyntheticModel;
  capacity.value.flexible_load_kw = Kw(2500);
  capacity.value.shiftable_headroom_kw = Kw(3000);
  capacity.value.storage_charge_kw = Kw(1000);
  capacity.value.storage_discharge_kw = Kw(1000);
  capacity.value.storage_usable_energy_kwh = Kwh(4000);
  evidence.capacity = capacity;

  ecg::Evidence<ecg::RiskState> risk;
  risk.meta.authority = ecg::AuthorityKind::kRiskAuthority;
  risk.meta.source = ecg::SourceId::FromTrusted("risk.authority");
  risk.meta.generation = ecg::EvidenceGeneration::FromRaw(3);
  risk.meta.epoch = epoch;
  risk.meta.observed_at = observed;
  risk.meta.published_at = observed;
  risk.meta.provenance = ecg::Provenance::kSyntheticModel;
  risk.value.posture = ecg::RiskPosture::kNormal;
  risk.value.max_ramp_kw_per_min = KwPerMin(500);
  risk.value.max_deferral = Secs(14400);
  risk.value.minimum_dwell = Secs(1800);
  risk.value.last_change_at = ecg::UtcInstant::FromRaw(0);
  evidence.risk = risk;

  ecg::Evidence<ecg::IncidentState> incident;
  incident.meta.authority = ecg::AuthorityKind::kIncidentAuthority;
  incident.meta.source = ecg::SourceId::FromTrusted("incident.authority");
  incident.meta.generation = ecg::EvidenceGeneration::FromRaw(11);
  incident.meta.epoch = epoch;
  incident.meta.observed_at = observed;
  incident.meta.published_at = observed;
  incident.meta.provenance = ecg::Provenance::kSyntheticModel;
  incident.value.severity = ecg::IncidentSeverity::kNone;
  incident.value.declared_at = ecg::UtcInstant::FromRaw(0);
  evidence.incident = incident;

  ecg::Evidence<ecg::ZoneRules> zone;
  zone.meta.authority = ecg::AuthorityKind::kTimeZoneAuthority;
  zone.meta.source = ecg::SourceId::FromTrusted("tz.authority");
  zone.meta.generation = ecg::EvidenceGeneration::FromRaw(1);
  zone.meta.epoch = epoch;
  zone.meta.observed_at = observed;
  zone.meta.published_at = observed;
  zone.meta.provenance = ecg::Provenance::kSyntheticModel;
  zone.value = ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("UTC"), 0, {}).value();
  evidence.zone = zone;

  return scenario;
}

/// Builds a request with a stable identity derived from the given ordinal.
[[nodiscard]] inline ecg::DecisionRequest MakeRequest(const Scenario& scenario,
                                                      std::uint64_t ordinal,
                                                      ecg::RequestKind kind,
                                                      PowerKw magnitude,
                                                      DurationSec duration,
                                                      const ecg::Interval& window,
                                                      const char* service_class = "") {
  ecg::DecisionRequest request;
  request.request_id = ecg::RequestId::FromRaw(ordinal);
  request.client_id = ecg::ClientId::FromTrusted("ops.duty");
  request.epoch = scenario.policy.epoch;
  request.policy_generation = scenario.policy.generation;
  request.kind = kind;
  if (service_class != nullptr && service_class[0] != '\0') {
    request.service_class = ecg::ServiceClassId::FromTrusted(service_class);
  }
  request.magnitude_kw = magnitude;
  request.duration = duration;
  request.desired_window = window;
  request.rationale = ecg::Rationale::kPriceArbitrage;
  request.requested_at = scenario.now;
  request.attempt = 0;
  return request;
}

/// Evaluates and aborts the test when the engine reports an error.
[[nodiscard]] inline ecg::Decision EvaluateOrDie(const ecg::GovernorEngine& engine,
                                                 const ecg::DecisionRequest& request,
                                                 const ecg::EvidenceSet& evidence,
                                                 UtcInstant now) {
  const auto decision = engine.Evaluate(request, evidence, now);
  if (!decision.ok()) {
    std::fprintf(stderr, "engine returned an error: %s\n", decision.error().ToString().c_str());
    std::abort();
  }
  return decision.value();
}

/// True when the trace contains a reason with the given code.
[[nodiscard]] inline bool HasReason(const ecg::Decision& decision, ecg::ReasonCode code) {
  for (const ecg::Reason& reason : decision.reasons) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

/// The detail text of the first reason with the given code, or an empty string.
[[nodiscard]] inline std::string ReasonDetail(const ecg::Decision& decision, ecg::ReasonCode code) {
  for (const ecg::Reason& reason : decision.reasons) {
    if (reason.code == code) {
      return reason.detail;
    }
  }
  return {};
}

}  // namespace ecgtest

#endif  // ECG_TEST_FIXTURES_HPP
