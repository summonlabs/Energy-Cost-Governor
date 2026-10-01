#include "ecg/ledger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ecg/canonical.hpp"
#include "ecg/hash.hpp"
#include "ecg/json.hpp"
#include "ecg/version.hpp"

namespace ecg {
namespace {

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

constexpr std::size_t kHeaderPrefixBytes = 16;  // magic + length + crc

[[nodiscard]] Status ReadWholeFile(const std::filesystem::path& path, std::size_t max_bytes,
                                   std::string* out, bool* exists) {
  *exists = false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return OkStatus();
  }
  *exists = true;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return MakeError(ErrorCode::kIoFailure, path.string(), "cannot size file: " + ec.message());
  }
  if (size > max_bytes) {
    return MakeError(ErrorCode::kResourceLimitExceeded, path.string(),
                     "file is " + std::to_string(size) + " bytes, above the " +
                         std::to_string(max_bytes) + " byte bound");
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return MakeError(ErrorCode::kIoFailure, path.string(), "cannot open file for reading");
  }
  out->resize(static_cast<std::size_t>(size));
  std::size_t read = 0;
  if (size > 0) {
    read = std::fread(out->data(), 1, static_cast<std::size_t>(size), file);
  }
  std::fclose(file);
  if (read != static_cast<std::size_t>(size)) {
    return MakeError(ErrorCode::kIoFailure, path.string(), "short read");
  }
  return OkStatus();
}

/// Writes bytes to a temporary file, flushes them to stable storage, and renames
/// the temporary over the target. A reader therefore sees either the previous
/// complete file or the new complete file, never a partial write.
[[nodiscard]] Status WriteFileAtomically(const std::filesystem::path& target,
                                         const std::filesystem::path& temporary,
                                         std::string_view bytes) {
  {
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
      return MakeError(ErrorCode::kIoFailure, temporary.string(), "cannot open temporary file");
    }
    const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = std::fflush(file) == 0;
    bool synced = true;
#if defined(_WIN32)
    synced = _commit(_fileno(file)) == 0;
#else
    synced = ::fsync(::fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    if (written != bytes.size() || !flushed || !synced || !closed) {
      std::error_code ignore;
      std::filesystem::remove(temporary, ignore);
      return MakeError(ErrorCode::kIoFailure, temporary.string(),
                       "failed to write and flush the temporary file");
    }
  }
#if defined(_WIN32)
  if (MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = GetLastError();
    std::error_code ignore;
    std::filesystem::remove(temporary, ignore);
    return MakeError(ErrorCode::kIoFailure, target.string(),
                     "atomic rename failed with error " + std::to_string(code));
  }
#else
  if (::rename(temporary.string().c_str(), target.string().c_str()) != 0) {
    std::error_code ignore;
    std::filesystem::remove(temporary, ignore);
    return MakeError(ErrorCode::kIoFailure, target.string(), "atomic rename failed");
  }
#endif
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Kernel-enforced single-writer lock
//
// The lock is released by the kernel when the owning process exits, however it
// exits. A crashed writer therefore cannot strand the ledger.
// ---------------------------------------------------------------------------

enum class LockOutcome { kAcquired, kBusy, kFailed };

#if defined(_WIN32)
[[nodiscard]] LockOutcome AcquireKernelLock(const std::filesystem::path& path, void** handle_out) {
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return LockOutcome::kFailed;
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                 &overlapped) == 0) {
    const DWORD code = GetLastError();
    CloseHandle(handle);
    return code == ERROR_LOCK_VIOLATION ? LockOutcome::kBusy : LockOutcome::kFailed;
  }
  *handle_out = handle;
  return LockOutcome::kAcquired;
}

void ReleaseKernelLock(void* handle) {
  if (handle == nullptr) {
    return;
  }
  auto* raw = static_cast<HANDLE>(handle);
  OVERLAPPED overlapped{};
  UnlockFileEx(raw, 0, 1, 0, &overlapped);
  CloseHandle(raw);
}
#else
[[nodiscard]] LockOutcome AcquireKernelLock(const std::filesystem::path& path, void** handle_out) {
  const int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return LockOutcome::kFailed;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return LockOutcome::kBusy;
  }
  *handle_out = reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
  return LockOutcome::kAcquired;
}

void ReleaseKernelLock(void* handle) {
  if (handle == nullptr) {
    return;
  }
  const int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle)) - 1;
  ::flock(fd, LOCK_UN);
  ::close(fd);
}
#endif

// ---------------------------------------------------------------------------
// Journal framing
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue RecordBodyToJson(const LedgerRecord& record) {
  JsonValue::Object members;
  members.emplace_back("kind", JsonValue::String(LedgerRecordKindName(record.kind)));
  members.emplace_back("sequence", JsonValue::String(record.sequence.ToString()));
  members.emplace_back("epoch", JsonValue::String(record.epoch.ToString()));
  members.emplace_back("recorded_at", ToJson(record.recorded_at));
  if (!record.client_id.empty()) {
    members.emplace_back("client_id", JsonValue::String(record.client_id.value()));
  }
  if (!record.request_id.is_unset()) {
    members.emplace_back("request_id", JsonValue::String(record.request_id.ToString()));
  }
  if (record.previous_epoch.has_value()) {
    members.emplace_back("previous_epoch", JsonValue::String(record.previous_epoch->ToString()));
  }
  if (record.state.has_value()) {
    members.emplace_back("state", ToJson(*record.state));
  }
  if (record.decision.has_value()) {
    members.emplace_back("decision", ToJson(*record.decision));
  }
  if (record.acknowledgement.has_value()) {
    const IntentAcknowledgement& ack = *record.acknowledgement;
    JsonValue::Object entry;
    entry.emplace_back("intent_id", JsonValue::String(ack.intent_id.ToString()));
    entry.emplace_back("decision_id", JsonValue::String(ack.decision_id.ToString()));
    entry.emplace_back("acknowledged_by", JsonValue::String(ack.acknowledged_by.value()));
    entry.emplace_back("acknowledged_at", ToJson(ack.acknowledged_at));
    entry.emplace_back("note", JsonValue::String(ack.note));
    members.emplace_back("intent_acknowledgement", JsonValue::ObjectValue(std::move(entry)));
  }
  return JsonValue::ObjectValue(std::move(members));
}

[[nodiscard]] Result<std::uint64_t> ReadUint(const JsonValue& object, std::string_view key,
                                             const std::string& context) {
  const JsonValue* member = object.Find(key);
  if (member == nullptr) {
    return MakeError(ErrorCode::kMissingRequiredField, context,
                     "missing '" + std::string(key) + "'");
  }
  std::string literal;
  if (member->is_string()) {
    literal = member->as_string();
  } else if (member->is_number()) {
    literal = member->as_number_text();
  } else {
    return MakeError(ErrorCode::kMalformedInput, context,
                     "'" + std::string(key) + "' must be an integer");
  }
  if (literal.empty() || literal.size() > 20) {
    return MakeError(ErrorCode::kOutOfRange, context, "'" + std::string(key) + "' is not 64-bit");
  }
  std::uint64_t value = 0;
  for (const char c : literal) {
    if (c < '0' || c > '9') {
      return MakeError(ErrorCode::kMalformedInput, context,
                       "'" + std::string(key) + "' must be a non-negative integer");
    }
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

[[nodiscard]] Result<LedgerRecordKind> ParseRecordKind(std::string_view name) {
  if (name == "epoch_advance") return LedgerRecordKind::kEpochAdvance;
  if (name == "state_snapshot") return LedgerRecordKind::kStateSnapshot;
  if (name == "decision") return LedgerRecordKind::kDecision;
  if (name == "intent_acknowledgement") return LedgerRecordKind::kIntentAcknowledgement;
  return MakeError(ErrorCode::kUnsupportedValue, "journal_record",
                   "unknown record kind '" + std::string(name) + "'");
}

[[nodiscard]] Result<LedgerRecord> RecordBodyFromJson(const JsonValue& body) {
  const std::string context = "journal_record";
  if (!body.is_object()) {
    return MakeError(ErrorCode::kMalformedInput, context, "record body must be an object");
  }
  const JsonValue* kind_member = body.Find("kind");
  if (kind_member == nullptr || !kind_member->is_string()) {
    return MakeError(ErrorCode::kMissingRequiredField, context, "record body has no 'kind'");
  }
  ECG_TRY(kind, ParseRecordKind(kind_member->as_string()));
  ECG_TRY(sequence, ReadUint(body, "sequence", context));
  ECG_TRY(epoch, ReadUint(body, "epoch", context));

  LedgerRecord record;
  record.kind = kind;
  record.sequence = JournalSequence::FromRaw(sequence);
  record.epoch = Epoch::FromRaw(epoch);

  const JsonValue* recorded = body.Find("recorded_at");
  if (recorded == nullptr || !recorded->is_string()) {
    return MakeError(ErrorCode::kMissingRequiredField, context, "record body has no 'recorded_at'");
  }
  ECG_TRY(recorded_at, ParseRfc3339(recorded->as_string()));
  record.recorded_at = recorded_at;

  if (const JsonValue* client = body.Find("client_id"); client != nullptr) {
    if (!client->is_string()) {
      return MakeError(ErrorCode::kMalformedInput, context, "'client_id' must be a string");
    }
    ECG_TRY(parsed, ClientId::Parse(client->as_string()));
    record.client_id = parsed;
  }
  if (const JsonValue* request = body.Find("request_id"); request != nullptr) {
    if (!request->is_string()) {
      return MakeError(ErrorCode::kMalformedInput, context, "'request_id' must be a string");
    }
    ECG_TRY(parsed, RequestId::Parse(request->as_string()));
    record.request_id = parsed;
  }
  if (const JsonValue* previous = body.Find("previous_epoch"); previous != nullptr) {
    if (!previous->is_string()) {
      return MakeError(ErrorCode::kMalformedInput, context, "'previous_epoch' must be a string");
    }
    ECG_TRY(parsed, Epoch::Parse(previous->as_string()));
    record.previous_epoch = parsed;
  }
  if (const JsonValue* state = body.Find("state"); state != nullptr) {
    ECG_TRY(parsed, ParseStateDocument(*state));
    record.state = std::move(parsed);
  }
  if (const JsonValue* decision = body.Find("decision"); decision != nullptr) {
    ECG_TRY(parsed, ParseDecision(*decision));
    record.decision = std::move(parsed);
  }
  if (const JsonValue* ack = body.Find("intent_acknowledgement"); ack != nullptr) {
    if (!ack->is_object()) {
      return MakeError(ErrorCode::kMalformedInput, context, "'intent_acknowledgement' must be an object");
    }
    IntentAcknowledgement parsed;
    const JsonValue* intent_id = ack->Find("intent_id");
    const JsonValue* decision_id = ack->Find("decision_id");
    const JsonValue* by = ack->Find("acknowledged_by");
    const JsonValue* at = ack->Find("acknowledged_at");
    if (intent_id == nullptr || decision_id == nullptr || by == nullptr || at == nullptr) {
      return MakeError(ErrorCode::kMissingRequiredField, context,
                       "intent acknowledgement is missing a required field");
    }
    ECG_TRY(intent_parsed, IntentId::Parse(intent_id->as_string()));
    ECG_TRY(decision_parsed, DecisionId::Parse(decision_id->as_string()));
    ECG_TRY(by_parsed, ClientId::Parse(by->as_string()));
    ECG_TRY(at_parsed, ParseRfc3339(at->as_string()));
    parsed.intent_id = intent_parsed;
    parsed.decision_id = decision_parsed;
    parsed.acknowledged_by = by_parsed;
    parsed.acknowledged_at = at_parsed;
    if (const JsonValue* note = ack->Find("note"); note != nullptr && note->is_string()) {
      parsed.note = note->as_string();
    }
    record.acknowledgement = std::move(parsed);
  }
  return record;
}

}  // namespace

const char* LedgerRecordKindName(LedgerRecordKind kind) noexcept {
  switch (kind) {
    case LedgerRecordKind::kEpochAdvance: return "epoch_advance";
    case LedgerRecordKind::kStateSnapshot: return "state_snapshot";
    case LedgerRecordKind::kDecision: return "decision";
    case LedgerRecordKind::kIntentAcknowledgement: return "intent_acknowledgement";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Construction and opening
// ---------------------------------------------------------------------------

DecisionLedger::DecisionLedger(std::filesystem::path directory, GovernorEngine engine,
                               LedgerOptions options)
    : directory_(std::move(directory)),
      journal_path_(directory_ / kJournalFileName),
      lock_path_(directory_ / kLockFileName),
      snapshot_path_(directory_ / kSnapshotFileName),
      engine_(std::move(engine)),
      options_(options) {}

DecisionLedger::~DecisionLedger() {
  const Status status = Close();
  (void)status;
}

Result<std::unique_ptr<DecisionLedger>> DecisionLedger::Open(const std::filesystem::path& directory,
                                                             PolicySet policy,
                                                             LedgerOptions options) {
  ECG_TRY(engine, GovernorEngine::Make(std::move(policy)));

  std::error_code ec;
  if (options.create_if_missing) {
    std::filesystem::create_directories(directory, ec);
    if (ec) {
      return MakeError(ErrorCode::kIoFailure, directory.string(),
                       "cannot create the ledger directory: " + ec.message());
    }
  } else if (!std::filesystem::is_directory(directory, ec)) {
    return MakeError(ErrorCode::kNotFound, directory.string(), "ledger directory does not exist");
  }

  auto ledger =
      std::unique_ptr<DecisionLedger>(new DecisionLedger(directory, std::move(engine), options));
  ECG_TRY_STATUS(ledger->OpenInternal());
  return ledger;
}

Status DecisionLedger::OpenInternal() {
  void* handle = nullptr;
  const LockOutcome outcome = AcquireKernelLock(lock_path_, &handle);
  if (outcome == LockOutcome::kBusy) {
    return MakeError(ErrorCode::kLockUnavailable, lock_path_.string(),
                     "another process holds the single-writer lock for this ledger");
  }
  if (outcome != LockOutcome::kAcquired) {
    return MakeError(ErrorCode::kIoFailure, lock_path_.string(), "cannot acquire the writer lock");
  }
  lock_handle_ = handle;
  lock_held_ = true;

  ECG_TRY(report, Replay(true));
  recovery_ = report;
  current_epoch_ = report.current_epoch;
  last_sequence_ = report.last_sequence;
  last_recorded_at_ = report.last_recorded_at;

  journal_ = std::fopen(journal_path_.string().c_str(), "ab");
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "cannot open the journal for append");
  }

  // The published snapshot is a derived artefact. A snapshot that claims a
  // position the journal has not reached was published out of order and is
  // reported, not trusted.
  bool exists = false;
  std::string snapshot_bytes;
  const Status snapshot_status =
      ReadWholeFile(snapshot_path_, 64u << 20, &snapshot_bytes, &exists);
  if (snapshot_status.ok() && exists) {
    const auto parsed = JsonValue::Parse(snapshot_bytes);
    if (parsed.ok()) {
      const JsonValue* sequence_member = parsed.value().Find("journal_sequence");
      if (sequence_member != nullptr) {
        const auto declared = ReadUint(parsed.value(), "journal_sequence", "state_snapshot");
        if (declared.ok()) {
          recovery_.snapshot_present = true;
          recovery_.snapshot_sequence = JournalSequence::FromRaw(declared.value());
          if (declared.value() > last_sequence_.raw()) {
            recovery_.snapshot_present = false;
            recovery_.snapshot_sequence = JournalSequence::FromRaw(0);
            std::error_code remove_error;
            std::filesystem::remove(snapshot_path_, remove_error);
          }
        }
      }
    }
  }
  return OkStatus();
}

Status DecisionLedger::Close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return OkStatus();
  }
  closed_ = true;
  Status result = OkStatus();
  if (journal_ != nullptr) {
    if (std::fflush(journal_) != 0) {
      result = MakeError(ErrorCode::kIoFailure, journal_path_.string(), "failed to flush the journal");
    }
    std::fclose(journal_);
    journal_ = nullptr;
  }
  if (lock_held_) {
    ReleaseKernelLock(lock_handle_);
    lock_handle_ = nullptr;
    lock_held_ = false;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Replay and recovery
// ---------------------------------------------------------------------------

Result<RecoveryReport> DecisionLedger::Replay(bool repair) {
  RecoveryReport report;
  bool exists = false;
  std::string bytes;
  const std::size_t bound = options_.max_journal_bytes + kMaxJournalFrameBytes + kHeaderPrefixBytes;
  ECG_TRY_STATUS(ReadWholeFile(journal_path_, bound, &bytes, &exists));

  if (!exists || bytes.size() < kHeaderPrefixBytes) {
    if (!exists || bytes.empty()) {
      // A brand-new journal: write the header, which is the chain anchor.
      Encoder header;
      header.U32(kJournalFormatVersion);
      header.U32(kEncodingVersion);
      header.U64(1);
      const auto created = ParseRfc3339("2026-01-01T00:00:00.000000Z");
      header.I64(created.ok() ? created.value().raw() : 0);
      (void)header.String(std::string(BuildIdentity()));
      header_bytes_ = header.bytes();

      std::string file;
      file.append(kJournalMagic, sizeof(kJournalMagic));
      Encoder prefix;
      prefix.U32(static_cast<std::uint32_t>(header_bytes_.size()));
      prefix.U32(Crc32c(header_bytes_));
      file.append(prefix.bytes());
      file.append(header_bytes_);

      std::FILE* out = std::fopen(journal_path_.string().c_str(), "wb");
      if (out == nullptr) {
        return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "cannot create the journal");
      }
      const std::size_t written = std::fwrite(file.data(), 1, file.size(), out);
      const bool flushed = std::fflush(out) == 0;
#if defined(_WIN32)
      const bool synced = _commit(_fileno(out)) == 0;
#else
      const bool synced = ::fsync(::fileno(out)) == 0;
#endif
      std::fclose(out);
      if (written != file.size() || !flushed || !synced) {
        return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "cannot initialise the journal");
      }
      header_present_ = true;
      chain_ = Fnv1a64(header_bytes_);
      last_sequence_ = JournalSequence::FromRaw(0);
      current_epoch_ = Epoch::FromRaw(1);
      report.current_epoch = current_epoch_;
      report.last_sequence = last_sequence_;
      return report;
    }
    return MakeError(ErrorCode::kIntegrityFailure, journal_path_.string(),
                     "journal is shorter than its header; there is no chain anchor to recover from");
  }

  if (std::memcmp(bytes.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
    return MakeError(ErrorCode::kIntegrityFailure, journal_path_.string(),
                     "journal magic does not match; refusing to interpret the file");
  }
  const auto read_u32 = [&bytes](std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
  };
  const std::uint32_t header_length = read_u32(8);
  const std::uint32_t header_crc = read_u32(12);
  if (header_length == 0 || kHeaderPrefixBytes + header_length > bytes.size()) {
    return MakeError(ErrorCode::kIntegrityFailure, journal_path_.string(),
                     "journal header length is not consistent with the file size");
  }
  header_bytes_ = bytes.substr(kHeaderPrefixBytes, header_length);
  if (Crc32c(header_bytes_) != header_crc) {
    return MakeError(ErrorCode::kIntegrityFailure, journal_path_.string(),
                     "journal header fails its integrity check");
  }
  {
    Decoder header_decoder(header_bytes_);
    ECG_TRY(format_version, header_decoder.U32());
    if (format_version != kJournalFormatVersion) {
      return MakeError(ErrorCode::kUnsupportedVersion, journal_path_.string(),
                       "journal format version " + std::to_string(format_version) +
                           " is not supported by this build");
    }
  }
  header_present_ = true;
  chain_ = Fnv1a64(header_bytes_);

  std::size_t offset = kHeaderPrefixBytes + header_length;
  JournalSequence expected_sequence = JournalSequence::FromRaw(1);
  std::uint64_t expected_chain = chain_;
  std::vector<LedgerRecord> records;
  std::size_t damaged_offset = 0;
  bool damaged = false;
  bool truncation = false;

  while (offset < bytes.size()) {
    if (offset + 8 > bytes.size()) {
      // Fewer bytes than a frame header: the write was cut short.
      damaged = true;
      truncation = true;
      damaged_offset = offset;
      break;
    }
    const std::uint32_t payload_length = read_u32(offset);
    const std::uint32_t payload_crc = read_u32(offset + 4);
    if (offset + 8 + payload_length > bytes.size()) {
      // The frame claims more bytes than the file holds: the write was cut short.
      damaged = true;
      truncation = true;
      damaged_offset = offset;
      break;
    }
    if (payload_length == 0 || payload_length > kMaxJournalFrameBytes) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    const std::string_view payload(bytes.data() + offset + 8, payload_length);
    if (Crc32c(payload) != payload_crc) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    Decoder decoder(payload);
    const auto sequence = decoder.U64();
    const auto previous_chain = decoder.U64();
    const auto kind = decoder.U8();
    const auto epoch = decoder.U64();
    const auto recorded_at = decoder.I64();
    const auto body = decoder.String();
    if (!sequence.ok() || !previous_chain.ok() || !kind.ok() || !epoch.ok() || !recorded_at.ok() ||
        !body.ok() || !decoder.RequireEnd().ok()) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    if (sequence.value() != expected_sequence.raw() || previous_chain.value() != expected_chain) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    const auto parsed_json = JsonValue::Parse(body.value());
    if (!parsed_json.ok()) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    const auto record = RecordBodyFromJson(parsed_json.value());
    if (!record.ok()) {
      damaged = true;
      damaged_offset = offset;
      break;
    }
    if (record.value().sequence.raw() != sequence.value() ||
        record.value().epoch.raw() != epoch.value() ||
        record.value().recorded_at.raw() != recorded_at.value() ||
        static_cast<std::uint8_t>(record.value().kind) != kind.value()) {
      // The binary envelope and the JSON body must agree; a disagreement means
      // the frame was rewritten by something that did not understand both.
      damaged = true;
      damaged_offset = offset;
      break;
    }

    records.push_back(record.value());
    expected_chain = Fnv1a64(payload, expected_chain);
    expected_sequence = JournalSequence::FromRaw(expected_sequence.raw() + 1);
    offset += 8 + payload_length;
    report.frames_read += 1;
  }

  if (damaged) {
    const std::size_t tail_size = bytes.size() - damaged_offset;
    report.tail_damaged = true;
    report.torn_tail_bytes_discarded = tail_size;

    // Only genuinely absent bytes are a torn tail. A zero-filled remainder is
    // also treated as a torn tail, because several file systems leave zeros in
    // the final block after a crash. Anything else -- a complete frame that
    // fails its checksum, a chain link that does not match, an impossible frame
    // length -- is damage to committed history, and this runtime refuses to open
    // rather than silently dropping records.
    const bool zero_filled =
        std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(damaged_offset), bytes.end(),
                    [](char byte) { return byte == '\0'; });
    if (!truncation && !zero_filled) {
      return MakeError(ErrorCode::kInteriorCorruption, journal_path_.string(),
                       "a complete frame at byte " + std::to_string(damaged_offset) +
                           " fails its integrity check; refusing to drop committed records");
    }
    if (tail_size > options_.max_torn_tail_scan) {
      return MakeError(ErrorCode::kInteriorCorruption, journal_path_.string(),
                       "a damaged region of " + std::to_string(tail_size) +
                           " bytes is too large to recover as a torn tail");
    }
    if (repair) {
      std::error_code ec;
      std::filesystem::resize_file(journal_path_, damaged_offset, ec);
      if (ec) {
        return MakeError(ErrorCode::kIoFailure, journal_path_.string(),
                         "cannot truncate the torn tail: " + ec.message());
      }
      report.truncated_torn_tail = true;
    }
  }

  Epoch running_epoch = Epoch::FromRaw(1);
  JournalSequence running_sequence = JournalSequence::FromRaw(0);
  UtcInstant running_time = UtcInstant::FromRaw(0);
  for (const LedgerRecord& record : records) {
    if (record.kind == LedgerRecordKind::kEpochAdvance) {
      if (!record.previous_epoch.has_value() || !(*record.previous_epoch == running_epoch)) {
        return MakeError(ErrorCode::kStaleReplayRejected, journal_path_.string(),
                         "epoch advance at sequence " + record.sequence.ToString() +
                             " does not continue from the epoch in force");
      }
      if (!(running_epoch < record.epoch)) {
        return MakeError(ErrorCode::kStaleReplayRejected, journal_path_.string(),
                         "epoch advance at sequence " + record.sequence.ToString() +
                             " moves the authority epoch backwards");
      }
      running_epoch = record.epoch;
      report.epoch_advances += 1;
    } else if (record.epoch < running_epoch) {
      return MakeError(ErrorCode::kStaleReplayRejected, journal_path_.string(),
                       "record " + record.sequence.ToString() + " carries epoch " +
                           record.epoch.ToString() + ", older than the epoch in force " +
                           running_epoch.ToString());
    }
    if (record.kind == LedgerRecordKind::kDecision) {
      report.decisions += 1;
      if (record.decision.has_value()) {
        CommittedDecision committed;
        committed.decision = *record.decision;
        committed.sequence = record.sequence;
        committed_.emplace(std::make_pair(record.client_id.value(), record.request_id.raw()),
                           std::move(committed));
      }
    } else if (record.kind == LedgerRecordKind::kIntentAcknowledgement) {
      report.acknowledgements += 1;
    } else if (record.kind == LedgerRecordKind::kStateSnapshot) {
      report.snapshots += 1;
    }
    running_sequence = record.sequence;
    running_time = record.recorded_at;
  }

  // Recompute the chain from the frames themselves, so the in-memory anchor
  // matches the file rather than the loop above.
  {
    std::uint64_t recomputed = Fnv1a64(header_bytes_);
    std::size_t cursor = kHeaderPrefixBytes + header_length;
    for (std::size_t i = 0; i < report.frames_read; ++i) {
      const std::uint32_t length = read_u32(cursor);
      recomputed = Fnv1a64(std::string_view(bytes.data() + cursor + 8, length), recomputed);
      cursor += 8 + length;
    }
    chain_ = recomputed;
  }

  last_sequence_ = running_sequence;
  last_recorded_at_ = running_time;
  current_epoch_ = running_epoch;
  report.last_sequence = running_sequence;
  report.last_recorded_at = running_time;
  report.current_epoch = running_epoch;
  return report;
}

Result<std::uint64_t> DecisionLedger::CurrentChain() const { return chain_; }

// ---------------------------------------------------------------------------
// Appending
// ---------------------------------------------------------------------------

Result<std::string> DecisionLedger::EncodeFrame(const LedgerRecord& record) const {
  const JsonValue body = RecordBodyToJson(record);
  const std::string body_text = body.Dump();

  Encoder payload;
  payload.U64(record.sequence.raw());
  payload.U64(chain_);
  payload.U8(static_cast<std::uint8_t>(record.kind));
  payload.U64(record.epoch.raw());
  payload.I64(record.recorded_at.raw());
  ECG_TRY_STATUS(payload.String(body_text));

  std::string frame;
  frame.reserve(payload.size() + 8);
  Encoder prefix;
  prefix.U32(static_cast<std::uint32_t>(payload.size()));
  prefix.U32(Crc32c(payload.bytes()));
  frame.append(prefix.bytes());
  frame.append(payload.bytes());
  return frame;
}

Status DecisionLedger::AppendRecord(LedgerRecord record) {
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "journal is not open for append");
  }
  record.sequence = JournalSequence::FromRaw(last_sequence_.raw() + 1);
  if (record.recorded_at.is_zero()) {
    record.recorded_at = last_recorded_at_;
  }

  ECG_TRY(frame, EncodeFrame(record));

  {
    std::error_code ec;
    const auto size = std::filesystem::file_size(journal_path_, ec);
    if (!ec && size + frame.size() > options_.max_journal_bytes) {
      return MakeError(ErrorCode::kResourceLimitExceeded, journal_path_.string(),
                       "the journal would exceed its " + std::to_string(options_.max_journal_bytes) +
                           " byte bound");
    }
  }

  const std::size_t written = std::fwrite(frame.data(), 1, frame.size(), journal_);
  if (written != frame.size()) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "short write to the journal");
  }
  ECG_TRY_STATUS(Flush());

  // The record becomes visible only after the flush above: that is the commit
  // point. Everything below is in-memory bookkeeping derived from it.
  chain_ = Fnv1a64(std::string_view(frame.data() + 8, frame.size() - 8), chain_);
  last_sequence_ = record.sequence;
  last_recorded_at_ = record.recorded_at;
  Remember(record);
  return OkStatus();
}

Status DecisionLedger::Flush() {
  if (std::fflush(journal_) != 0) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "flush failed");
  }
#if defined(_WIN32)
  if (_commit(_fileno(journal_)) != 0) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(),
                     "cannot flush the journal to stable storage");
  }
#else
  if (::fsync(::fileno(journal_)) != 0) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(),
                     "cannot flush the journal to stable storage");
  }
#endif
  return OkStatus();
}

void DecisionLedger::Remember(const LedgerRecord& record) {
  if (record.kind != LedgerRecordKind::kDecision || !record.decision.has_value()) {
    return;
  }
  CommittedDecision committed;
  committed.decision = *record.decision;
  committed.sequence = record.sequence;
  committed_.emplace(std::make_pair(record.client_id.value(), record.request_id.raw()),
                     std::move(committed));
}

Result<std::optional<CommittedDecision>> DecisionLedger::FindCommitted(const ClientId& client,
                                                                      const RequestId& request) const {
  const auto it = committed_.find(std::make_pair(client.value(), request.raw()));
  if (it == committed_.end()) {
    return std::optional<CommittedDecision>{};
  }
  return std::optional<CommittedDecision>{it->second};
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

Result<CommittedDecision> DecisionLedger::Submit(const DecisionRequest& request,
                                                 const EvidenceSet& evidence, UtcInstant now) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!(request.epoch == current_epoch_)) {
      return MakeError(ErrorCode::kStaleReplayRejected, request.request_id.ToString(),
                       "request epoch " + request.epoch.ToString() +
                           " is not the epoch in force (" + current_epoch_.ToString() +
                           "); stale submissions are refused");
    }
    if (const auto existing = FindCommitted(request.client_id, request.request_id);
        existing.ok() && existing.value().has_value()) {
      CommittedDecision result = *existing.value();
      result.duplicate = true;
      return result;
    }
  }

  // Evaluation is a pure function and runs outside the writer lock, so a slow
  // decision never blocks other submissions or the journal.
  ECG_TRY(decision, engine_.Evaluate(request, evidence, now));
  return Commit(decision, now);
}

Result<CommittedDecision> DecisionLedger::Commit(const Decision& decision, UtcInstant now) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "the ledger is closed");
  }
  if (!(decision.epoch == current_epoch_)) {
    return MakeError(ErrorCode::kStaleReplayRejected, decision.request_id.ToString(),
                     "decision epoch " + decision.epoch.ToString() +
                         " is not the epoch in force (" + current_epoch_.ToString() + ")");
  }
  if (const auto existing = FindCommitted(decision.client_id, decision.request_id);
      existing.ok() && existing.value().has_value()) {
    CommittedDecision result = *existing.value();
    result.duplicate = true;
    return result;
  }

  LedgerRecord record;
  record.kind = LedgerRecordKind::kDecision;
  record.epoch = decision.epoch;
  record.recorded_at = now;
  record.client_id = decision.client_id;
  record.request_id = decision.request_id;
  record.decision = decision;
  ECG_TRY_STATUS(AppendRecord(record));

  CommittedDecision committed;
  committed.decision = decision;
  committed.sequence = last_sequence_;
  committed.duplicate = false;
  return committed;
}

Status DecisionLedger::AdvanceEpoch(Epoch next, UtcInstant now) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "the ledger is closed");
  }
  if (!(current_epoch_ < next)) {
    return MakeError(ErrorCode::kStaleReplayRejected, journal_path_.string(),
                     "epoch must advance strictly: current " + current_epoch_.ToString() +
                         ", requested " + next.ToString());
  }
  LedgerRecord record;
  record.kind = LedgerRecordKind::kEpochAdvance;
  record.epoch = next;
  record.recorded_at = now;
  record.previous_epoch = current_epoch_;
  ECG_TRY_STATUS(AppendRecord(record));
  current_epoch_ = next;
  return OkStatus();
}

Status DecisionLedger::PublishState(const StateDocument& state, UtcInstant now) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "the ledger is closed");
  }
  LedgerRecord record;
  record.kind = LedgerRecordKind::kStateSnapshot;
  record.epoch = current_epoch_;
  record.recorded_at = now;
  record.state = state;
  record.state->epoch = current_epoch_;
  ECG_TRY_STATUS(AppendRecord(record));

  JsonValue::Object envelope;
  envelope.emplace_back("journal_sequence", JsonValue::String(last_sequence_.ToString()));
  envelope.emplace_back("written_at", ToJson(now));
  envelope.emplace_back("state", ToJson(record.state.value()));
  const std::string text = JsonValue::ObjectValue(std::move(envelope)).Dump();
  return WriteFileAtomically(snapshot_path_, directory_ / kSnapshotTempName, text);
}

Status DecisionLedger::RecordAcknowledgement(const IntentAcknowledgement& acknowledgement) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_ == nullptr) {
    return MakeError(ErrorCode::kIoFailure, journal_path_.string(), "the ledger is closed");
  }
  LedgerRecord record;
  record.kind = LedgerRecordKind::kIntentAcknowledgement;
  record.epoch = current_epoch_;
  record.recorded_at = acknowledgement.acknowledged_at;
  record.acknowledgement = acknowledgement;
  return AppendRecord(record);
}

Result<std::optional<StateDocument>> DecisionLedger::LoadState() const {
  // The snapshot is published by an atomic rename, so a reader always sees a
  // complete file. The journal position it claims is compared against the
  // in-memory position, which must be read under the writer lock.
  JournalSequence journal_position;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    journal_position = last_sequence_;
  }
  bool exists = false;
  std::string bytes;
  ECG_TRY_STATUS(ReadWholeFile(snapshot_path_, 64u << 20, &bytes, &exists));
  if (!exists) {
    return std::optional<StateDocument>{};
  }
  ECG_TRY(document, JsonValue::Parse(bytes));
  const JsonValue* state_member = document.Find("state");
  if (state_member == nullptr) {
    return MakeError(ErrorCode::kIntegrityFailure, snapshot_path_.string(),
                     "snapshot has no 'state' member");
  }
  ECG_TRY(state, ParseStateDocument(*state_member));
  const JsonValue* sequence_member = document.Find("journal_sequence");
  if (sequence_member != nullptr) {
    ECG_TRY(declared, ReadUint(document, "journal_sequence", "state_snapshot"));
    if (declared > journal_position.raw()) {
      return MakeError(ErrorCode::kIntegrityFailure, snapshot_path_.string(),
                       "snapshot claims journal position " + std::to_string(declared) +
                           " but the journal ends at " + journal_position.ToString());
    }
  }
  // Recovered dynamic evidence is never promoted to current truth.
  MarkEvidenceRecovered(&state.evidence);
  return std::optional<StateDocument>{std::move(state)};
}

RecoveryReport DecisionLedger::recovery() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return recovery_;
}

Epoch DecisionLedger::current_epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return current_epoch_;
}

JournalSequence DecisionLedger::last_sequence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_sequence_;
}

std::vector<CommittedDecision> DecisionLedger::RecoveredDecisions() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<CommittedDecision> decisions;
  decisions.reserve(committed_.size());
  for (const auto& entry : committed_) {
    decisions.push_back(entry.second);
  }
  std::sort(decisions.begin(), decisions.end(),
            [](const CommittedDecision& a, const CommittedDecision& b) {
              return a.sequence < b.sequence;
            });
  return decisions;
}

Result<RecoveryReport> DecisionLedger::Verify() {
  bool exists = false;
  std::string bytes;
  const std::size_t bound = options_.max_journal_bytes + kMaxJournalFrameBytes + kHeaderPrefixBytes;
  ECG_TRY_STATUS(ReadWholeFile(journal_path_, bound, &bytes, &exists));
  if (!exists) {
    return MakeError(ErrorCode::kNotFound, journal_path_.string(), "journal does not exist");
  }
  // Re-run the frame walk without repairing: verification must not modify the
  // file it is checking, so all replay state is saved and restored around it.
  std::lock_guard<std::mutex> guard(mutex_);
  const std::uint64_t saved_chain = chain_;
  const JournalSequence saved_sequence = last_sequence_;
  const Epoch saved_epoch = current_epoch_;
  const UtcInstant saved_time = last_recorded_at_;
  const auto committed_saved = committed_;
  const std::string saved_header = header_bytes_;
  const bool saved_header_present = header_present_;
  const RecoveryReport saved_recovery = recovery_;

  auto replayed = Replay(false);

  chain_ = saved_chain;
  last_sequence_ = saved_sequence;
  current_epoch_ = saved_epoch;
  last_recorded_at_ = saved_time;
  committed_ = committed_saved;
  header_bytes_ = saved_header;
  header_present_ = saved_header_present;
  recovery_ = saved_recovery;

  if (!replayed.ok()) {
    return replayed.error();
  }
  return replayed.value();
}

}  // namespace ecg
