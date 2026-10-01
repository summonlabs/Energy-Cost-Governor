// Energy-Cost-Governor command line implementation.
//
// Every command answers with data an operator or a script can act on: a
// deterministic exit code plus a reason trace. Nothing here invents an effect:
// "decide" records an authorisation, and "intent acknowledge" records that
// another authority acknowledged an intent, never that load actually moved.

#include "commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/json.hpp"
#include "ecg/json_io.hpp"
#include "ecg/ledger.hpp"
#include "ecg/policy.hpp"
#include "ecg/version.hpp"

namespace ecg::cli {
namespace {

constexpr const char* kProgram = "ecg";

/// True when the invocation asked for JSON output. Set once per process from the
/// raw argument list so that even argument-parsing failures are reported in the
/// format the caller asked for.
bool g_json_output = false;

[[nodiscard]] int Fail(const Error& error, bool json = false);

/// Propagates a failure as a CLI exit code, reporting it in the requested format.
#define ECG_TRY_LOCAL(temp, expr)                        \
  auto temp##_result = (expr);                           \
  if (!temp##_result.ok()) {                             \
    return Fail(temp##_result.error(), g_json_output);   \
  }                                                      \
  auto& temp = temp##_result.value()

struct Options {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;

  [[nodiscard]] bool Has(const std::string& name) const { return values.count(name) != 0; }
  [[nodiscard]] bool Flag(const std::string& name) const { return flags.count(name) != 0; }
  [[nodiscard]] std::string Get(const std::string& name) const {
    const auto it = values.find(name);
    return it == values.end() ? std::string() : it->second;
  }
};

using KnownOptions = std::map<std::string, bool>;

[[nodiscard]] Result<Options> ParseOptions(const std::vector<std::string>& arguments,
                                           const KnownOptions& known) {
  Options options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];
    if (argument == "--") {
      for (++index; index < arguments.size(); ++index) {
        options.positional.push_back(arguments[index]);
      }
      break;
    }
    if (argument.size() > 2 && argument[0] == '-' && argument[1] == '-') {
      std::string name = argument;
      std::string value;
      bool inline_value = false;
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        name = argument.substr(0, equals);
        value = argument.substr(equals + 1);
        inline_value = true;
      }
      const auto it = known.find(name);
      if (it == known.end()) {
        return MakeError(ErrorCode::kUnsupportedValue, kProgram, "unknown option '" + name + "'");
      }
      if (it->second) {
        if (!inline_value) {
          if (index + 1 >= arguments.size()) {
            return MakeError(ErrorCode::kMissingRequiredField, kProgram,
                             "option '" + name + "' requires a value");
          }
          value = arguments[++index];
        }
        options.values[name] = value;
      } else {
        if (inline_value) {
          return MakeError(ErrorCode::kInvalidArgument, kProgram,
                           "option '" + name + "' does not take a value");
        }
        options.flags.insert(name);
      }
      continue;
    }
    options.positional.push_back(argument);
  }
  return options;
}

[[nodiscard]] Result<std::string> ReadInput(const std::string& path) {
  std::string text;
  if (path == "-") {
    std::string line;
    while (std::getline(std::cin, line)) {
      text.append(line);
      text.push_back('\n');
    }
    return text;
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return MakeError(ErrorCode::kNotFound, path, "cannot open input file");
  }
  char buffer[8192];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      text.append(buffer, read);
    }
    if (read < sizeof(buffer)) {
      break;
    }
  }
  std::fclose(file);
  return text;
}

[[nodiscard]] Result<JsonValue> ParseJsonText(const std::string& text, const std::string& subject) {
  const auto parsed = JsonValue::Parse(text);
  if (!parsed.ok()) {
    return MakeError(parsed.error().code(), subject, parsed.error().message());
  }
  return parsed.value();
}

/// Current UTC time. Only used when a caller does not supply --now; every test
/// supplies it so results are reproducible.
[[nodiscard]] UtcInstant SystemNow() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
  return UtcInstant::FromRaw(static_cast<std::int64_t>(micros));
}

[[nodiscard]] Result<UtcInstant> ResolveNow(const Options& options, const JsonValue* document) {
  if (options.Has("--now")) {
    const auto parsed = ParseRfc3339(options.Get("--now"));
    if (!parsed.ok()) {
      return MakeError(parsed.error().code(), "--now", parsed.error().message());
    }
    return parsed.value();
  }
  if (document != nullptr) {
    if (const JsonValue* member = document->Find("now"); member != nullptr && member->is_string()) {
      const auto parsed = ParseRfc3339(member->as_string());
      if (!parsed.ok()) {
        return MakeError(parsed.error().code(), "now", parsed.error().message());
      }
      return parsed.value();
    }
  }
  return SystemNow();
}

void PrintError(const Error& error, bool json) {
  if (json) {
    JsonValue::Object members;
    members.emplace_back("error", JsonValue::Bool(true));
    members.emplace_back("code", JsonValue::String(std::string(ErrorCodeName(error.code()))));
    members.emplace_back("category", JsonValue::String(std::string(ErrorCodeCategory(error.code()))));
    members.emplace_back("subject", JsonValue::String(error.subject()));
    members.emplace_back("message", JsonValue::String(error.message()));
    std::fprintf(stderr, "%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
    return;
  }
  std::fprintf(stderr, "%s: %s: %s\n", kProgram, std::string(ErrorCodeName(error.code())).c_str(),
               error.ToString().c_str());
}

[[nodiscard]] int Fail(const Error& error, bool json) {
  PrintError(error, json);
  return ExitCodeForError(error.code());
}

[[nodiscard]] Result<JsonValue> LoadDocument(const Options& options, const std::string& default_path) {
  const std::string path = options.Has("--input") ? options.Get("--input") : default_path;
  if (path.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, kProgram, "--input is required");
  }
  ECG_TRY(text, ReadInput(path));
  return ParseJsonText(text, path);
}

/// Resolves the policy: an explicit file wins, then a policy member of the
/// document, then the documented default.
[[nodiscard]] Result<PolicySet> ResolvePolicy(const Options& options, const JsonValue* document) {
  if (options.Has("--policy")) {
    ECG_TRY(text, ReadInput(options.Get("--policy")));
    ECG_TRY(value, ParseJsonText(text, options.Get("--policy")));
    ECG_TRY(policy, ParsePolicySet(value));
    return policy;
  }
  if (document != nullptr) {
    if (const JsonValue* member = document->Find("policy"); member != nullptr) {
      ECG_TRY(policy, ParsePolicySet(*member));
      return policy;
    }
  }
  return DefaultPolicy();
}

[[nodiscard]] Result<std::filesystem::path> ResolveStateDir(const Options& options) {
  if (!options.Has("--state")) {
    return MakeError(ErrorCode::kMissingRequiredField, kProgram, "--state <directory> is required");
  }
  return std::filesystem::path(options.Get("--state"));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CommandVersion(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options, ParseOptions(arguments, {{"--json", false}}));
  if (options.Flag("--json")) {
    JsonValue::Object members;
    members.emplace_back("name", JsonValue::String("energy-cost-governor"));
    members.emplace_back("version", JsonValue::String(VersionString()));
    members.emplace_back("build", JsonValue::String(std::string(BuildIdentity())));
    members.emplace_back("journal_format_version",
                         JsonValue::String(std::to_string(kJournalFormatVersion)));
    members.emplace_back("encoding_version", JsonValue::String(std::to_string(kEncodingVersion)));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
    return static_cast<int>(ExitCode::kOk);
  }
  std::printf("energy-cost-governor %s\n%s\n", VersionString(), std::string(BuildIdentity()).c_str());
  std::printf("journal format %u, encoding %u\n", kJournalFormatVersion, kEncodingVersion);
  return static_cast<int>(ExitCode::kOk);
}

int CommandDecide(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options,
                ParseOptions(arguments, {{"--state", true},
                                         {"--input", true},
                                         {"--now", true},
                                         {"--policy", true},
                                         {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  ECG_TRY_LOCAL(document, LoadDocument(options, "-"));
  if (!document.is_object()) {
    return Fail(MakeError(ErrorCode::kMalformedInput, "decide", "input must be a JSON object"), json);
  }
  const JsonValue* request_member = document.Find("request");
  if (request_member == nullptr) {
    return Fail(MakeError(ErrorCode::kMissingRequiredField, "decide", "input has no 'request' member"), json);
  }
  const JsonValue* evidence_member = document.Find("evidence");
  if (evidence_member == nullptr) {
    return Fail(MakeError(ErrorCode::kMissingRequiredField, "decide", "input has no 'evidence' member"), json);
  }
  ECG_TRY_LOCAL(request, ParseDecisionRequest(*request_member));
  ECG_TRY_LOCAL(evidence, ParseEvidenceSet(*evidence_member));
  ECG_TRY_LOCAL(now, ResolveNow(options, &document));
  ECG_TRY_LOCAL(policy, ResolvePolicy(options, &document));

  const auto opened = DecisionLedger::Open(state_dir, policy);
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  const std::unique_ptr<DecisionLedger>& ledger = opened.value();

  const auto committed = ledger->Submit(request, evidence, now);
  if (!committed.ok()) {
    return Fail(committed.error(), json);
  }

  if (json) {
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("decide"));
    members.emplace_back("duplicate", JsonValue::Bool(committed.value().duplicate));
    members.emplace_back("journal_sequence", JsonValue::String(committed.value().sequence.ToString()));
    members.emplace_back("decision", ToJson(committed.value().decision));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
  } else {
    std::printf("%s", FormatDecisionReport(committed.value().decision).c_str());
    std::printf("journal_sequence: %s\n", committed.value().sequence.ToString().c_str());
    if (committed.value().duplicate) {
      std::printf("note: this (client, request) identity was already committed; the stored decision was returned\n");
    }
  }
  return ExitCodeForOutcome(committed.value().decision.outcome);
}

int CommandStateImport(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options,
                ParseOptions(arguments, {{"--state", true},
                                         {"--input", true},
                                         {"--now", true},
                                         {"--policy", true},
                                         {"--epoch", true},
                                         {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  ECG_TRY_LOCAL(document, LoadDocument(options, "-"));
  const JsonValue* evidence_member = document.Find("evidence");
  if (evidence_member == nullptr) {
    return Fail(MakeError(ErrorCode::kMissingRequiredField, "state import", "input has no 'evidence' member"), json);
  }
  ECG_TRY_LOCAL(evidence, ParseEvidenceSet(*evidence_member));
  ECG_TRY_LOCAL(now, ResolveNow(options, &document));
  ECG_TRY_LOCAL(policy, ResolvePolicy(options, &document));

  const auto opened = DecisionLedger::Open(state_dir, policy);
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  const std::unique_ptr<DecisionLedger>& ledger = opened.value();

  if (options.Has("--epoch")) {
    const auto requested = Epoch::Parse(options.Get("--epoch"));
    if (!requested.ok()) {
      return Fail(requested.error(), json);
    }
    if (ledger->current_epoch() < requested.value()) {
      const Status advanced = ledger->AdvanceEpoch(requested.value(), now);
      if (!advanced.ok()) {
        return Fail(advanced.error(), json);
      }
    } else if (!(ledger->current_epoch() == requested.value())) {
      return Fail(MakeError(ErrorCode::kStaleReplayRejected, "state import",
                            "requested epoch " + requested.value().ToString() +
                                " is older than the epoch in force " +
                                ledger->current_epoch().ToString()),
                  json);
    }
  }

  StateDocument state;
  state.epoch = ledger->current_epoch();
  state.policy = ledger->policy();
  state.evidence = evidence;
  state.updated_at = now;
  const Status published = ledger->PublishState(state, now);
  if (!published.ok()) {
    return Fail(published.error(), json);
  }

  if (json) {
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("state import"));
    members.emplace_back("epoch", JsonValue::String(ledger->current_epoch().ToString()));
    members.emplace_back("journal_sequence", JsonValue::String(ledger->last_sequence().ToString()));
    members.emplace_back("state_file", JsonValue::String((state_dir / kSnapshotFileName).string()));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
  } else {
    std::printf("published state at epoch %s, journal sequence %s\n",
                ledger->current_epoch().ToString().c_str(),
                ledger->last_sequence().ToString().c_str());
  }
  return static_cast<int>(ExitCode::kOk);
}

int CommandStateShow(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options, ParseOptions(arguments, {{"--state", true}, {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  const auto opened = DecisionLedger::Open(state_dir, DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  const auto state = opened.value()->LoadState();
  if (!state.ok()) {
    return Fail(state.error(), json);
  }
  if (!state.value().has_value()) {
    if (json) {
      std::printf("{\"command\":\"state show\",\"present\":false}\n");
    } else {
      std::printf("no published state\n");
    }
    return static_cast<int>(ExitCode::kOk);
  }
  const StateDocument& document = *state.value();
  if (json) {
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("state show"));
    members.emplace_back("present", JsonValue::Bool(true));
    members.emplace_back("state", ToJson(document));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
    return static_cast<int>(ExitCode::kOk);
  }
  std::printf("epoch: %s\n", document.epoch.ToString().c_str());
  std::printf("updated_at: %s\n", FormatRfc3339(document.updated_at).c_str());
  std::printf("policy: %s@%s\n", document.policy.id.value().c_str(),
              document.policy.generation.ToString().c_str());
  std::printf("evidence (all entries recovered from persistence and therefore never fresh):\n");
  std::printf("%s\n", ToJson(document.evidence).DumpPretty(2).c_str());
  return static_cast<int>(ExitCode::kOk);
}

int CommandJournalVerify(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options, ParseOptions(arguments, {{"--state", true}, {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  const auto opened = DecisionLedger::Open(state_dir, DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  const auto verified = opened.value()->Verify();
  if (!verified.ok()) {
    return Fail(verified.error(), json);
  }
  const RecoveryReport& report = verified.value();
  if (json) {
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("journal verify"));
    members.emplace_back("valid", JsonValue::Bool(!report.tail_damaged));
    members.emplace_back("frames", JsonValue::String(std::to_string(report.frames_read)));
    members.emplace_back("decisions", JsonValue::String(std::to_string(report.decisions)));
    members.emplace_back("epoch_advances", JsonValue::String(std::to_string(report.epoch_advances)));
    members.emplace_back("state_snapshots", JsonValue::String(std::to_string(report.snapshots)));
    members.emplace_back("tail_damaged", JsonValue::Bool(report.tail_damaged));
    members.emplace_back("torn_tail_bytes", JsonValue::String(std::to_string(report.torn_tail_bytes_discarded)));
    members.emplace_back("current_epoch", JsonValue::String(report.current_epoch.ToString()));
    members.emplace_back("last_sequence", JsonValue::String(report.last_sequence.ToString()));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
  } else {
    std::printf("frames: %zu\n", report.frames_read);
    std::printf("decisions: %zu\n", report.decisions);
    std::printf("epoch advances: %zu\n", report.epoch_advances);
    std::printf("state snapshots: %zu\n", report.snapshots);
    std::printf("current epoch: %s\n", report.current_epoch.ToString().c_str());
    std::printf("last sequence: %s\n", report.last_sequence.ToString().c_str());
    std::printf("tail damaged: %s\n", report.tail_damaged ? "yes" : "no");
  }
  return static_cast<int>(ExitCode::kOk);
}

int CommandJournalShow(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options, ParseOptions(arguments, {{"--state", true}, {"--json", false}, {"--limit", true}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  std::size_t limit = 0;
  if (options.Has("--limit")) {
    try {
      limit = static_cast<std::size_t>(std::stoull(options.Get("--limit")));
    } catch (const std::exception&) {
      return Fail(MakeError(ErrorCode::kMalformedInput, "--limit", "must be a non-negative integer"), json);
    }
  }
  const auto opened = DecisionLedger::Open(state_dir, DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  std::vector<CommittedDecision> decisions = opened.value()->RecoveredDecisions();
  if (limit > 0 && decisions.size() > limit) {
    decisions.erase(decisions.begin(), decisions.end() - static_cast<std::ptrdiff_t>(limit));
  }
  if (json) {
    JsonValue::Array entries;
    entries.reserve(decisions.size());
    for (const CommittedDecision& committed : decisions) {
      JsonValue::Object entry;
      entry.emplace_back("sequence", JsonValue::String(committed.sequence.ToString()));
      entry.emplace_back("outcome", JsonValue::String(OutcomeName(committed.decision.outcome)));
      entry.emplace_back("decision_id", JsonValue::String(committed.decision.id.ToString()));
      entry.emplace_back("request_id", JsonValue::String(committed.decision.request_id.ToString()));
      entry.emplace_back("client_id", JsonValue::String(committed.decision.client_id.value()));
      entry.emplace_back("kind", JsonValue::String(RequestKindName(committed.decision.kind)));
      entry.emplace_back("decided_at", ToJson(committed.decision.decided_at));
      entries.push_back(JsonValue::ObjectValue(std::move(entry)));
    }
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("journal show"));
    members.emplace_back("count", JsonValue::String(std::to_string(decisions.size())));
    members.emplace_back("decisions", JsonValue::ArrayValue(std::move(entries)));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
  } else {
    std::printf("%zu committed decision(s)\n", decisions.size());
    for (const CommittedDecision& committed : decisions) {
      std::printf("  %s  %-13s %s  %s  %s\n", committed.sequence.ToString().c_str(),
                  OutcomeName(committed.decision.outcome),
                  committed.decision.id.ToString().c_str(),
                  RequestKindName(committed.decision.kind),
                  committed.decision.client_id.value().c_str());
    }
  }
  return static_cast<int>(ExitCode::kOk);
}

int CommandEpochAdvance(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options,
                ParseOptions(arguments, {{"--state", true}, {"--epoch", true}, {"--now", true}, {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  if (!options.Has("--epoch")) {
    return Fail(MakeError(ErrorCode::kMissingRequiredField, kProgram, "--epoch is required"), json);
  }
  ECG_TRY_LOCAL(next, Epoch::Parse(options.Get("--epoch")));
  ECG_TRY_LOCAL(now, ResolveNow(options, nullptr));
  const auto opened = DecisionLedger::Open(state_dir, DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  const Status advanced = opened.value()->AdvanceEpoch(next, now);
  if (!advanced.ok()) {
    return Fail(advanced.error(), json);
  }
  if (json) {
    std::printf("{\"command\":\"epoch advance\",\"epoch\":\"%s\"}\n", next.ToString().c_str());
  } else {
    std::printf("epoch advanced to %s\n", next.ToString().c_str());
  }
  return static_cast<int>(ExitCode::kOk);
}

int CommandIntentAcknowledge(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options,
                ParseOptions(arguments, {{"--state", true},
                                         {"--decision", true},
                                         {"--intent", true},
                                         {"--by", true},
                                         {"--at", true},
                                         {"--note", true},
                                         {"--json", false}}));
  const bool json = options.Flag("--json");
  ECG_TRY_LOCAL(state_dir, ResolveStateDir(options));
  for (const char* required : {"--decision", "--intent", "--by"}) {
    if (!options.Has(required)) {
      return Fail(MakeError(ErrorCode::kMissingRequiredField, kProgram,
                            std::string(required) + " is required"),
                  json);
    }
  }
  ECG_TRY_LOCAL(decision_id, DecisionId::Parse(options.Get("--decision")));
  ECG_TRY_LOCAL(intent_id, IntentId::Parse(options.Get("--intent")));
  ECG_TRY_LOCAL(by, ClientId::Parse(options.Get("--by")));
  UtcInstant at = SystemNow();
  if (options.Has("--at")) {
    ECG_TRY_LOCAL(parsed_at, ParseRfc3339(options.Get("--at")));
    at = parsed_at;
  }
  const auto opened = DecisionLedger::Open(state_dir, DefaultPolicy(), LedgerOptions{});
  if (!opened.ok()) {
    return Fail(opened.error(), json);
  }
  IntentAcknowledgement acknowledgement;
  acknowledgement.intent_id = intent_id;
  acknowledgement.decision_id = decision_id;
  acknowledgement.acknowledged_by = by;
  acknowledgement.acknowledged_at = at;
  acknowledgement.note = options.Get("--note");
  const Status recorded = opened.value()->RecordAcknowledgement(acknowledgement);
  if (!recorded.ok()) {
    return Fail(recorded.error(), json);
  }
  if (json) {
    std::printf("{\"command\":\"intent acknowledge\",\"recorded\":true,\"effect_observed\":false}\n");
  } else {
    std::printf("acknowledgement recorded for intent %s\n", intent_id.ToString().c_str());
    std::printf("note: an acknowledgement is not an effect; this runtime has not observed any load change\n");
  }
  return static_cast<int>(ExitCode::kOk);
}

/// Verifies the CLI's own contract: exit-code mapping and the self-verifying
/// decision encoding round-trip.
int CommandSelftest(const std::vector<std::string>& arguments) {
  ECG_TRY_LOCAL(options, ParseOptions(arguments, {{"--json", false}}));
  const bool json = options.Flag("--json");
  bool ok = true;
  const auto report = [&ok, json](const std::string& name, bool passed, const std::string& detail) {
    if (!passed) {
      ok = false;
    }
    if (!json) {
      const std::string suffix = detail.empty() ? std::string() : (" (" + detail + ")");
      std::printf("%-40s %s%s\n", name.c_str(), passed ? "PASS" : "FAIL", suffix.c_str());
    }
  };

  report("exit_code.allowed", ExitCodeForOutcome(Outcome::kAllowed) == 0, "");
  report("exit_code.refused", ExitCodeForOutcome(Outcome::kRefused) == 3, "");
  report("exit_code.deferred", ExitCodeForOutcome(Outcome::kDeferred) == 4, "");
  report("exit_code.indeterminate", ExitCodeForOutcome(Outcome::kIndeterminate) == 5, "");
  report("exit_code.lock_unavailable", ExitCodeForError(ErrorCode::kLockUnavailable) == 6, "");
  report("exit_code.interior_corruption", ExitCodeForError(ErrorCode::kInteriorCorruption) == 7, "");

  // A decision produced by the engine must survive the published encoding and
  // verify against its own digest.
  PolicySet policy = DefaultPolicy();
  const auto engine = GovernorEngine::Make(policy);
  report("engine.construct", engine.ok(), engine.ok() ? "" : engine.error().ToString());
  if (engine.ok()) {
    EvidenceSet evidence;
    evidence.epoch = policy.epoch;
    const auto now = ParseRfc3339("2026-03-01T00:00:00.000000Z");
    if (now.ok()) {
      DecisionRequest request;
      request.request_id = RequestId::FromRaw(1);
      request.client_id = ClientId::FromTrusted("ecg.selftest");
      request.epoch = policy.epoch;
      request.policy_generation = policy.generation;
      request.kind = RequestKind::kReserveRestoration;
      request.magnitude_kw = PowerKw::FromRaw(100000);
      request.duration = DurationSec::FromRaw(600);
      const auto window = Interval::Make(now.value(), UtcInstant::FromRaw(now.value().raw() + 600000000));
      if (window.ok()) {
        request.desired_window = window.value();
        const auto decision = engine.value().Evaluate(request, evidence, now.value());
        report("engine.evaluates_without_evidence", decision.ok(),
               decision.ok() ? "" : decision.error().ToString());
        if (decision.ok()) {
          report("engine.indeterminate_without_evidence",
                 decision.value().outcome == Outcome::kIndeterminate,
                 OutcomeName(decision.value().outcome));
          const std::string text = ToJson(decision.value()).Dump();
          const auto reparsed = JsonValue::Parse(text);
          report("json.decision_reparses", reparsed.ok(), "");
          if (reparsed.ok()) {
            const auto round_tripped = ParseDecision(reparsed.value());
            report("json.decision_digest_verifies", round_tripped.ok(),
                   round_tripped.ok() ? "" : round_tripped.error().ToString());
            if (round_tripped.ok()) {
              report("json.decision_is_stable",
                     round_tripped.value().digest == decision.value().digest, "");
            }
          }
        }
      }
    }
  }

  if (json) {
    JsonValue::Object members;
    members.emplace_back("command", JsonValue::String("selftest"));
    members.emplace_back("passed", JsonValue::Bool(ok));
    std::printf("%s\n", JsonValue::ObjectValue(std::move(members)).Dump().c_str());
  } else {
    std::printf("selftest: %s\n", ok ? "PASS" : "FAIL");
  }
  return ok ? static_cast<int>(ExitCode::kOk) : static_cast<int>(ExitCode::kInternal);
}

}  // namespace

int ExitCodeForOutcome(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kAllowed: return static_cast<int>(ExitCode::kOk);
    case Outcome::kRefused: return static_cast<int>(ExitCode::kRefused);
    case Outcome::kDeferred: return static_cast<int>(ExitCode::kDeferred);
    case Outcome::kIndeterminate: return static_cast<int>(ExitCode::kIndeterminate);
  }
  return static_cast<int>(ExitCode::kInternal);
}

int ExitCodeForError(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone:
      return static_cast<int>(ExitCode::kOk);
    case ErrorCode::kInvalidArgument:
    case ErrorCode::kMalformedInput:
    case ErrorCode::kOutOfRange:
    case ErrorCode::kUnsupportedValue:
    case ErrorCode::kMissingRequiredField:
    case ErrorCode::kNotFound:
    // A stale epoch or generation is an inadmissible submission, which is a
    // caller problem rather than an internal failure.
    case ErrorCode::kStaleReplayRejected:
    case ErrorCode::kEpochMismatch:
    case ErrorCode::kGenerationRegression:
      return static_cast<int>(ExitCode::kUsage);
    case ErrorCode::kIoFailure:
    case ErrorCode::kLockUnavailable:
    case ErrorCode::kPermissionDenied:
      return static_cast<int>(ExitCode::kPersistence);
    case ErrorCode::kIntegrityFailure:
    case ErrorCode::kInteriorCorruption:
    case ErrorCode::kUnsupportedVersion:
      return static_cast<int>(ExitCode::kIntegrity);
    case ErrorCode::kTornTail:
    case ErrorCode::kNumericOverflow:
    case ErrorCode::kDivisionByZero:
    case ErrorCode::kResourceLimitExceeded:
    case ErrorCode::kAlreadyExists:
    case ErrorCode::kStateConflict:
    case ErrorCode::kCancelled:
    case ErrorCode::kShuttingDown:
    case ErrorCode::kNotImplemented:
    case ErrorCode::kInternalInvariantViolation:
      return static_cast<int>(ExitCode::kInternal);
  }
  return static_cast<int>(ExitCode::kInternal);
}

void PrintUsage() {
  std::printf(
      "energy-cost-governor %s -- deterministic energy-cost policy decisions\n"
      "\n"
      "usage: %s <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                      print version and format versions\n"
      "  decide                       answer one decision request and commit it\n"
      "      --state <dir>            ledger directory (required)\n"
      "      --input <file|->         {\"request\":{...},\"evidence\":{...}[,\"now\",\"policy\"]}\n"
      "      --now <rfc3339>          evaluation instant (required for reproducibility)\n"
      "      --policy <file>          policy override\n"
      "      --json                   emit canonical JSON\n"
      "  state import                 publish an evidence snapshot\n"
      "      --state <dir> --input <file|-> [--epoch <n>] [--now <rfc3339>] [--json]\n"
      "  state show                   show the published snapshot (recovered provenance)\n"
      "  journal verify               verify every journal frame and chain link\n"
      "  journal show                 list committed decisions [--limit <n>]\n"
      "  epoch advance                advance the authority epoch (--epoch <n>)\n"
      "  intent acknowledge           record an acknowledgement (never an effect)\n"
      "      --state <dir> --decision <hex> --intent <hex> --by <client> [--at <rfc3339>]\n"
      "  selftest                     verify this binary's own contract\n"
      "\n"
      "exit codes: 0 allowed, 3 refused, 4 deferred, 5 indeterminate, 2 usage,\n"
      "            6 persistence or lock, 7 integrity, 1 internal\n",
      VersionString(), kProgram);
}

int RunCommand(const std::vector<std::string>& arguments) {
  g_json_output =
      std::find(arguments.begin(), arguments.end(), std::string("--json")) != arguments.end();
  if (arguments.empty()) {
    PrintUsage();
    return static_cast<int>(ExitCode::kUsage);
  }
  const std::string& command = arguments.front();
  std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  if (command == "help" || command == "--help" || command == "-h") {
    PrintUsage();
    return static_cast<int>(ExitCode::kOk);
  }
  if (command == "version" || command == "--version") {
    return CommandVersion(rest);
  }
  if (command == "decide") {
    return CommandDecide(rest);
  }
  if (command == "state") {
    if (rest.empty()) {
      std::fprintf(stderr, "%s: state requires a subcommand (import|show)\n", kProgram);
      return static_cast<int>(ExitCode::kUsage);
    }
    const std::string subcommand = rest.front();
    std::vector<std::string> sub_rest(rest.begin() + 1, rest.end());
    if (subcommand == "import") {
      return CommandStateImport(sub_rest);
    }
    if (subcommand == "show") {
      return CommandStateShow(sub_rest);
    }
    std::fprintf(stderr, "%s: unknown state subcommand '%s'\n", kProgram, subcommand.c_str());
    return static_cast<int>(ExitCode::kUsage);
  }
  if (command == "journal") {
    if (rest.empty()) {
      std::fprintf(stderr, "%s: journal requires a subcommand (verify|show)\n", kProgram);
      return static_cast<int>(ExitCode::kUsage);
    }
    const std::string subcommand = rest.front();
    std::vector<std::string> sub_rest(rest.begin() + 1, rest.end());
    if (subcommand == "verify") {
      return CommandJournalVerify(sub_rest);
    }
    if (subcommand == "show") {
      return CommandJournalShow(sub_rest);
    }
    std::fprintf(stderr, "%s: unknown journal subcommand '%s'\n", kProgram, subcommand.c_str());
    return static_cast<int>(ExitCode::kUsage);
  }
  if (command == "epoch") {
    if (rest.empty() || rest.front() != "advance") {
      std::fprintf(stderr, "%s: epoch requires the 'advance' subcommand\n", kProgram);
      return static_cast<int>(ExitCode::kUsage);
    }
    return CommandEpochAdvance(std::vector<std::string>(rest.begin() + 1, rest.end()));
  }
  if (command == "intent") {
    if (rest.empty() || rest.front() != "acknowledge") {
      std::fprintf(stderr, "%s: intent requires the 'acknowledge' subcommand\n", kProgram);
      return static_cast<int>(ExitCode::kUsage);
    }
    return CommandIntentAcknowledge(std::vector<std::string>(rest.begin() + 1, rest.end()));
  }
  if (command == "selftest") {
    return CommandSelftest(rest);
  }
  std::fprintf(stderr, "%s: unknown command '%s'\n", kProgram, command.c_str());
  PrintUsage();
  return static_cast<int>(ExitCode::kUsage);
}

}  // namespace ecg::cli
