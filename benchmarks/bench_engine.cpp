// Synthetic decision-path benchmark for Energy-Cost-Governor.
//
// ===========================================================================
// SYNTHETIC INPUT NOTICE
// ===========================================================================
// Every physical and economic value in this file is a hand-written model input.
// None of it was measured, metered, or read from a facility. The evidence bundle
// below is published with ecg::AuthorityKind::kSyntheticModel and
// ecg::Provenance::kSyntheticModel precisely so that it can never be mistaken
// for -- or promoted into -- metering, BMS, or DCIM telemetry, and no result of
// this program may be presented as data from real hardware.
//
// What is real here:
//   * the engine work being timed: real code, real fixed-point arithmetic, real
//     allocation, real reason traces and real decision digests;
//   * the durable ledger path (only when a state directory is supplied): real
//     files, real framing, and a real FlushFileBuffers/fsync per commit.
//
// ===========================================================================
// OUTPUT CONTRACT
// ===========================================================================
// One machine-readable line per measurement, in exactly this shape:
//
//     BENCH <name> <value> <unit> <REAL|SYNTHETIC|UNSUPPORTED>
//
// followed by a human-readable summary. A quantity that cannot be measured in
// this invocation is reported as UNSUPPORTED with a one-line reason; no number
// is ever invented to fill the gap. The exit code is 0 only when every
// requested measurement completed and every internal expectation held, 1 on
// internal failure, and 2 on a usage error.
//
// Usage: ecg_bench [decisions] [state_directory] [commits]
//   decisions        completed decisions to time          (default 200000)
//   state_directory  where the durable ledger lives       (optional; REAL work)
//   commits          durable commits to time              (default 200, max 20000)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/engine.hpp"
#include "ecg/evidence.hpp"
#include "ecg/ledger.hpp"
#include "ecg/policy.hpp"
#include "ecg/request.hpp"
#include "ecg/time.hpp"
#include "ecg/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int64_t kRawPerUnit = 1000;  // raw units per kW / per kWh
constexpr std::int64_t kMicrosPerSecond = 1000000;
constexpr std::size_t kDefaultDecisions = 200000;
constexpr std::size_t kDefaultCommits = 200;
constexpr std::size_t kMaxCommits = 20000;

// ---------------------------------------------------------------------------
// Fixed-point helpers. No floating point appears in any domain value: the whole
// point of this runtime is that a decision replays bit for bit.
// ---------------------------------------------------------------------------

[[nodiscard]] ecg::PowerKw Kw(std::int64_t kilowatts) noexcept {
  return ecg::PowerKw::FromRaw(kilowatts * kRawPerUnit);
}
[[nodiscard]] ecg::EnergyKwh Kwh(std::int64_t kilowatt_hours) noexcept {
  return ecg::EnergyKwh::FromRaw(kilowatt_hours * kRawPerUnit);
}
[[nodiscard]] ecg::PowerKwPerMin KwPerMin(std::int64_t kilowatts_per_minute) noexcept {
  return ecg::PowerKwPerMin::FromRaw(kilowatts_per_minute * kRawPerUnit);
}
[[nodiscard]] ecg::PriceMicrosPerKwh MicrosPerKwh(std::int64_t micros) noexcept {
  return ecg::PriceMicrosPerKwh::FromRaw(micros);
}
[[nodiscard]] ecg::MoneyMicros MoneyMicros(std::int64_t micros) noexcept {
  return ecg::MoneyMicros::FromRaw(micros);
}
[[nodiscard]] ecg::RatioPpm Ratio(std::int64_t ppm) noexcept { return ecg::RatioPpm::FromRaw(ppm); }
[[nodiscard]] ecg::DurationSec Secs(std::int64_t seconds) noexcept {
  return ecg::DurationSec::FromRaw(seconds);
}

// ---------------------------------------------------------------------------
// The synthetic timeline. All instants are UTC; 2026-03-02 is a Monday.
// ---------------------------------------------------------------------------

constexpr std::string_view kNowText = "2026-03-02T13:30:00.000000Z";
constexpr std::string_view kWindowBeginText = "2026-03-02T14:00:00.000000Z";
constexpr std::string_view kWindowEndText = "2026-03-02T15:00:00.000000Z";
constexpr std::string_view kDayBeginText = "2026-03-02T00:00:00.000000Z";
constexpr std::string_view kDayEndText = "2026-03-03T00:00:00.000000Z";
constexpr std::string_view kDstTransitionText = "2026-03-08T07:00:00.000000Z";

struct PriceRow {
  const char* begin;
  const char* end;
  std::int64_t micros_per_kwh;
};

// A five-block day: overnight cheap, morning shoulder, afternoon peak, evening
// shoulder, late cheap. The afternoon peak is what the modelled requests move
// load out of.
constexpr PriceRow kPriceRows[] = {
    {"2026-03-02T00:00:00.000000Z", "2026-03-02T06:00:00.000000Z", 80000},
    {"2026-03-02T06:00:00.000000Z", "2026-03-02T12:00:00.000000Z", 180000},
    {"2026-03-02T12:00:00.000000Z", "2026-03-02T18:00:00.000000Z", 320000},
    {"2026-03-02T18:00:00.000000Z", "2026-03-02T21:00:00.000000Z", 210000},
    {"2026-03-02T21:00:00.000000Z", "2026-03-03T00:00:00.000000Z", 90000},
};

// Instants below are relative to the synthetic "now"; that timeline is decades
// away from the representable floor, so a plain subtraction is exact here.
[[nodiscard]] ecg::UtcInstant Ago(ecg::UtcInstant now, std::int64_t seconds) noexcept {
  return ecg::UtcInstant::FromRaw(now.raw() - seconds * kMicrosPerSecond);
}

// Envelope metadata for modelled input. Authority and provenance are both
// kSyntheticModel on purpose: this input has no owning authority and must never
// be read as a facility observation.
[[nodiscard]] ecg::EvidenceMeta SyntheticMeta(const char* source, ecg::UtcInstant observed_at,
                                              std::int64_t generation, ecg::Epoch epoch) {
  ecg::EvidenceMeta meta;
  meta.authority = ecg::AuthorityKind::kSyntheticModel;
  meta.source = ecg::SourceId::FromTrusted(source);
  meta.generation = ecg::EvidenceGeneration::FromRaw(static_cast<std::uint64_t>(generation));
  meta.epoch = epoch;
  meta.observed_at = observed_at;
  meta.published_at = observed_at;
  meta.payload_version = 1;
  meta.provenance = ecg::Provenance::kSyntheticModel;
  return meta;
}

template <class Payload>
[[nodiscard]] ecg::Evidence<Payload> Wrap(Payload payload, const char* source,
                                          ecg::UtcInstant observed_at, std::int64_t generation,
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

[[nodiscard]] ecg::Result<ecg::PriceInterval> MakePriceInterval(const PriceRow& row) {
  ECG_TRY(window, MakeInterval(row.begin, row.end));
  ecg::PriceInterval interval;
  interval.window = window;
  interval.price = MicrosPerKwh(row.micros_per_kwh);
  return interval;
}

// ---------------------------------------------------------------------------
// The modelled world: one policy, one evidence bundle, three requests.
// ---------------------------------------------------------------------------

struct Scenario {
  const char* label;
  ecg::RequestKind kind;
  const char* service_class;
  ecg::Outcome expected;
};

constexpr Scenario kScenarios[] = {
    {"shift_flexible_load", ecg::RequestKind::kShiftFlexibleLoad, "vm.batch",
     ecg::Outcome::kAllowed},
    {"curtail_service_class", ecg::RequestKind::kCurtailServiceClass, "cooling.room",
     ecg::Outcome::kAllowed},
    {"defer_batch_work", ecg::RequestKind::kDeferBatchWork, "vm.batch", ecg::Outcome::kAllowed},
};

struct World {
  ecg::PolicySet policy;
  ecg::EvidenceSet evidence;
  ecg::UtcInstant now;
  ecg::Interval window;
  std::vector<ecg::DecisionRequest> requests;
};

[[nodiscard]] ecg::Result<World> BuildSyntheticWorld() {
  World world;
  world.policy = ecg::DefaultPolicy();

  ECG_TRY(now, ecg::ParseRfc3339(kNowText));
  ECG_TRY(window, MakeInterval(kWindowBeginText, kWindowEndText));
  ECG_TRY(validity, MakeInterval(kDayBeginText, kDayEndText));
  world.now = now;
  world.window = window;
  world.evidence.epoch = world.policy.epoch;

  // -- tariff authority -----------------------------------------------------
  ecg::Tariff tariff;
  tariff.id = ecg::TariffId::FromTrusted("synthetic.bench.tariff");
  tariff.revision = ecg::TariffRevision::FromRaw(1);
  tariff.epoch = world.policy.epoch;
  tariff.currency = "USD";
  tariff.published_at = Ago(now, 3600);
  tariff.validity = validity;
  for (const PriceRow& row : kPriceRows) {
    ECG_TRY(price_interval, MakePriceInterval(row));
    tariff.prices.push_back(price_interval);
  }
  ecg::DemandChargeRule demand_rule;
  demand_rule.id = ecg::DemandWindowId::FromTrusted("synthetic.bench.peak");
  demand_rule.recurrence.weekday_mask = 0x1Fu;  // Monday..Friday
  demand_rule.recurrence.start_second_of_day = 14 * 3600;
  demand_rule.recurrence.duration_seconds = 5 * 3600;
  demand_rule.averaging_interval = Secs(900);
  demand_rule.threshold_kw = Kw(4000);
  demand_rule.charge_per_kw = MoneyMicros(12000000);
  demand_rule.basis = ecg::DemandBasis::kPeakIntervalAverage;
  demand_rule.validity = validity;
  tariff.demand_charges.push_back(demand_rule);
  world.evidence.tariff = Wrap(tariff, "synthetic.bench.tariff", Ago(now, 120), 1,
                               world.policy.epoch);

  // -- metering authority ---------------------------------------------------
  ecg::DemandObservation demand;
  demand.current_demand_kw = Kw(3000);
  demand.rolling_peak_kw = Kw(3200);
  demand.interval_length = Secs(900);
  demand.interval_end = Ago(now, 30);
  world.evidence.demand = Wrap(demand, "synthetic.bench.meter", Ago(now, 30), 2,
                               world.policy.epoch);

  // -- reserve authority ----------------------------------------------------
  ecg::ReserveState reserve;
  reserve.current_reserve_kw = Kw(900);
  reserve.reserve_floor_kw = Kw(400);
  reserve.capacity_kw = Kw(5000);
  reserve.reserve_floor_ratio = Ratio(100000);  // 10% of installed capacity
  world.evidence.reserve = Wrap(reserve, "synthetic.bench.reserve", Ago(now, 30), 3,
                                world.policy.epoch);

  // -- efficiency authority -------------------------------------------------
  ecg::EfficiencyState efficiency;
  efficiency.pue = Ratio(1250000);
  efficiency.design_pue = Ratio(1200000);
  ECG_TRY(efficiency_window, ecg::Interval::Make(Ago(now, 3600), now));
  efficiency.measurement_window = efficiency_window;
  efficiency.storage_round_trip_ratio = Ratio(880000);
  world.evidence.efficiency = Wrap(efficiency, "synthetic.bench.efficiency", Ago(now, 600), 4,
                                   world.policy.epoch);

  // -- service catalog authority -------------------------------------------
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

  ecg::ServiceClassTerms cooling;
  cooling.id = ecg::ServiceClassId::FromTrusted("cooling.room");
  cooling.permission = ecg::CurtailmentPermission::kWithNotice;
  cooling.max_curtailment_ratio = Ratio(400000);
  cooling.max_curtailment_duration = Secs(5400);
  cooling.minimum_notice = Secs(900);
  cooling.priority = 90;
  cooling.nominal_load_kw = Kw(8000);
  catalog.classes.push_back(cooling);

  ecg::ServiceClassTerms lighting;
  lighting.id = ecg::ServiceClassId::FromTrusted("lighting.common");
  lighting.permission = ecg::CurtailmentPermission::kProhibited;
  lighting.max_curtailment_ratio = Ratio(0);
  lighting.max_curtailment_duration = Secs(0);
  lighting.minimum_notice = Secs(0);
  lighting.priority = 100;
  lighting.nominal_load_kw = Kw(500);
  catalog.classes.push_back(lighting);

  world.evidence.services = Wrap(catalog, "synthetic.bench.services", Ago(now, 600), 5,
                                 world.policy.epoch);

  // -- capacity authority ---------------------------------------------------
  ecg::CapacityState capacity;
  capacity.flexible_load_kw = Kw(2000);
  capacity.shiftable_headroom_kw = Kw(1200);
  capacity.storage_charge_kw = Kw(500);
  capacity.storage_discharge_kw = Kw(500);
  capacity.storage_usable_energy_kwh = Kwh(1000);
  world.evidence.capacity = Wrap(capacity, "synthetic.bench.capacity", Ago(now, 30), 6,
                                 world.policy.epoch);

  // -- risk authority -------------------------------------------------------
  ecg::RiskState risk;
  risk.posture = ecg::RiskPosture::kNormal;
  risk.max_ramp_kw_per_min = KwPerMin(100);
  risk.max_deferral = Secs(14400);
  risk.minimum_dwell = Secs(900);
  risk.last_change_at = Ago(now, 7200);
  world.evidence.risk = Wrap(risk, "synthetic.bench.risk", Ago(now, 30), 7, world.policy.epoch);

  // -- incident authority ---------------------------------------------------
  ecg::IncidentState incident;
  incident.severity = ecg::IncidentSeverity::kNone;
  incident.declared_at = Ago(now, 21600);
  world.evidence.incident = Wrap(incident, "synthetic.bench.incident", Ago(now, 10), 8,
                                 world.policy.epoch);

  // -- time zone authority --------------------------------------------------
  ECG_TRY(dst, ecg::ParseRfc3339(kDstTransitionText));
  std::vector<ecg::ZoneTransition> transitions;
  ecg::ZoneTransition spring_forward;
  spring_forward.at = dst;
  spring_forward.offset_seconds = -4 * 3600;
  transitions.push_back(spring_forward);
  ECG_TRY(zone, ecg::ZoneRules::Make(ecg::ZoneId::FromTrusted("america/new_york"), -5 * 3600,
                                     std::move(transitions)));
  world.evidence.zone = Wrap(zone, "synthetic.bench.zone", Ago(now, 3600), 9, world.policy.epoch);

  // -- requests -------------------------------------------------------------
  for (const Scenario& scenario : kScenarios) {
    ecg::DecisionRequest request;
    request.request_id = ecg::RequestId::FromRaw(1);
    request.client_id = ecg::ClientId::FromTrusted("synthetic.bench.client");
    request.epoch = world.policy.epoch;
    request.policy_generation = world.policy.generation;
    request.kind = scenario.kind;
    request.service_class = ecg::ServiceClassId::FromTrusted(scenario.service_class);
    request.magnitude_kw = Kw(500);
    request.duration = Secs(3600);
    request.desired_window = window;
    request.rationale = ecg::Rationale::kPriceArbitrage;
    request.requested_at = now;
    request.attempt = 0;
    world.requests.push_back(request);
  }
  return world;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct EvaluationTiming {
  std::size_t completed{0};
  std::int64_t batch_ns{0};
  std::vector<std::int64_t> samples_ns;
  std::uint64_t checksum{0};
  std::array<std::uint64_t, 4> outcomes{};
};

// Evaluates count decisions and consumes every outcome: the outcome class, the
// trace length, the evidence dependencies, the bounded intent, and the replay
// digest. A decision that was produced but not consumed would not be completed
// work, so the checksum is part of the measurement contract, not decoration.
[[nodiscard]] bool EvaluateDecisions(const ecg::GovernorEngine& engine, const World& world,
                                     std::size_t count, std::uint64_t first_request_id,
                                     EvaluationTiming* timing, std::string* error) {
  const std::size_t scenario_count = world.requests.size();
  timing->samples_ns.clear();
  timing->samples_ns.reserve(count);
  timing->outcomes = {};
  timing->checksum = 0;
  timing->completed = 0;

  std::uint64_t checksum = 0;
  std::array<std::uint64_t, 4> outcomes{};
  const auto batch_start = Clock::now();
  for (std::size_t i = 0; i < count; ++i) {
    ecg::DecisionRequest request = world.requests[i % scenario_count];
    request.request_id = ecg::RequestId::FromRaw(first_request_id + i);

    const auto start = Clock::now();
    const ecg::Result<ecg::Decision> result = engine.Evaluate(request, world.evidence, world.now);
    const auto finish = Clock::now();
    if (!result.ok()) {
      *error = "GovernorEngine::Evaluate failed for request " + request.request_id.ToString() +
               ": " + result.error().ToString();
      return false;
    }
    const ecg::Decision& decision = result.value();
    outcomes[static_cast<std::size_t>(decision.outcome)] += 1;
    checksum += decision.digest.raw();
    checksum += static_cast<std::uint64_t>(decision.reasons.size());
    checksum += static_cast<std::uint64_t>(decision.evidence_dependencies.size());
    if (decision.intent.has_value()) {
      checksum += static_cast<std::uint64_t>(decision.intent->magnitude_limit_kw.raw());
      checksum += static_cast<std::uint64_t>(decision.intent->duration_limit.raw());
    }
    timing->samples_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    timing->completed += 1;
  }
  timing->batch_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - batch_start).count();
  timing->checksum = checksum;
  timing->outcomes = outcomes;
  if (timing->completed != count) {
    *error = "only " + std::to_string(timing->completed) + " of " + std::to_string(count) +
             " decisions completed";
    return false;
  }
  return true;
}

[[nodiscard]] std::int64_t MeanNs(const std::vector<std::int64_t>& samples) {
  if (samples.empty()) {
    return 0;
  }
  std::int64_t total = 0;
  for (const std::int64_t sample : samples) {
    total += sample;
  }
  return total / static_cast<std::int64_t>(samples.size());
}

// Nearest-rank percentile over an ascending vector.
[[nodiscard]] std::int64_t PercentileNs(const std::vector<std::int64_t>& ascending,
                                        std::size_t numerator, std::size_t denominator) {
  if (ascending.empty()) {
    return 0;
  }
  std::size_t index = (ascending.size() * numerator) / denominator;
  if (index >= ascending.size()) {
    index = ascending.size() - 1;
  }
  return ascending[index];
}

void EmitBench(const char* name, std::int64_t value, const char* unit, const char* kind) {
  std::printf("BENCH %s %lld %s %s\n", name, static_cast<long long>(value), unit, kind);
}

void EmitBenchRate(const char* name, double value, const char* unit, const char* kind) {
  std::printf("BENCH %s %.0f %s %s\n", name, value, unit, kind);
}

void EmitUnsupported(const char* name, const char* unit, const char* reason) {
  std::printf("BENCH %s - %s UNSUPPORTED\n", name, unit);
  std::printf("      reason: %s\n", reason);
}

// ---------------------------------------------------------------------------
// Durable ledger measurement. REAL: real files, real flush to stable storage.
// ---------------------------------------------------------------------------

struct LedgerTiming {
  std::string directory;
  std::int64_t open_ns{0};
  std::int64_t batch_ns{0};
  std::vector<std::int64_t> samples_ns;
  std::uint64_t checksum{0};
  std::uint64_t first_sequence{0};
  std::uint64_t last_sequence{0};
  std::uint64_t journal_bytes{0};
  std::uint64_t verified_frames{0};
  std::size_t committed{0};
};

// A per-run identity base, so a directory reused across runs cannot turn a
// fresh submission into a deduplicated -- and therefore unmeasured -- one.
[[nodiscard]] std::uint64_t RequestIdBase() noexcept {
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  return (static_cast<std::uint64_t>(micros) << 16) + 1u;
}

[[nodiscard]] bool RunDurableCommits(const World& world, const std::filesystem::path& directory,
                                     std::size_t commits, LedgerTiming* timing,
                                     std::string* error) {
  std::error_code absolute_error;
  const std::filesystem::path absolute = std::filesystem::absolute(directory, absolute_error);
  timing->directory = absolute_error ? directory.string() : absolute.string();
  timing->samples_ns.clear();
  timing->samples_ns.reserve(commits);

  const auto open_start = Clock::now();
  const ecg::Result<std::unique_ptr<ecg::DecisionLedger>> opened =
      ecg::DecisionLedger::Open(directory, world.policy);
  timing->open_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - open_start).count();
  if (!opened.ok()) {
    *error = "DecisionLedger::Open failed: " + opened.error().ToString();
    return false;
  }
  const std::unique_ptr<ecg::DecisionLedger>& ledger = opened.value();

  const std::uint64_t first_request_id = RequestIdBase();
  const std::size_t scenario_count = world.requests.size();
  std::uint64_t checksum = 0;
  const auto batch_start = Clock::now();
  for (std::size_t i = 0; i < commits; ++i) {
    ecg::DecisionRequest request = world.requests[i % scenario_count];
    request.request_id = ecg::RequestId::FromRaw(first_request_id + i);

    const auto start = Clock::now();
    const ecg::Result<ecg::CommittedDecision> submitted =
        ledger->Submit(request, world.evidence, world.now);
    const auto finish = Clock::now();
    if (!submitted.ok()) {
      *error = "DecisionLedger::Submit failed for request " + request.request_id.ToString() + ": " +
               submitted.error().ToString();
      return false;
    }
    const ecg::CommittedDecision& committed = submitted.value();
    if (committed.duplicate) {
      *error =
          "the ledger reported a duplicate for a fresh (client, request) identity; the durable "
          "write did not happen and the timing would be meaningless";
      return false;
    }
    checksum += committed.sequence.raw();
    checksum += committed.decision.digest.raw();
    if (timing->committed == 0) {
      timing->first_sequence = committed.sequence.raw();
    }
    timing->last_sequence = committed.sequence.raw();
    timing->samples_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    timing->committed += 1;
  }
  timing->batch_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - batch_start).count();
  timing->checksum = checksum;

  // Verify re-reads the whole journal and checks every frame and chain link:
  // real work against the real file, and the proof that the commits landed.
  const ecg::Result<ecg::RecoveryReport> report = ledger->Verify();
  if (!report.ok()) {
    *error = "DecisionLedger::Verify failed: " + report.error().ToString();
    return false;
  }
  timing->verified_frames = static_cast<std::uint64_t>(report.value().frames_read);

  std::error_code size_error;
  const std::uintmax_t bytes =
      std::filesystem::file_size(directory / ecg::kJournalFileName, size_error);
  if (size_error) {
    *error = std::string("cannot size the journal file: ") + size_error.message();
    return false;
  }
  timing->journal_bytes = static_cast<std::uint64_t>(bytes);

  const ecg::Status closed = ledger->Close();
  if (!closed.ok()) {
    *error = "DecisionLedger::Close failed: " + closed.error().ToString();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Argument handling
// ---------------------------------------------------------------------------

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [decisions] [state_directory] [commits]\n"
               "  decisions        completed decisions to time (default %zu)\n"
               "  state_directory  durable ledger directory; enables the REAL measurements\n"
               "  commits          durable commits to time (default %zu, max %zu)\n",
               program, kDefaultDecisions, kDefaultCommits, kMaxCommits);
}

[[nodiscard]] bool ParseCount(const char* text, std::size_t minimum, std::size_t maximum,
                              std::size_t* out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  if (parsed < minimum || parsed > maximum) {
    return false;
  }
  *out = static_cast<std::size_t>(parsed);
  return true;
}

[[nodiscard]] int Fail(const std::string& message) {
  std::fprintf(stderr, "ecg_bench: FAIL: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t decisions = kDefaultDecisions;
  std::size_t commits = kDefaultCommits;
  std::string state_directory;

  const char* program = argc > 0 ? argv[0] : "ecg_bench";
  if (argc > 4) {
    PrintUsage(program);
    return 2;
  }
  if (argc > 1 && !ParseCount(argv[1], 1, 1000000000ULL, &decisions)) {
    std::fprintf(stderr, "ecg_bench: invalid decision count '%s'\n", argv[1]);
    PrintUsage(program);
    return 2;
  }
  if (argc > 2) {
    state_directory = argv[2];
    if (state_directory.empty()) {
      std::fprintf(stderr, "ecg_bench: the state directory must not be empty\n");
      return 2;
    }
  }
  if (argc > 3 && !ParseCount(argv[3], 1, kMaxCommits, &commits)) {
    std::fprintf(stderr, "ecg_bench: invalid commit count '%s'\n", argv[3]);
    PrintUsage(program);
    return 2;
  }

  const ecg::Result<World> world_result = BuildSyntheticWorld();
  if (!world_result.ok()) {
    return Fail("cannot build the synthetic world: " + world_result.error().ToString());
  }
  const World& world = world_result.value();

  const ecg::Result<ecg::GovernorEngine> engine_result = ecg::GovernorEngine::Make(world.policy);
  if (!engine_result.ok()) {
    return Fail("GovernorEngine::Make: " + engine_result.error().ToString());
  }
  const ecg::GovernorEngine& engine = engine_result.value();

  std::printf("=== Energy-Cost-Governor decision benchmark ===\n");
  std::printf("build            : %s\n", std::string(ecg::BuildIdentity()).c_str());
  std::printf("policy           : %s generation %s epoch %s\n", world.policy.id.c_str(),
              world.policy.generation.ToString().c_str(), world.policy.epoch.ToString().c_str());
  std::printf("evidence         : SYNTHETIC MODEL INPUT -- hand-written values, not facility\n");
  std::printf("                   telemetry, not BMS/DCIM data, not a measurement of anything.\n");
  std::printf("                   authority=synthetic_model provenance=synthetic_model\n");
  std::printf("modelled now     : %s\n", ecg::FormatRfc3339(world.now).c_str());
  std::printf("modelled window  : %s .. %s\n", ecg::FormatRfc3339(world.window.begin()).c_str(),
              ecg::FormatRfc3339(world.window.end()).c_str());
  if (state_directory.empty()) {
    std::printf("requested        : %zu decisions; durable ledger not requested\n", decisions);
  } else {
    std::printf("requested        : %zu decisions; %zu durable commits into %s\n", decisions,
                commits, std::filesystem::path(state_directory).string().c_str());
  }
  std::printf("\n");

  // Before timing anything, prove the modelled scenario does what it claims. A
  // synthetic benchmark whose scenario quietly stopped exercising the intended
  // path would report a number about nothing.
  std::printf("-- scenario self-check (every modelled request must be %s) --\n",
              ecg::OutcomeName(ecg::Outcome::kAllowed));
  for (std::size_t i = 0; i < world.requests.size(); ++i) {
    const ecg::Result<ecg::Decision> decision =
        engine.Evaluate(world.requests[i], world.evidence, world.now);
    if (!decision.ok()) {
      return Fail("self-check evaluation failed: " + decision.error().ToString());
    }
    std::printf("  %-22s -> %-14s %s\n", kScenarios[i].label,
                ecg::OutcomeName(decision.value().outcome),
                ecg::FormatDecisionSummary(decision.value()).c_str());
    if (decision.value().outcome != kScenarios[i].expected) {
      return Fail(std::string("scenario '") + kScenarios[i].label + "' produced " +
                  ecg::OutcomeName(decision.value().outcome) +
                  " but the modelled evidence implies " + ecg::OutcomeName(kScenarios[i].expected));
    }
  }
  std::printf("\n");

  // -- warm-up, reported separately from the measured run -------------------
  const std::size_t warmup_count =
      std::min<std::size_t>(std::max<std::size_t>(decisions / 10, 1000), 20000);
  std::string error;
  EvaluationTiming warmup;
  if (!EvaluateDecisions(engine, world, warmup_count, 0x1000000ULL, &warmup, &error)) {
    return Fail("warm-up: " + error);
  }

  // -- measured run ---------------------------------------------------------
  EvaluationTiming timed;
  if (!EvaluateDecisions(engine, world, decisions, 0x2000000ULL, &timed, &error)) {
    return Fail("measurement: " + error);
  }

  std::vector<std::int64_t> ascending = timed.samples_ns;
  std::sort(ascending.begin(), ascending.end());
  const std::int64_t mean_ns = MeanNs(timed.samples_ns);
  const std::int64_t median_ns = PercentileNs(ascending, 1, 2);
  const std::int64_t p99_ns = PercentileNs(ascending, 99, 100);
  const std::int64_t batch_mean_ns = timed.batch_ns / static_cast<std::int64_t>(timed.completed);
  const double throughput =
      static_cast<double>(timed.completed) * 1e9 / static_cast<double>(timed.batch_ns);

  EmitBench("evaluate_warmup_decisions", static_cast<std::int64_t>(warmup.completed), "decisions",
            "SYNTHETIC");
  EmitBench("evaluate_warmup_mean", MeanNs(warmup.samples_ns), "ns/decision", "SYNTHETIC");
  EmitBench("evaluate_decisions", static_cast<std::int64_t>(timed.completed), "decisions",
            "SYNTHETIC");
  EmitBench("evaluate_mean", mean_ns, "ns/decision", "SYNTHETIC");
  EmitBench("evaluate_batch_mean", batch_mean_ns, "ns/decision", "SYNTHETIC");
  EmitBench("evaluate_median", median_ns, "ns/decision", "SYNTHETIC");
  EmitBench("evaluate_p99", p99_ns, "ns/decision", "SYNTHETIC");
  EmitBenchRate("evaluate_throughput", throughput, "decisions/s", "SYNTHETIC");

  // -- durable ledger -------------------------------------------------------
  LedgerTiming ledger;
  bool ledger_measured = false;
  std::int64_t ledger_median = 0;
  if (state_directory.empty()) {
    const char* reason = "no state directory was supplied (pass it as argv[2])";
    EmitUnsupported("ledger_open", "ns", reason);
    EmitUnsupported("ledger_commits", "decisions", reason);
    EmitUnsupported("ledger_commit_mean", "ns/commit", reason);
    EmitUnsupported("ledger_commit_median", "ns/commit", reason);
    EmitUnsupported("ledger_commit_p99", "ns/commit", reason);
    EmitUnsupported("ledger_commit_throughput", "commits/s", reason);
    EmitUnsupported("ledger_journal_bytes", "bytes", reason);
    EmitUnsupported("ledger_verified_frames", "frames", reason);
  } else {
    if (!RunDurableCommits(world, std::filesystem::path(state_directory), commits, &ledger,
                           &error)) {
      return Fail("durable ledger: " + error);
    }
    ledger_measured = true;
    std::vector<std::int64_t> ledger_ascending = ledger.samples_ns;
    std::sort(ledger_ascending.begin(), ledger_ascending.end());
    const std::int64_t ledger_mean = MeanNs(ledger.samples_ns);
    ledger_median = PercentileNs(ledger_ascending, 1, 2);
    EmitBench("ledger_open", ledger.open_ns, "ns", "REAL");
    EmitBench("ledger_commits", static_cast<std::int64_t>(ledger.committed), "decisions", "REAL");
    EmitBench("ledger_commit_mean", ledger_mean, "ns/commit", "REAL");
    EmitBench("ledger_commit_median", ledger_median, "ns/commit", "REAL");
    EmitBench("ledger_commit_p99", PercentileNs(ledger_ascending, 99, 100), "ns/commit", "REAL");
    EmitBenchRate("ledger_commit_throughput",
                  static_cast<double>(ledger.committed) * 1e9 /
                      static_cast<double>(ledger.batch_ns),
                  "commits/s", "REAL");
    EmitBench("ledger_journal_bytes", static_cast<std::int64_t>(ledger.journal_bytes), "bytes",
              "REAL");
    EmitBench("ledger_verified_frames", static_cast<std::int64_t>(ledger.verified_frames), "frames",
              "REAL");
  }

  // -- human-readable summary ----------------------------------------------
  std::printf("\n=== summary ===\n");
  std::printf("decision path -- SYNTHETIC input, real engine work\n");
  std::printf("  completed decisions : %zu (warm-up %zu, reported separately)\n", timed.completed,
              warmup.completed);
  std::printf("  mean                : %lld ns (%.3f us) per completed decision\n",
              static_cast<long long>(mean_ns), static_cast<double>(mean_ns) / 1000.0);
  std::printf("  mean, whole batch   : %lld ns (%.3f us) per completed decision, with no clock\n",
              static_cast<long long>(batch_mean_ns),
              static_cast<double>(batch_mean_ns) / 1000.0);
  std::printf("                        read inside the timed region\n");
  std::printf("  median / p99        : %lld ns / %lld ns per completed decision\n",
              static_cast<long long>(median_ns), static_cast<long long>(p99_ns));
  std::printf("  throughput          : %.0f completed decisions/s (whole-batch timing)\n",
              throughput);
  std::printf("  outcome mix         : allowed=%llu refused=%llu deferred=%llu indeterminate=%llu\n",
              static_cast<unsigned long long>(timed.outcomes[0]),
              static_cast<unsigned long long>(timed.outcomes[1]),
              static_cast<unsigned long long>(timed.outcomes[2]),
              static_cast<unsigned long long>(timed.outcomes[3]));
  std::printf("  consumed work digest: 0x%016llx\n",
              static_cast<unsigned long long>(timed.checksum));
  std::printf("                        (outcomes, traces, evidence dependencies, intents and\n");
  std::printf("                        decision digests were read; iterations alone would not\n");
  std::printf("                        produce this value)\n");

  if (ledger_measured) {
    const std::int64_t ledger_mean = MeanNs(ledger.samples_ns);
    std::printf("\ndurable ledger -- REAL local filesystem, one flush to stable storage per commit\n");
    std::printf("  directory           : %s\n", ledger.directory.c_str());
    std::printf("  open                : %lld ns\n", static_cast<long long>(ledger.open_ns));
    std::printf("  commits completed   : %zu, journal sequence %llu..%llu\n", ledger.committed,
                static_cast<unsigned long long>(ledger.first_sequence),
                static_cast<unsigned long long>(ledger.last_sequence));
    std::printf("  per-commit mean     : %lld ns (%.3f ms), median %lld ns\n",
                static_cast<long long>(ledger_mean),
                static_cast<double>(ledger_mean) / 1000000.0,
                static_cast<long long>(ledger_median));
    std::printf("  journal             : %llu bytes, re-read and chain-verified: %llu frames\n",
                static_cast<unsigned long long>(ledger.journal_bytes),
                static_cast<unsigned long long>(ledger.verified_frames));
    std::printf("  consumed work digest: 0x%016llx\n",
                static_cast<unsigned long long>(ledger.checksum));
  } else {
    std::printf("\ndurable ledger -- REAL: not measured in this run; see the UNSUPPORTED lines above\n");
  }

  std::printf("\nlabels: REAL       = measured on this machine, real files or the real engine;\n");
  std::printf("        SYNTHETIC  = real engine work driven by hand-written model input;\n");
  std::printf("        UNSUPPORTED= not measurable here, with the reason stated above.\n");
  std::printf("The SYNTHETIC evidence is a model. It is not facility telemetry, and it must never\n");
  std::printf("be presented as a reading from real hardware, a BMS, or a DCIM.\n");
  return 0;
}
