#ifndef ECG_CLI_COMMANDS_HPP
#define ECG_CLI_COMMANDS_HPP

#include <string>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/result.hpp"

namespace ecg::cli {

/// Process exit codes. These are part of the automation contract: a script can
/// branch on the outcome of a decision without parsing output.
enum class ExitCode : int {
  kOk = 0,
  kInternal = 1,
  kUsage = 2,
  /// The decision was Refused.
  kRefused = 3,
  /// The decision was Deferred.
  kDeferred = 4,
  /// The decision was Indeterminate.
  kIndeterminate = 5,
  /// Persistence failed, or the writer lock is held by another process.
  kPersistence = 6,
  /// Integrity failure: corrupt or unsupported journal content.
  kIntegrity = 7,
};

[[nodiscard]] int ExitCodeForOutcome(Outcome outcome) noexcept;
[[nodiscard]] int ExitCodeForError(ErrorCode code) noexcept;

/// Runs one command line (arguments exclude the program name).
[[nodiscard]] int RunCommand(const std::vector<std::string>& arguments);

void PrintUsage();

}  // namespace ecg::cli

#endif  // ECG_CLI_COMMANDS_HPP
