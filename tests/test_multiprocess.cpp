// Real multiprocess behaviour: kernel-enforced single-writer ownership, abrupt
// process death, and durability observed by a different process.

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ecg/ledger.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

std::filesystem::path ChildExecutable() {
  const char* value = std::getenv("ECG_TEST_CHILD");
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return std::filesystem::path(value);
}

/// True when the executable needed for this test is available.
bool ChildAvailable(const std::filesystem::path& child) { return !child.empty(); }

std::string ReadResult(const std::filesystem::path& path) {
  std::string text;
  // A missing result file simply means the child wrote nothing; that is itself
  // the observation the assertions below depend on.
  (void)ReadTextFile(path, &text);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

}  // namespace

ECG_TEST("multiprocess.second_process_is_refused_while_the_lock_is_held") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_busy");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  const std::filesystem::path result = directory.File("probe.txt");
  const int exit_code = RunChildProcess(child, {"lock-probe", directory.path().string(),
                                                result.string()});
  ECG_CHECK_EQ(exit_code, 6);
  ECG_CHECK_EQ(ReadResult(result), std::string("busy"));
}

ECG_TEST("multiprocess.several_processes_are_all_refused_while_one_holds_the_lock") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_many");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());
  for (int index = 0; index < 4; ++index) {
    const std::filesystem::path result =
        directory.File("probe" + std::to_string(index) + ".txt");
    const int exit_code =
        RunChildProcess(child, {"lock-probe", directory.path().string(), result.string()});
    ECG_CHECK_EQ(exit_code, 6);
    ECG_CHECK_EQ(ReadResult(result), std::string("busy"));
  }
}

ECG_TEST("multiprocess.lock_is_available_once_the_holder_exits") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_release");
  {
    const auto opened =
        ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
    ECG_REQUIRE(opened.ok());
  }
  const std::filesystem::path result = directory.File("probe.txt");
  const int exit_code =
      RunChildProcess(child, {"lock-probe", directory.path().string(), result.string()});
  ECG_CHECK_EQ(exit_code, 0);
  ECG_CHECK_EQ(ReadResult(result), std::string("acquired"));
}

ECG_TEST("multiprocess.decision_committed_by_another_process_is_visible_here") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_submit");
  const std::filesystem::path result = directory.File("submit.txt");
  const int exit_code = RunChildProcess(
      child, {"submit", directory.path().string(), "31337", result.string()});
  ECG_CHECK_EQ(exit_code, 0);
  ECG_CHECK_EQ(ReadResult(result).rfind("committed ", 0), std::size_t{0});

  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  const std::vector<ecg::CommittedDecision> decisions = reopened.value()->RecoveredDecisions();
  ECG_REQUIRE(decisions.size() == 1);
  ECG_CHECK_EQ(decisions.front().decision.request_id.raw(), std::uint64_t{31337});
  const auto report = reopened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK(!report.value().tail_damaged);
}

ECG_TEST("multiprocess.abrupt_process_death_leaves_a_verifiable_journal") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_abrupt");
  const int exit_code =
      RunChildProcess(child, {"abrupt-exit", directory.path().string(), "5150"});
  ECG_CHECK_EQ(exit_code, 0);

  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  const auto report = reopened.value()->Verify();
  ECG_REQUIRE(report.ok());
  // Either the commit completed before the process died, or it did not exist at
  // all. What must never happen is a half-written record being accepted.
  ECG_CHECK(!report.value().tail_damaged);
  const std::vector<ecg::CommittedDecision> decisions = reopened.value()->RecoveredDecisions();
  ECG_CHECK_EQ(decisions.size(), report.value().decisions);
  for (const ecg::CommittedDecision& committed : decisions) {
    ECG_CHECK(committed.decision.id == ecg::DecisionId::FromRaw(committed.decision.digest.raw()));
  }
}

ECG_TEST("multiprocess.sequential_processes_keep_the_sequence_contiguous") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_sequence");
  for (int index = 0; index < 5; ++index) {
    const std::filesystem::path result = directory.File("submit" + std::to_string(index) + ".txt");
    const int exit_code = RunChildProcess(
        child, {"submit", directory.path().string(), std::to_string(600 + index), result.string()});
    ECG_CHECK_EQ(exit_code, 0);
  }

  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  const std::vector<ecg::CommittedDecision> decisions = reopened.value()->RecoveredDecisions();
  ECG_REQUIRE(decisions.size() == 5);
  std::uint64_t expected = 1;
  for (const ecg::CommittedDecision& committed : decisions) {
    ECG_CHECK_EQ(committed.sequence.raw(), expected);
    ++expected;
  }
}

ECG_TEST("multiprocess.state_written_by_one_process_is_recovered_by_another") {
  const std::filesystem::path child = ChildExecutable();
  ECG_REQUIRE(ChildAvailable(child));
  const Scenario scenario = MakeScenario();
  TempDir directory("multiprocess_state");
  {
    const auto opened =
        ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
    ECG_REQUIRE(opened.ok());
    ecg::StateDocument state;
    state.epoch = opened.value()->current_epoch();
    state.policy = scenario.policy;
    state.evidence = scenario.evidence;
    state.updated_at = scenario.now;
    ECG_REQUIRE(opened.value()->PublishState(state, scenario.now).ok());
  }
  const std::filesystem::path result = directory.File("state.txt");
  const int exit_code =
      RunChildProcess(child, {"read-state", directory.path().string(), result.string()});
  ECG_CHECK_EQ(exit_code, 0);
  const std::string text = ReadResult(result);
  ECG_CHECK(text.find("provenance=recovered_persistence") != std::string::npos);
  ECG_CHECK(text.find("epoch=1") != std::string::npos);
}
