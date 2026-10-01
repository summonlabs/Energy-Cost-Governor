// Concurrency: shared-read evaluation, serialised writer ownership, idempotency
// under a race, and epoch admission while other threads are writing.
//
// There are no timeouts here by design. A hang in any of these tests is a real
// defect and must be diagnosed, not masked by killing the test.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"
#include "ecg/ledger.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

ecg::DecisionRequest ReserveRequest(const Scenario& scenario, std::uint64_t ordinal) {
  return MakeRequest(scenario, ordinal, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                     Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
}

std::string DecisionJson(const ecg::Decision& decision) { return ecg::ToJson(decision).Dump(); }

}  // namespace

ECG_TEST("concurrency.engine_evaluation_is_shareable_across_threads") {
  const Scenario scenario = MakeScenario();
  const auto engine = ecg::GovernorEngine::Make(scenario.policy);
  ECG_REQUIRE(engine.ok());

  std::vector<ecg::DecisionRequest> requests;
  for (std::uint64_t ordinal = 1; ordinal <= 8; ++ordinal) {
    requests.push_back(MakeRequest(scenario, ordinal, ecg::RequestKind::kShiftFlexibleLoad, Kw(400),
                                   Secs(1800),
                                   Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"),
                                   "vm.flex"));
  }

  std::vector<std::string> expected;
  for (const ecg::DecisionRequest& request : requests) {
    expected.push_back(DecisionJson(EvaluateOrDie(engine.value(), request, scenario.evidence,
                                                  scenario.now)));
  }

  constexpr int kThreads = 8;
  constexpr int kRounds = 40;
  std::atomic<int> mismatches{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&engine, &requests, &expected, &scenario, &mismatches]() {
      for (int round = 0; round < kRounds; ++round) {
        for (std::size_t i = 0; i < requests.size(); ++i) {
          const auto decision = engine.value().Evaluate(requests[i], scenario.evidence, scenario.now);
          if (!decision.ok() || DecisionJson(decision.value()) != expected[i]) {
            mismatches.fetch_add(1);
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  ECG_CHECK_EQ(mismatches.load(), 0);
}

ECG_TEST("concurrency.distinct_submissions_from_many_threads_all_commit_once") {
  const Scenario scenario = MakeScenario();
  TempDir directory("concurrency_distinct");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  constexpr int kThreads = 6;
  constexpr int kPerThread = 12;
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&opened, &scenario, &failures, thread_index]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::uint64_t ordinal =
            static_cast<std::uint64_t>(1000 + thread_index * kPerThread + i);
        const auto committed =
            opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
        if (!committed.ok() || committed.value().duplicate) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  ECG_CHECK_EQ(failures.load(), 0);

  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, static_cast<std::size_t>(kThreads * kPerThread));
  ECG_CHECK_EQ(opened.value()->RecoveredDecisions().size(),
               static_cast<std::size_t>(kThreads * kPerThread));
  // Sequences are contiguous from one: no gaps and no duplicates.
  std::uint64_t expected_sequence = 1;
  for (const ecg::CommittedDecision& committed : opened.value()->RecoveredDecisions()) {
    ECG_CHECK_EQ(committed.sequence.raw(), expected_sequence);
    ++expected_sequence;
  }
}

ECG_TEST("concurrency.identical_submissions_race_to_exactly_one_commit") {
  const Scenario scenario = MakeScenario();
  TempDir directory("concurrency_identical");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  constexpr int kThreads = 8;
  std::atomic<int> fresh{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> failures{0};
  std::vector<std::string> decision_ids(kThreads);
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&opened, &scenario, &fresh, &duplicates, &failures, &decision_ids,
                          thread_index]() {
      const auto committed =
          opened.value()->Submit(ReserveRequest(scenario, 4242), scenario.evidence, scenario.now);
      if (!committed.ok()) {
        failures.fetch_add(1);
        return;
      }
      decision_ids[static_cast<std::size_t>(thread_index)] = committed.value().decision.id.ToString();
      if (committed.value().duplicate) {
        duplicates.fetch_add(1);
      } else {
        fresh.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  ECG_CHECK_EQ(failures.load(), 0);
  ECG_CHECK_EQ(fresh.load(), 1);
  ECG_CHECK_EQ(duplicates.load(), kThreads - 1);
  for (const std::string& id : decision_ids) {
    ECG_CHECK_EQ(id, decision_ids.front());
  }
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, std::size_t{1});
}

ECG_TEST("concurrency.epoch_advance_never_admits_a_stale_decision") {
  const Scenario scenario = MakeScenario();
  TempDir directory("concurrency_epoch");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  constexpr int kSubmitters = 4;
  constexpr int kPerSubmitter = 15;
  std::atomic<int> admitted{0};
  std::atomic<int> refused{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> workers;
  workers.reserve(kSubmitters + 1);
  for (int thread_index = 0; thread_index < kSubmitters; ++thread_index) {
    workers.emplace_back([&opened, &scenario, &admitted, &refused, &unexpected, thread_index]() {
      for (int i = 0; i < kPerSubmitter; ++i) {
        const std::uint64_t ordinal = static_cast<std::uint64_t>(7000 + thread_index * kPerSubmitter + i);
        const auto committed =
            opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
        if (committed.ok()) {
          admitted.fetch_add(1);
        } else if (committed.error().code() == ecg::ErrorCode::kStaleReplayRejected) {
          refused.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }
  workers.emplace_back([&opened, &scenario, &unexpected]() {
    for (std::uint64_t epoch = 2; epoch <= 6; ++epoch) {
      const auto status = opened.value()->AdvanceEpoch(ecg::Epoch::FromRaw(epoch), scenario.now);
      if (!status.ok() && status.error().code() != ecg::ErrorCode::kStaleReplayRejected) {
        unexpected.fetch_add(1);
      }
    }
  });
  for (std::thread& worker : workers) {
    worker.join();
  }
  ECG_CHECK_EQ(unexpected.load(), 0);
  ECG_CHECK_EQ(admitted.load() + refused.load(), kSubmitters * kPerSubmitter);

  // Whatever the interleaving, every committed decision carries the epoch that
  // was in force when it was appended, and the journal verifies.
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK_EQ(report.value().decisions, static_cast<std::size_t>(admitted.load()));
}

ECG_TEST("concurrency.verification_while_writing_stays_consistent") {
  const Scenario scenario = MakeScenario();
  TempDir directory("concurrency_verify");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  constexpr int kCommits = 60;
  std::atomic<bool> writer_done{false};
  std::atomic<int> verify_failures{0};
  std::atomic<int> submit_failures{0};
  std::atomic<int> verify_rounds{0};

  std::thread writer([&opened, &scenario, &writer_done, &submit_failures]() {
    for (int i = 0; i < kCommits; ++i) {
      const std::uint64_t ordinal = static_cast<std::uint64_t>(9000 + i);
      const auto committed =
          opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
      if (!committed.ok()) {
        submit_failures.fetch_add(1);
      }
    }
    writer_done.store(true);
  });

  // The verifier runs until the writer has finished, so the two operations are
  // genuinely concurrent and the writer is guaranteed to make progress.
  std::thread verifier([&opened, &writer_done, &verify_failures, &verify_rounds]() {
    while (!writer_done.load()) {
      const auto report = opened.value()->Verify();
      if (!report.ok() || report.value().tail_damaged) {
        verify_failures.fetch_add(1);
      }
      verify_rounds.fetch_add(1);
    }
  });
  writer.join();
  verifier.join();

  ECG_CHECK_EQ(submit_failures.load(), 0);
  ECG_CHECK_EQ(verify_failures.load(), 0);
  ECG_CHECK(verify_rounds.load() > 0);
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK(report.value().decisions > 0);
}

ECG_TEST("concurrency.close_is_safe_after_workers_stop") {
  const Scenario scenario = MakeScenario();
  TempDir directory("concurrency_close");
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());

  std::vector<std::thread> workers;
  for (int thread_index = 0; thread_index < 4; ++thread_index) {
    workers.emplace_back([&opened, &scenario, thread_index]() {
      for (int i = 0; i < 10; ++i) {
        const std::uint64_t ordinal = static_cast<std::uint64_t>(9500 + thread_index * 10 + i);
        const auto committed =
            opened.value()->Submit(ReserveRequest(scenario, ordinal), scenario.evidence, scenario.now);
        (void)committed;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Closing with no worker holding the ledger must succeed and be idempotent.
  ECG_REQUIRE(opened.value()->Close().ok());
  ECG_REQUIRE(opened.value()->Close().ok());
  // A closed ledger refuses new work rather than writing into a released file.
  const auto rejected =
      opened.value()->Submit(ReserveRequest(scenario, 9999), scenario.evidence, scenario.now);
  ECG_CHECK(!rejected.ok());
}
