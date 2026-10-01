#include "ecg/result.hpp"

namespace ecg {

std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone: return "none";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kMalformedInput: return "malformed_input";
    case ErrorCode::kOutOfRange: return "out_of_range";
    case ErrorCode::kUnsupportedValue: return "unsupported_value";
    case ErrorCode::kMissingRequiredField: return "missing_required_field";
    case ErrorCode::kNumericOverflow: return "numeric_overflow";
    case ErrorCode::kDivisionByZero: return "division_by_zero";
    case ErrorCode::kResourceLimitExceeded: return "resource_limit_exceeded";
    case ErrorCode::kNotFound: return "not_found";
    case ErrorCode::kAlreadyExists: return "already_exists";
    case ErrorCode::kStateConflict: return "state_conflict";
    case ErrorCode::kEpochMismatch: return "epoch_mismatch";
    case ErrorCode::kGenerationRegression: return "generation_regression";
    case ErrorCode::kStaleReplayRejected: return "stale_replay_rejected";
    case ErrorCode::kUnsupportedVersion: return "unsupported_version";
    case ErrorCode::kIoFailure: return "io_failure";
    case ErrorCode::kIntegrityFailure: return "integrity_failure";
    case ErrorCode::kInteriorCorruption: return "interior_corruption";
    case ErrorCode::kTornTail: return "torn_tail";
    case ErrorCode::kLockUnavailable: return "lock_unavailable";
    case ErrorCode::kPermissionDenied: return "permission_denied";
    case ErrorCode::kCancelled: return "cancelled";
    case ErrorCode::kShuttingDown: return "shutting_down";
    case ErrorCode::kNotImplemented: return "not_implemented";
    case ErrorCode::kInternalInvariantViolation: return "internal_invariant_violation";
  }
  return "unknown";
}

std::string_view ErrorCodeCategory(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone: return "ok";
    case ErrorCode::kInvalidArgument:
    case ErrorCode::kMalformedInput:
    case ErrorCode::kOutOfRange:
    case ErrorCode::kUnsupportedValue:
    case ErrorCode::kMissingRequiredField: return "input";
    case ErrorCode::kNumericOverflow:
    case ErrorCode::kDivisionByZero: return "numeric";
    case ErrorCode::kResourceLimitExceeded: return "resource";
    case ErrorCode::kNotFound:
    case ErrorCode::kAlreadyExists:
    case ErrorCode::kStateConflict:
    case ErrorCode::kEpochMismatch:
    case ErrorCode::kGenerationRegression:
    case ErrorCode::kStaleReplayRejected:
    case ErrorCode::kUnsupportedVersion: return "state";
    case ErrorCode::kIoFailure:
    case ErrorCode::kIntegrityFailure:
    case ErrorCode::kInteriorCorruption:
    case ErrorCode::kTornTail:
    case ErrorCode::kLockUnavailable:
    case ErrorCode::kPermissionDenied: return "persistence";
    case ErrorCode::kCancelled:
    case ErrorCode::kShuttingDown:
    case ErrorCode::kNotImplemented:
    case ErrorCode::kInternalInvariantViolation: return "lifecycle";
  }
  return "unknown";
}

std::string Error::ToString() const {
  if (subject_.empty()) {
    return message_;
  }
  std::string out;
  out.reserve(subject_.size() + message_.size() + 2);
  out.append(subject_);
  out.append(": ");
  out.append(message_);
  return out;
}

}  // namespace ecg
