// End-to-end tests of the runtime's operator surface: the real ecg executable,
// launched as a separate process with redirected streams, real files, and real
// exit codes. No shell is involved, so quoting cannot change what runs.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ecg/json.hpp"
#include "ecg/json_io.hpp"
#include "fixtures.hpp"
#include "test.hpp"

using namespace ecgtest;

namespace {

struct CliRun {
  int exit_code{-1};
  std::string out;
  std::string err;
};

std::filesystem::path CliExecutable() {
  const char* value = std::getenv("ECG_TEST_CLI");
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return std::filesystem::path(value);
}

CliRun RunCli(const std::filesystem::path& cli, const std::vector<std::string>& arguments) {
  const ChildOutput captured = RunChildProcessCapture(cli, arguments);
  CliRun run;
  run.exit_code = captured.exit_code;
  run.out = captured.out;
  run.err = captured.err;
  return run;
}

std::string DecideDocument(const Scenario& scenario, const ecg::DecisionRequest& request,
                           const ecg::EvidenceSet& evidence) {
  ecg::JsonValue::Object members;
  members.emplace_back("request", ecg::ToJson(request));
  members.emplace_back("evidence", ecg::ToJson(evidence));
  members.emplace_back("now", ecg::ToJson(scenario.now));
  members.emplace_back("policy", ecg::ToJson(scenario.policy));
  return ecg::JsonValue::ObjectValue(std::move(members)).Dump();
}

bool JsonHasOutcome(const std::string& text, const char* outcome) {
  return text.find(std::string("\"outcome\":\"") + outcome + "\"") != std::string::npos;
}

std::vector<std::string> DecideArguments(const std::filesystem::path& state,
                                         const std::filesystem::path& input) {
  return {"decide",          "--state", state.string(), "--input",
          input.string(),    "--now",   "2026-03-02T12:00:00.000000Z", "--json"};
}

}  // namespace

ECG_TEST("cli.version_reports_the_library_and_format_versions") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const CliRun run = RunCli(cli, {"version", "--json"});
  ECG_CHECK_EQ(run.exit_code, 0);
  ECG_CHECK(run.out.find("\"version\":\"1.0.0\"") != std::string::npos);
  ECG_CHECK(run.out.find("\"journal_format_version\"") != std::string::npos);
  ECG_CHECK(run.out.find("\"encoding_version\"") != std::string::npos);
}

ECG_TEST("cli.selftest_passes_in_the_installed_binary") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const CliRun run = RunCli(cli, {"selftest", "--json"});
  ECG_CHECK_EQ(run.exit_code, 0);
  ECG_CHECK(run.out.find("\"passed\":true") != std::string::npos);
}

ECG_TEST("cli.unknown_option_is_a_usage_error") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const CliRun run = RunCli(cli, {"version", "--not-an-option"});
  ECG_CHECK_EQ(run.exit_code, 2);
  ECG_CHECK(run.err.find("unknown option") != std::string::npos);
}

ECG_TEST("cli.allowed_decision_exits_zero_and_is_journalled") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_allowed");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("decide.json");
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 501, ecg::RequestKind::kShiftFlexibleLoad, Kw(400), Secs(1800),
                  Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"), "vm.flex");
  ECG_REQUIRE(WriteTextFile(input, DecideDocument(scenario, request, scenario.evidence)));

  const CliRun run = RunCli(cli, DecideArguments(state, input));
  ECG_CHECK_EQ(run.exit_code, 0);
  ECG_CHECK(JsonHasOutcome(run.out, "allowed"));
  ECG_CHECK(run.out.find("\"duplicate\":false") != std::string::npos);

  const CliRun verify = RunCli(cli, {"journal", "verify", "--state", state.string(), "--json"});
  ECG_CHECK_EQ(verify.exit_code, 0);
  ECG_CHECK(verify.out.find("\"valid\":true") != std::string::npos);
  ECG_CHECK(verify.out.find("\"decisions\":\"1\"") != std::string::npos);

  const CliRun show = RunCli(cli, {"journal", "show", "--state", state.string(), "--json"});
  ECG_CHECK_EQ(show.exit_code, 0);
  ECG_CHECK(show.out.find("\"count\":\"1\"") != std::string::npos);
}

ECG_TEST("cli.repeated_identical_request_is_reported_as_a_duplicate") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_duplicate");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("decide.json");
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 502, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                  Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
  ECG_REQUIRE(WriteTextFile(input, DecideDocument(scenario, request, scenario.evidence)));

  const std::vector<std::string> arguments = DecideArguments(state, input);
  const CliRun first = RunCli(cli, arguments);
  ECG_CHECK_EQ(first.exit_code, 0);
  const CliRun second = RunCli(cli, arguments);
  ECG_CHECK_EQ(second.exit_code, 0);
  ECG_CHECK(second.out.find("\"duplicate\":true") != std::string::npos);

  const CliRun show = RunCli(cli, {"journal", "show", "--state", state.string(), "--json"});
  ECG_CHECK_EQ(show.exit_code, 0);
  ECG_CHECK(show.out.find("\"count\":\"1\"") != std::string::npos);
}

ECG_TEST("cli.exit_codes_distinguish_every_outcome") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_outcomes");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("decide.json");

  // Refused: the service class is protected.
  ECG_REQUIRE(WriteTextFile(
      input,
      DecideDocument(scenario,
                     MakeRequest(scenario, 510, ecg::RequestKind::kCurtailServiceClass, Kw(500),
                                 Secs(1800),
                                 Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"),
                                 "vm.critical"),
                     scenario.evidence)));
  const CliRun refused = RunCli(cli, DecideArguments(state, input));
  ECG_CHECK_EQ(refused.exit_code, 3);
  ECG_CHECK(JsonHasOutcome(refused.out, "refused"));

  // Deferred: the window lies beyond the decision horizon.
  ECG_REQUIRE(WriteTextFile(
      input,
      DecideDocument(scenario,
                     MakeRequest(scenario, 511, ecg::RequestKind::kShiftFlexibleLoad, Kw(400),
                                 Secs(1800),
                                 Span("2026-03-03T18:30:00.000000Z", "2026-03-03T19:00:00.000000Z"),
                                 "vm.flex"),
                     scenario.evidence)));
  const CliRun deferred = RunCli(cli, DecideArguments(state, input));
  ECG_CHECK_EQ(deferred.exit_code, 4);
  ECG_CHECK(JsonHasOutcome(deferred.out, "deferred"));

  // Indeterminate: a required evidence category is absent.
  ecg::EvidenceSet sparse = scenario.evidence;
  sparse.risk.reset();
  ECG_REQUIRE(WriteTextFile(
      input,
      DecideDocument(scenario,
                     MakeRequest(scenario, 512, ecg::RequestKind::kShiftFlexibleLoad, Kw(400),
                                 Secs(1800),
                                 Span("2026-03-02T18:30:00.000000Z", "2026-03-02T19:00:00.000000Z"),
                                 "vm.flex"),
                     sparse)));
  const CliRun indeterminate = RunCli(cli, DecideArguments(state, input));
  ECG_CHECK_EQ(indeterminate.exit_code, 5);
  ECG_CHECK(JsonHasOutcome(indeterminate.out, "indeterminate"));
}

ECG_TEST("cli.state_import_then_show_reports_recovered_provenance") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_state");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("state.json");
  ecg::JsonValue::Object members;
  members.emplace_back("evidence", ecg::ToJson(scenario.evidence));
  members.emplace_back("policy", ecg::ToJson(scenario.policy));
  ECG_REQUIRE(WriteTextFile(input, ecg::JsonValue::ObjectValue(std::move(members)).Dump()));

  const CliRun imported =
      RunCli(cli, {"state", "import", "--state", state.string(), "--input", input.string(), "--now",
                   "2026-03-02T12:00:00.000000Z", "--json"});
  ECG_CHECK_EQ(imported.exit_code, 0);

  const CliRun shown = RunCli(cli, {"state", "show", "--state", state.string(), "--json"});
  ECG_CHECK_EQ(shown.exit_code, 0);
  ECG_CHECK(shown.out.find("recovered_persistence") != std::string::npos);
}

ECG_TEST("cli.stale_epoch_submission_is_refused_as_an_input_error") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_epoch");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("decide.json");

  const CliRun advance =
      RunCli(cli, {"epoch", "advance", "--state", state.string(), "--epoch", "4", "--now",
                   "2026-03-02T12:00:00.000000Z", "--json"});
  ECG_CHECK_EQ(advance.exit_code, 0);

  // The request still declares epoch 1, which is no longer in force.
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 520, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                  Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
  ECG_REQUIRE(WriteTextFile(input, DecideDocument(scenario, request, scenario.evidence)));
  const CliRun run = RunCli(cli, DecideArguments(state, input));
  ECG_CHECK_EQ(run.exit_code, 2);
  ECG_CHECK(run.err.find("stale_replay_rejected") != std::string::npos);
}

ECG_TEST("cli.acknowledgement_records_but_never_claims_an_effect") {
  const std::filesystem::path cli = CliExecutable();
  ECG_REQUIRE(!cli.empty());
  const Scenario scenario = MakeScenario();
  TempDir scratch("cli_ack");
  const std::filesystem::path state = scratch.File("state");
  const std::filesystem::path input = scratch.File("decide.json");
  const ecg::DecisionRequest request =
      MakeRequest(scenario, 530, ecg::RequestKind::kReserveRestoration, Kw(400), Secs(600),
                  Span("2026-03-02T12:05:00.000000Z", "2026-03-02T12:15:00.000000Z"));
  ECG_REQUIRE(WriteTextFile(input, DecideDocument(scenario, request, scenario.evidence)));
  const CliRun decided = RunCli(cli, DecideArguments(state, input));
  ECG_REQUIRE(decided.exit_code == 0);

  const auto parsed = ecg::JsonValue::Parse(decided.out);
  ECG_REQUIRE(parsed.ok());
  const ecg::JsonValue* decision = parsed.value().Find("decision");
  ECG_REQUIRE(decision != nullptr);
  const ecg::JsonValue* intent = decision->Find("intent");
  ECG_REQUIRE(intent != nullptr);
  const ecg::JsonValue* intent_id = intent->Find("id");
  const ecg::JsonValue* decision_id = decision->Find("id");
  ECG_REQUIRE(intent_id != nullptr && decision_id != nullptr && intent_id->is_string() &&
              decision_id->is_string());

  const CliRun acked = RunCli(cli, {"intent", "acknowledge", "--state", state.string(),
                                    "--decision", decision_id->as_string(), "--intent",
                                    intent_id->as_string(), "--by", "capacity.authority", "--json"});
  ECG_CHECK_EQ(acked.exit_code, 0);
  ECG_CHECK(acked.out.find("\"effect_observed\":false") != std::string::npos);

  const CliRun verify = RunCli(cli, {"journal", "verify", "--state", state.string(), "--json"});
  ECG_CHECK_EQ(verify.exit_code, 0);
  ECG_CHECK(verify.out.find("\"decisions\":\"1\"") != std::string::npos);
}
