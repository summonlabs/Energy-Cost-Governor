// Torn tails, zero-filled tails, interior corruption, and header damage.
//
// Each case is produced by damaging a real journal produced by the real ledger,
// so the recovery path is exercised against bytes the writer actually emitted.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ecg/hash.hpp"
#include "ecg/ledger.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

using LedgerPtr = std::unique_ptr<ecg::DecisionLedger>;

ecg::DecisionRequest ReserveRequest(const Scenario& scenario, std::uint64_t ordinal) {
  return MakeRequest(scenario, ordinal, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                     Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
}

bool ReadBytes(const std::filesystem::path& path, std::string* out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  *out = std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return true;
}

bool WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return stream.good();
}

/// Produces a journal holding the given number of committed decisions.
void BuildJournal(const std::filesystem::path& directory, const Scenario& scenario, int decisions) {
  const auto opened = ecg::DecisionLedger::Open(directory, scenario.policy, ecg::LedgerOptions{});
  if (!opened.ok()) {
    std::abort();
  }
  for (int i = 0; i < decisions; ++i) {
    const auto committed = opened.value()->Submit(ReserveRequest(scenario, 100 + static_cast<std::uint64_t>(i)),
                                                  scenario.evidence, scenario.now);
    if (!committed.ok()) {
      std::abort();
    }
  }
}

}  // namespace

ECG_TEST("recovery.truncated_final_frame_is_recovered_as_a_torn_tail") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_torn");
  BuildJournal(directory.path(), scenario, 3);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);
  const auto size = std::filesystem::file_size(journal);
  // Cut into the final frame, which is exactly what a crash during a write leaves.
  std::filesystem::resize_file(journal, size - 7);

  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy,
                                                  ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  ECG_CHECK(reopened.value()->recovery().tail_damaged);
  ECG_CHECK(reopened.value()->recovery().truncated_torn_tail);
  ECG_CHECK_EQ(reopened.value()->RecoveredDecisions().size(), std::size_t{2});
  ECG_CHECK_EQ(reopened.value()->last_sequence().raw(), std::uint64_t{2});

  // The recovered journal must keep working: the damaged tail is gone and new
  // commits continue the sequence.
  const auto committed = reopened.value()->Submit(ReserveRequest(scenario, 200), scenario.evidence,
                                                  scenario.now);
  ECG_REQUIRE(committed.ok());
  ECG_CHECK_EQ(committed.value().sequence.raw(), std::uint64_t{3});
  const auto report = reopened.value()->Verify();
  ECG_REQUIRE(report.ok());
  ECG_CHECK(!report.value().tail_damaged);
  ECG_CHECK_EQ(report.value().decisions, std::size_t{3});
}

ECG_TEST("recovery.zero_filled_tail_is_treated_as_a_torn_tail") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_zero");
  BuildJournal(directory.path(), scenario, 2);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::app);
    const char zeros[64] = {};
    stream.write(zeros, sizeof(zeros));
  }
  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy,
                                                  ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  ECG_CHECK(reopened.value()->recovery().tail_damaged);
  ECG_CHECK(reopened.value()->recovery().truncated_torn_tail);
  ECG_CHECK_EQ(reopened.value()->RecoveredDecisions().size(), std::size_t{2});
}

ECG_TEST("recovery.damaged_interior_frame_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_interior");
  BuildJournal(directory.path(), scenario, 3);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  std::string bytes;
  ECG_REQUIRE(ReadBytes(journal, &bytes));
  // Flip a byte well inside the first frame's payload, keeping the file length
  // identical so only a checksum can detect the damage.
  const std::size_t target = bytes.size() / 3;
  bytes[target] = static_cast<char>(bytes[target] ^ 0x5A);
  ECG_REQUIRE(WriteBytes(journal, bytes));

  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kInteriorCorruption);
}

ECG_TEST("recovery.damaged_final_frame_is_also_refused_rather_than_dropped") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_final");
  BuildJournal(directory.path(), scenario, 2);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  std::string bytes;
  ECG_REQUIRE(ReadBytes(journal, &bytes));
  // Damage the very last byte of the file: physically present, so it is treated
  // as damage to a committed record rather than as an absent tail.
  bytes.back() = static_cast<char>(bytes.back() ^ 0xFF);
  ECG_REQUIRE(WriteBytes(journal, bytes));

  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kInteriorCorruption);
}

ECG_TEST("recovery.verification_does_not_modify_the_file_it_checks") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_verify_readonly");
  BuildJournal(directory.path(), scenario, 2);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  std::string original;
  ECG_REQUIRE(ReadBytes(journal, &original));
  std::string damaged = original;
  damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x33);
  ECG_REQUIRE(WriteBytes(journal, damaged));

  // Opening refuses, so use a healthy journal to check that Verify is read-only.
  ECG_REQUIRE(WriteBytes(journal, original));
  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());
  std::string before;
  ECG_REQUIRE(ReadBytes(journal, &before));
  const auto report = opened.value()->Verify();
  ECG_REQUIRE(report.ok());
  std::string after;
  ECG_REQUIRE(ReadBytes(journal, &after));
  ECG_CHECK_EQ(before, after);
}

ECG_TEST("recovery.corrupted_magic_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_magic");
  BuildJournal(directory.path(), scenario, 1);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  std::string bytes;
  ECG_REQUIRE(ReadBytes(journal, &bytes));
  bytes[0] = 'X';
  ECG_REQUIRE(WriteBytes(journal, bytes));

  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("recovery.corrupted_header_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_header");
  BuildJournal(directory.path(), scenario, 1);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  std::string bytes;
  ECG_REQUIRE(ReadBytes(journal, &bytes));
  bytes[20] = static_cast<char>(bytes[20] ^ 0x11);
  ECG_REQUIRE(WriteBytes(journal, bytes));

  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("recovery.unsupported_journal_format_version_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_version");
  BuildJournal(directory.path(), scenario, 1);
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);

  // Rebuild a header that is internally consistent but declares a future format
  // version, so only the version check can reject it.
  std::string bytes;
  ECG_REQUIRE(ReadBytes(journal, &bytes));
  const std::uint32_t header_length =
      static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[8])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[9])) << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[10])) << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[11])) << 24);
  ECG_REQUIRE(header_length >= 4);
  std::string header = bytes.substr(16, header_length);
  header[0] = static_cast<char>(99);  // format version 99, little endian
  header[1] = 0;
  header[2] = 0;
  header[3] = 0;
  // Recompute the header checksum so the only thing wrong with this journal is
  // the format version it declares.
  const std::uint32_t crc = ecg::Crc32c(header);
  std::string rebuilt = bytes.substr(0, 12);
  rebuilt.push_back(static_cast<char>(crc & 0xFFu));
  rebuilt.push_back(static_cast<char>((crc >> 8) & 0xFFu));
  rebuilt.push_back(static_cast<char>((crc >> 16) & 0xFFu));
  rebuilt.push_back(static_cast<char>((crc >> 24) & 0xFFu));
  rebuilt.append(header);
  rebuilt.append(bytes.substr(16 + header_length));
  ECG_REQUIRE(WriteBytes(journal, rebuilt));

  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kUnsupportedVersion);
}

ECG_TEST("recovery.empty_file_is_initialised_as_a_fresh_journal") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_empty");
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);
  ECG_REQUIRE(WriteBytes(journal, std::string()));

  const auto opened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(opened.ok());
  ECG_CHECK_EQ(opened.value()->RecoveredDecisions().size(), std::size_t{0});
  const auto committed = opened.value()->Submit(ReserveRequest(scenario, 300), scenario.evidence,
                                                scenario.now);
  ECG_REQUIRE(committed.ok());
  ECG_CHECK_EQ(committed.value().sequence.raw(), std::uint64_t{1});
}

ECG_TEST("recovery.short_garbage_file_is_refused") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_garbage");
  const std::filesystem::path journal = directory.File(ecg::kJournalFileName);
  ECG_REQUIRE(WriteBytes(journal, "ECG"));
  ECG_CHECK_ERR(ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{}),
                ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("recovery.recovered_decisions_keep_their_content_and_order") {
  const Scenario scenario = MakeScenario();
  TempDir directory("recovery_order");
  BuildJournal(directory.path(), scenario, 4);
  const auto reopened = ecg::DecisionLedger::Open(directory.path(), scenario.policy, ecg::LedgerOptions{});
  ECG_REQUIRE(reopened.ok());
  const std::vector<ecg::CommittedDecision> decisions = reopened.value()->RecoveredDecisions();
  ECG_REQUIRE(decisions.size() == 4);
  for (std::size_t i = 0; i < decisions.size(); ++i) {
    ECG_CHECK_EQ(decisions[i].sequence.raw(), static_cast<std::uint64_t>(i + 1));
    // The recovered decision still verifies against its own content digest.
    ECG_CHECK(decisions[i].decision.id ==
              ecg::DecisionId::FromRaw(decisions[i].decision.digest.raw()));
  }
}
