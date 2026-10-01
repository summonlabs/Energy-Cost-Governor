#ifndef ECG_LEDGER_HPP
#define ECG_LEDGER_HPP

// The durable decision ledger.
//
// A decision that was made and then forgotten is an operational liability: the
// same request arrives again, gets answered differently because a meter reading
// moved, and an action is taken twice. The ledger removes that class of failure
// and defines exactly where a decision becomes durable.
//
// Guarantees, each of which is proved by a test:
//
//   Commit point. A record is committed when its frame is fully written to the
//   journal and flushed to stable storage. Nothing is durable before that, and
//   everything durable after it. The published state snapshot is a derived
//   artefact: it is written to a temporary file and renamed over the previous
//   snapshot, so a reader never observes a half-written snapshot.
//
//   Recovered is not fresh. State read back from the journal or the snapshot is
//   returned with provenance kRecoveredPersistence. The engine refuses to
//   authorise anything on recovered dynamic evidence, so a restart cannot
//   resurrect a stale reading as current truth.
//
//   Torn tail versus interior corruption. Truncation of the final frame is
//   expected after a crash and is recovered by discarding the incomplete tail.
//   A damaged frame that is followed by a valid, chain-linked frame is interior
//   corruption, and the ledger refuses to open rather than silently dropping
//   history.
//
//   Idempotency identity travels in the same commit as the mutation. The
//   (client, request) key is stored inside the decision frame itself, so a retry
//   after a crash either finds the original decision or re-evaluates it; it can
//   never append the same decision twice.
//
//   Replay is rejected. Records carry the authority epoch. A journal whose
//   history moves backwards in epoch, and a submission carrying a stale epoch,
//   are both refused.
//
//   One writer per machine. A kernel-enforced exclusive lock is held for the
//   lifetime of an open ledger. A second process is refused immediately instead
//   of corrupting the journal.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/engine.hpp"
#include "ecg/evidence.hpp"
#include "ecg/json_io.hpp"
#include "ecg/policy.hpp"
#include "ecg/request.hpp"
#include "ecg/strong.hpp"

namespace ecg {

/// Journal geometry constants. Changing any of them is a format break.
inline constexpr char kJournalMagic[8] = {'E', 'C', 'G', 'J', 'R', 'N', 'L', '\x01'};
inline constexpr std::uint32_t kMaxJournalFrameBytes = 8u << 20;
inline constexpr std::size_t kMaxTornTailScan = 4u << 20;

/// Names of the files that make up a ledger directory.
inline constexpr const char* kJournalFileName = "journal.ecgj";
inline constexpr const char* kLockFileName = "writer.lock";
inline constexpr const char* kSnapshotFileName = "state.json";
inline constexpr const char* kSnapshotTempName = "state.json.tmp";

/// A record is one of these. Values are persisted and must not be renumbered.
enum class LedgerRecordKind : std::uint8_t {
  kEpochAdvance = 1,
  kStateSnapshot = 2,
  kDecision = 3,
  kIntentAcknowledgement = 4,
};

[[nodiscard]] const char* LedgerRecordKindName(LedgerRecordKind kind) noexcept;

/// An acknowledgement that another authority received an intent.
///
/// Recording an acknowledgement is not recording an effect. This runtime cannot
/// observe whether load actually moved, and the ledger never derives an effect
/// from an acknowledgement.
struct IntentAcknowledgement {
  IntentId intent_id;
  DecisionId decision_id;
  ClientId acknowledged_by;
  UtcInstant acknowledged_at;
  std::string note;
};

/// One durable entry.
struct LedgerRecord {
  JournalSequence sequence;
  LedgerRecordKind kind{LedgerRecordKind::kDecision};
  Epoch epoch;
  UtcInstant recorded_at;
  ClientId client_id;
  RequestId request_id;
  std::optional<Decision> decision;
  std::optional<StateDocument> state;
  std::optional<IntentAcknowledgement> acknowledgement;
  std::optional<Epoch> previous_epoch;
};

/// What recovery found. Reported, never hidden.
struct RecoveryReport {
  std::size_t frames_read{0};
  std::size_t decisions{0};
  std::size_t acknowledgements{0};
  std::size_t epoch_advances{0};
  std::size_t snapshots{0};
  /// True when the journal ended in an incomplete or damaged final frame.
  bool tail_damaged{false};
  /// True when that tail was actually removed from the file.
  bool truncated_torn_tail{false};
  std::size_t torn_tail_bytes_discarded{0};
  JournalSequence last_sequence;
  UtcInstant last_recorded_at;
  Epoch current_epoch;
  bool snapshot_present{false};
  JournalSequence snapshot_sequence;
};

/// A decision together with its durable position.
struct CommittedDecision {
  Decision decision;
  JournalSequence sequence;
  /// True when an identical (client, request) identity was already committed and
  /// the stored decision was returned instead of a new one being written.
  bool duplicate{false};
};

struct LedgerOptions {
  /// Create the directory and journal when they do not exist.
  bool create_if_missing{true};
  /// Refuse to append when the journal exceeds this size.
  std::size_t max_journal_bytes{256u << 20};
  /// Refuse to open when a damaged region larger than this must be scanned to
  /// distinguish a torn tail from interior corruption.
  std::size_t max_torn_tail_scan{kMaxTornTailScan};
};

class DecisionLedger {
 public:
  DecisionLedger(const DecisionLedger&) = delete;
  DecisionLedger& operator=(const DecisionLedger&) = delete;
  DecisionLedger(DecisionLedger&&) = delete;
  DecisionLedger& operator=(DecisionLedger&&) = delete;
  ~DecisionLedger();

  /// Opens (or creates) the ledger in a directory, taking the single-writer lock
  /// and recovering any torn tail. Fails with kLockUnavailable when another
  /// process holds the lock, and with kInteriorCorruption when damaged history is
  /// followed by valid history.
  [[nodiscard]] static Result<std::unique_ptr<DecisionLedger>> Open(
      const std::filesystem::path& directory, PolicySet policy, LedgerOptions options = {});

  /// Evaluates a request and commits the resulting decision unless an identical
  /// (client, request) identity has already been committed. Evaluation happens
  /// outside the writer lock; only the dedupe check and the append are serialised.
  [[nodiscard]] Result<CommittedDecision> Submit(const DecisionRequest& request,
                                                 const EvidenceSet& evidence, UtcInstant now);

  /// Records a decision that was produced elsewhere (for example by a batch
  /// evaluator) using the same idempotency and epoch rules.
  [[nodiscard]] Result<CommittedDecision> Commit(const Decision& decision, UtcInstant now);

  /// Advances the authority epoch. The new epoch must be strictly greater than
  /// the current one; the advance is durable before this call returns.
  [[nodiscard]] Status AdvanceEpoch(Epoch next, UtcInstant now);

  /// Publishes a state snapshot: written to a temporary file, flushed, and
  /// renamed over the previous snapshot.
  [[nodiscard]] Status PublishState(const StateDocument& state, UtcInstant now);

  /// Records an intent acknowledgement. This never implies an effect.
  [[nodiscard]] Status RecordAcknowledgement(const IntentAcknowledgement& acknowledgement);

  /// Loads the published snapshot, marking its evidence as recovered so it can
  /// never authorise anything.
  [[nodiscard]] Result<std::optional<StateDocument>> LoadState() const;

  /// Re-reads the journal end to end and verifies every frame and chain link.
  /// Verification never modifies the file it is checking.
  [[nodiscard]] Result<RecoveryReport> Verify();

  /// Every decision recovered from the journal, in sequence order. Read-only.
  [[nodiscard]] std::vector<CommittedDecision> RecoveredDecisions() const;

  // The accessors below return by value and take the writer lock, because every
  // one of these fields is mutated by a concurrent append. Returning a reference
  // or reading the field directly would be a data race, not a micro-optimisation.
  [[nodiscard]] RecoveryReport recovery() const;
  [[nodiscard]] Epoch current_epoch() const;
  [[nodiscard]] JournalSequence last_sequence() const;

  /// Immutable after construction, so no lock is required.
  [[nodiscard]] const PolicySet& policy() const noexcept { return engine_.policy(); }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

  /// Flushes and releases the writer lock. Idempotent.
  [[nodiscard]] Status Close();

 private:
  DecisionLedger(std::filesystem::path directory, GovernorEngine engine, LedgerOptions options);

  [[nodiscard]] Status OpenInternal();
  [[nodiscard]] Result<RecoveryReport> Replay(bool repair);
  [[nodiscard]] Result<LedgerRecord> ReadFrames(bool repair, RecoveryReport* report);
  [[nodiscard]] Status AppendRecord(LedgerRecord record);
  [[nodiscard]] Result<std::string> EncodeFrame(const LedgerRecord& record) const;
  [[nodiscard]] Result<LedgerRecord> DecodeFramePayload(std::string_view payload,
                                                       std::uint64_t* chain_out) const;
  [[nodiscard]] Result<std::uint64_t> CurrentChain() const;
  [[nodiscard]] Status Flush();
  [[nodiscard]] Result<std::optional<CommittedDecision>> FindCommitted(const ClientId& client,
                                                                      const RequestId& request) const;
  void Remember(const LedgerRecord& record);

  std::filesystem::path directory_;
  std::filesystem::path journal_path_;
  std::filesystem::path lock_path_;
  std::filesystem::path snapshot_path_;
  GovernorEngine engine_;
  LedgerOptions options_;

  mutable std::mutex mutex_;
  std::FILE* journal_{nullptr};
  void* lock_handle_{nullptr};      // HANDLE on Windows, int fd encoded on POSIX
  bool lock_held_{false};
  bool closed_{false};
  std::uint64_t chain_{0};
  JournalSequence last_sequence_;
  Epoch current_epoch_;
  UtcInstant last_recorded_at_;
  bool header_present_{false};
  std::string header_bytes_;
  RecoveryReport recovery_;
  std::map<std::pair<std::string, std::uint64_t>, CommittedDecision> committed_;
};

}  // namespace ecg

#endif  // ECG_LEDGER_HPP
