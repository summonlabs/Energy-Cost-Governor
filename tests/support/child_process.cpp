// Helper process for the multiprocess, lock, and restart tests.
//
// This is a real executable that links the real library and is launched as a
// separate operating-system process. It deliberately shares no memory with its
// parent: the only channel between them is the ledger directory and the result
// file it is told to write.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/ledger.hpp"
#include "ecg/policy.hpp"

namespace {

using ecg::ClientId;
using ecg::DecisionLedger;
using ecg::DecisionRequest;
using ecg::DurationSec;
using ecg::Epoch;
using ecg::EvidenceSet;
using ecg::Interval;
using ecg::LedgerOptions;
using ecg::ParseRfc3339;
using ecg::PolicySet;
using ecg::PowerKw;
using ecg::RequestId;
using ecg::RequestKind;
using ecg::UtcInstant;

constexpr int kOk = 0;
constexpr int kError = 1;
constexpr int kLockUnavailable = 6;

[[nodiscard]] UtcInstant Now() { return ParseRfc3339("2026-03-02T12:00:00.000000Z").value(); }

[[nodiscard]] bool WriteResult(const std::string& path, const std::string& text) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
  const bool ok = written == text.size() && std::fclose(file) == 0;
  return ok;
}

/// Minimal, self-consistent synthetic evidence sufficient for a
/// reserve-restoration decision. No facility hardware produced any of it.
[[nodiscard]] EvidenceSet MakeEvidence(Epoch epoch) {
  const UtcInstant now = Now();
  const UtcInstant observed = UtcInstant::FromRaw(now.raw() - 60000000LL);
  EvidenceSet evidence;
  evidence.epoch = epoch;

  ecg::Evidence<ecg::ReserveState> reserve;
  reserve.meta.authority = ecg::AuthorityKind::kReserveAuthority;
  reserve.meta.source = ecg::SourceId::FromTrusted("reserve.authority");
  reserve.meta.generation = ecg::EvidenceGeneration::FromRaw(1);
  reserve.meta.epoch = epoch;
  reserve.meta.observed_at = observed;
  reserve.meta.published_at = observed;
  reserve.meta.provenance = ecg::Provenance::kSyntheticModel;
  reserve.value.current_reserve_kw = PowerKw::FromRaw(5000000);
  reserve.value.reserve_floor_kw = PowerKw::FromRaw(1000000);
  reserve.value.capacity_kw = PowerKw::FromRaw(12000000);
  reserve.value.reserve_floor_ratio = ecg::RatioPpm::FromRaw(100000);
  evidence.reserve = reserve;

  ecg::Evidence<ecg::CapacityState> capacity;
  capacity.meta = reserve.meta;
  capacity.meta.authority = ecg::AuthorityKind::kCapacityAuthority;
  capacity.meta.source = ecg::SourceId::FromTrusted("capacity.authority");
  capacity.meta.generation = ecg::EvidenceGeneration::FromRaw(2);
  capacity.value.flexible_load_kw = PowerKw::FromRaw(5000000);
  capacity.value.shiftable_headroom_kw = PowerKw::FromRaw(5000000);
  evidence.capacity = capacity;

  ecg::Evidence<ecg::RiskState> risk;
  risk.meta = reserve.meta;
  risk.meta.authority = ecg::AuthorityKind::kRiskAuthority;
  risk.meta.source = ecg::SourceId::FromTrusted("risk.authority");
  risk.meta.generation = ecg::EvidenceGeneration::FromRaw(3);
  risk.value.posture = ecg::RiskPosture::kNormal;
  risk.value.max_ramp_kw_per_min = ecg::PowerKwPerMin::FromRaw(500000);
  risk.value.last_change_at = UtcInstant::FromRaw(0);
  evidence.risk = risk;

  ecg::Evidence<ecg::IncidentState> incident;
  incident.meta = reserve.meta;
  incident.meta.authority = ecg::AuthorityKind::kIncidentAuthority;
  incident.meta.source = ecg::SourceId::FromTrusted("incident.authority");
  incident.meta.generation = ecg::EvidenceGeneration::FromRaw(4);
  incident.value.severity = ecg::IncidentSeverity::kNone;
  incident.value.declared_at = UtcInstant::FromRaw(0);
  evidence.incident = incident;

  return evidence;
}

[[nodiscard]] DecisionRequest MakeRequest(Epoch epoch, ecg::PolicyGeneration generation,
                                          std::uint64_t request_id) {
  DecisionRequest request;
  request.request_id = RequestId::FromRaw(request_id);
  request.client_id = ClientId::FromTrusted("child.process");
  request.epoch = epoch;
  request.policy_generation = generation;
  request.kind = RequestKind::kReserveRestoration;
  request.magnitude_kw = PowerKw::FromRaw(400000);
  request.duration = DurationSec::FromRaw(600);
  request.desired_window =
      Interval::Make(ParseRfc3339("2026-03-02T12:05:00.000000Z").value(),
                     ParseRfc3339("2026-03-02T12:15:00.000000Z").value())
          .value();
  request.rationale = ecg::Rationale::kReserveRestoration;
  request.requested_at = Now();
  return request;
}

int CommandLockProbe(const std::vector<std::string>& arguments) {
  const auto opened = DecisionLedger::Open(arguments[0], ecg::DefaultPolicy(), LedgerOptions{});
  if (opened.ok()) {
    (void)WriteResult(arguments[1], "acquired\n");
    // Release the lock immediately by letting the ledger go out of scope.
    return kOk;
  }
  if (opened.error().code() == ecg::ErrorCode::kLockUnavailable) {
    (void)WriteResult(arguments[1], "busy\n");
    return kLockUnavailable;
  }
  (void)WriteResult(arguments[1], "error:" + opened.error().ToString() + "\n");
  return kError;
}

int CommandSubmit(const std::vector<std::string>& arguments) {
  const std::uint64_t request_id = std::strtoull(arguments[1].c_str(), nullptr, 10);
  PolicySet policy = ecg::DefaultPolicy();
  const auto opened = DecisionLedger::Open(arguments[0], policy, LedgerOptions{});
  if (!opened.ok()) {
    (void)WriteResult(arguments[2], "error:" + opened.error().ToString() + "\n");
    return opened.error().code() == ecg::ErrorCode::kLockUnavailable ? kLockUnavailable : kError;
  }
  const auto committed =
      opened.value()->Submit(MakeRequest(policy.epoch, policy.generation, request_id),
                             MakeEvidence(policy.epoch), Now());
  if (!committed.ok()) {
    (void)WriteResult(arguments[2], "error:" + committed.error().ToString() + "\n");
    return kError;
  }
  (void)WriteResult(arguments[2], std::string("committed ") +
                                ecg::OutcomeName(committed.value().decision.outcome) + " seq=" +
                                committed.value().sequence.ToString() + " duplicate=" +
                                (committed.value().duplicate ? "true" : "false") + "\n");
  return kOk;
}

/// Commits a decision and then terminates the process without running any
/// destructor, which is the closest model of an abrupt process death.
int CommandAbruptExit(const std::vector<std::string>& arguments) {
  const std::uint64_t request_id = std::strtoull(arguments[1].c_str(), nullptr, 10);
  PolicySet policy = ecg::DefaultPolicy();
  const auto opened = DecisionLedger::Open(arguments[0], policy, LedgerOptions{});
  if (!opened.ok()) {
    return kError;
  }
  const auto committed = opened.value()->Submit(MakeRequest(policy.epoch, policy.generation, request_id),
                                                MakeEvidence(policy.epoch), Now());
  if (!committed.ok()) {
    return kError;
  }
  std::fflush(nullptr);
  std::_Exit(kOk);
}

int CommandReadState(const std::vector<std::string>& arguments) {
  const auto opened = DecisionLedger::Open(arguments[0], ecg::DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    (void)WriteResult(arguments[1], "error:" + opened.error().ToString() + "\n");
    return kError;
  }
  const auto state = opened.value()->LoadState();
  if (!state.ok()) {
    (void)WriteResult(arguments[1], "error:" + state.error().ToString() + "\n");
    return kError;
  }
  if (!state.value().has_value()) {
    (void)WriteResult(arguments[1], "absent\n");
    return kOk;
  }
  const std::string provenance =
      state.value()->evidence.reserve.has_value()
          ? std::string(ecg::ProvenanceName(state.value()->evidence.reserve->meta.provenance))
          : std::string("no-reserve");
  (void)WriteResult(arguments[1], "epoch=" + state.value()->epoch.ToString() + " provenance=" + provenance +
                                " decisions=" +
                                std::to_string(opened.value()->RecoveredDecisions().size()) + "\n");
  return kOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (arguments.empty()) {
    std::fprintf(stderr, "child_process: missing command\n");
    return kError;
  }
  const std::string& command = arguments[0];
  if (command == "lock-probe" && arguments.size() == 3) {
    return CommandLockProbe({arguments[1], arguments[2]});
  }
  if (command == "submit" && arguments.size() == 4) {
    return CommandSubmit({arguments[1], arguments[2], arguments[3]});
  }
  if (command == "abrupt-exit" && arguments.size() == 3) {
    return CommandAbruptExit({arguments[1], arguments[2]});
  }
  if (command == "read-state" && arguments.size() == 3) {
    return CommandReadState({arguments[1], arguments[2]});
  }
  std::fprintf(stderr, "child_process: unknown command or wrong argument count\n");
  return kError;
}
