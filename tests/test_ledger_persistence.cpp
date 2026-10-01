// Durable behaviour: the commit point, idempotency, epoch admission, snapshots,
// and real reopen.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"
#include "ecg/ledger.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

using LedgerPtr = std::unique_ptr<ecg::DecisionLedger>;

ecg::Result<LedgerPtr> OpenLedger(const std::filesystem::path& directory,
                                  const ecg::PolicySet& policy,
                                  ecg::LedgerOptions options = ecg::LedgerOptions{}) {
  return ecg::DecisionLedger::Open(directory, policy, options);
}

ecg::DecisionRequest ReserveRequest(const Scenario& scenario, std::uint64_t ordinal) {
  return MakeRequest(scenario, ordinal, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                     Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
}

}  // namespace

ECG_TEST("ledger.open_creates_a_journal_with_a_verifiable_header") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_open");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().frames_read, std::size_t{0});
  ECG_CHECK(!report.value().tail_damaged);
  ECG_CHECK(std::filesystem::exists(directory.File(ecg::kJournalFileName)));
  ECG_CHECK(std::filesystem::exists(directory.File(ecg::kLockFileName)));
}

ECG_TEST("ledger.commit_is_visible_after_a_real_reopen") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_reopen");
  {
    const auto opened = OpenLedger(directory.path(), scenario.policy);
    ECG_REQUIRE(opened.ok());
    const auto committed =
        opened.value()->Submit(ReserveRequest(scenario, 1), scenario.evidence, scenario.now);
    ECG_REQUIRE(committed.ok());
    ECG_CHECK_EQ(committed.value().sequence.raw(), std::uint64_t{1});
    ECG_CHECK(!committed.value().duplicate);
  }
  {
    const auto reopened = OpenLedger(directory.path(), scenario.policy);
    ECG_REQUIRE(reopened.ok());
    const std::vector<ecg::CommittedDecision> decisions = reopened.value()->RecoveredDecisions();
    ECG_REQUIRE(decisions.size() == 1);
    // The recovered decision is content-addressed: its identity is its digest.
    ECG_CHECK(decisions.front().decision.id ==
              ecg::DecisionId::FromRaw(decisions.front().decision.digest.raw()));
    ECG_CHECK(decisions.front().sequence.raw() == 1);
    const auto report = reopened.value()->Verify();
    ECG_REQUIRE(report.ok());
    ECG_CHECK_EQ(report.value().decisions, std::size_t{1});
    ECG_CHECK_EQ(report.value().frames_read, std::size_t{1});
  }
}

ECG_TEST("ledger.idempotency_returns_the_stored_decision") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_idempotent");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  const ecg::DecisionRequest request = ReserveRequest(scenario, 2);

  const auto first = opened.value()->Submit(request, scenario.evidence, scenario.now);
  ECG_REQUIRE(first.ok());
  ECG_CHECK(!first.value().duplicate);

  // The same identity arriving again -- even with a different evaluation instant
  // -- must return the committed decision rather than writing a second one.
  const auto second = opened.value()->Submit(
      request, scenario.evidence, ecg::UtcInstant::FromRaw(scenario.now.raw() + 60000000LL));
  ECG_REQUIRE(second.ok());
  ECG_CHECK(second.value().duplicate);
  ECG_CHECK(second.value().decision.id == first.value().decision.id);
  ECG_CHECK(second.value().sequence == first.value().sequence);

  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, std::size_t{1});
}

ECG_TEST("ledger.distinct_identities_produce_distinct_frames") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_distinct");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  for (std::uint64_t ordinal = 10; ordinal < 15; ++ordinal) {
    const auto committed =
        opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
    ECG_REQUIRE(committed.ok());
    ECG_CHECK(!committed.value().duplicate);
  }
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, std::size_t{5});
  ECG_CHECK_EQ(report.value().frames_read, std::size_t{5});
  ECG_CHECK_EQ(report.value().last_sequence.raw(), std::uint64_t{5});
}

ECG_TEST("ledger.stale_epoch_submission_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_epoch");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());

  const auto advanced = opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(2), scenario.now);
  ECG_REQUIRE(advanced.ok());
  ECG_CHECK_EQ(opened.value()->current_epoch().raw(), std::uint64_t{2});

  // The same request, still carrying epoch 1, is a replay of an older authority
  // epoch and must be refused rather than quietly evaluated.
  ECG_CHECK_ERR(opened.value()->Submit(ReserveRequest(scenario, 20), scenario.evidence, scenario.now),
                ecg::ErrorCode::kStaleReplayRejected);

  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, std::size_t{0});
  ECG_CHECK_EQ(report.value().epoch_advances, std::size_t{1});
}

ECG_TEST("ledger.epoch_must_advance_strictly") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_epoch_strict");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  ECG_CHECK_ERR(opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(1), scenario.now),
                ecg::ErrorCode::kStaleReplayRejected);
  ECG_REQUIRE(opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(5), scenario.now).ok());
  ECG_CHECK_ERR(opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(4), scenario.now),
                ecg::ErrorCode::kStaleReplayRejected);
  ECG_REQUIRE(opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(6), scenario.now).ok());
  ECG_CHECK_EQ(opened.value()->current_epoch().raw(), std::uint64_t{6});
}

ECG_TEST("ledger.epoch_survives_a_reopen") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_epoch_reopen");
  {
    const auto opened = OpenLedger(directory.path(), scenario.policy);
    ECG_REQUIRE(opened.ok());
    ECG_REQUIRE(opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(3), scenario.now).ok());
  }
  const auto reopened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(reopened.ok());
  ECG_CHECK_EQ(reopened.value()->current_epoch().raw(), std::uint64_t{3});
}

ECG_TEST("ledger.published_state_is_recovered_and_never_fresh") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_state");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());

  ecg::StateDocument state;
  state.epoch = opened.value()->current_epoch();
  state.policy = scenario.policy;
  state.evidence = scenario.evidence;
  state.updated_at = scenario.now;
  ECG_REQUIRE(opened.value()->PublishState(state, scenario.now).ok());

  const auto loaded = opened.value()->LoadState();
  ECG_REQUIRE(loaded.ok());
  ECG_REQUIRE(loaded.value().has_value());
  ECG_REQUIRE(loaded.value()->evidence.reserve.has_value());
  ECG_CHECK_EQ(loaded.value()->evidence.reserve->meta.provenance,
               ecg::Provenance::kRecoveredPersistence);
  ECG_CHECK_EQ(loaded.value()->evidence.tariff->meta.provenance,
               ecg::Provenance::kRecoveredPersistence);

  // A decision evaluated against recovered evidence must not be Allowed.
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());
  const ecg::Decision decision =
      EvaluateOrDie(engine.value(), ReserveRequest(scenario, 30), loaded.value()->evidence,
                    scenario.now);
  ECG_CHECK_EQ(decision.outcome, ecg::Outcome::kIndeterminate);
  ECG_CHECK(HasReason(decision, ecg::ReasonCode::kEvidenceRecovered));

  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().snapshots, std::size_t{1});
}

ECG_TEST("ledger.snapshot_survives_a_reopen_and_the_journal_stays_consistent") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_state_reopen");
  {
    const auto opened = OpenLedger(directory.path(), scenario.policy);
    ECG_REQUIRE(opened.ok());
    ecg::StateDocument state;
    state.epoch = opened.value()->current_epoch();
    state.policy = scenario.policy;
    state.evidence = scenario.evidence;
    state.updated_at = scenario.now;
    ECG_REQUIRE(opened.value()->PublishState(state, scenario.now).ok());
  }
  const auto reopened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(reopened.ok());
  const auto loaded = reopened.value()->LoadState();
  ECG_REQUIRE(loaded.ok());
  ECG_CHECK(loaded.value().has_value());
  ECG_CHECK(reopened.value()->recovery().snapshot_present);
}

ECG_TEST("ledger.acknowledgement_is_recorded_and_is_not_an_effect") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_ack");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  const auto committed =
      opened.value()->Submit(ReserveRequest(scenario, 40), scenario.evidence, scenario.now);
  ECG_REQUIRE(committed.ok());
  ECG_REQUIRE(committed.value().decision.intent.has_value());

  ecg::IntentAcknowledgement acknowledgement;
  acknowledgement.intent_id = committed.value().decision.intent->id;
  acknowledgement.decision_id = committed.value().decision.id;
  acknowledgement.acknowledged_by = ecg::ClientId::FromTrusted("capacity.authority");
  acknowledgement.acknowledged_at = ecg::UtcInstant::FromRaw(scenario.now.raw() + 1000000);
  acknowledgement.note = "queued by the adjacent authority";
  ECG_REQUIRE(opened.value()->RecordAcknowledgement(acknowledgement).ok());

  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().acknowledgements, std::size_t{1});
  // The acknowledgement does not create, extend, or re-authorise anything: the
  // decision count is unchanged and no effect record exists anywhere.
  ECG_CHECK_EQ(report.value().decisions, std::size_t{1});
}

ECG_TEST("ledger.journal_size_bound_is_enforced_before_writing") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_bound");
  ecg::LedgerOptions options;
  options.max_journal_bytes = 512;
  const auto opened = OpenLedger(directory.path(), scenario.policy, options);
  ECG_REQUIRE(opened.ok());
  bool refused = false;
  for (std::uint64_t ordinal = 50; ordinal < 60; ++ordinal) {
    const auto committed =
        opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
    if (!committed.ok()) {
      refused = committed.error().code() == ecg::ErrorCode::kResourceLimitExceeded;
      break;
    }
  }
  ECG_CHECK(refused);
}

ECG_TEST("ledger.closed_ledger_refuses_further_writes") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_closed");
  const auto opened = OpenLedger(directory.path(), scenario.policy);
  ECG_REQUIRE(opened.ok());
  ECG_REQUIRE(opened.value()->Close().ok());
  ECG_CHECK_ERR(opened.value()->Commit(ecg::Decision{}, scenario.now), ecg::ErrorCode::kIoFailure);
  // Close is idempotent.
  ECG_REQUIRE(opened.value()->Close().ok());
}

ECG_TEST("ledger.lock_is_released_on_close_and_reacquirable") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_lock_release");
  {
    const auto opened = OpenLedger(directory.path(), scenario.policy);
    ECG_REQUIRE(opened.ok());
  }
  const auto again = OpenLedger(directory.path(), scenario.policy);
  ECG_CHECK(again.ok());
}

ECG_TEST("ledger.missing_directory_without_create_is_reported") {
  const Scenario scenario = MakeScenario();
  TempDir directory("persistence_missing");
  ecg::LedgerOptions options;
  options.create_if_missing = false;
  ECG_CHECK_ERR(OpenLedger(directory.File("nested"), scenario.policy, options),
                ecg::ErrorCode::kNotFound);
}
