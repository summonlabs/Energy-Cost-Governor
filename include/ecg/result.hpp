#ifndef ECG_RESULT_HPP
#define ECG_RESULT_HPP

// Error and Result plumbing for Energy-Cost-Governor.
//
// Every fallible operation in this runtime returns an explicit Result. There
// are no exceptions on the decision path: a caller must confront the failure
// value, which is what keeps "missing evidence never becomes permissive"
// enforceable rather than aspirational.

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ecg {

/// Debug-only invariant traps for Result accessors. In release builds the
/// accessor preconditions are the caller's responsibility; in debug builds a
/// violated precondition stops immediately instead of returning garbage.
#if defined(NDEBUG)
#define ECG_RESULT_CHECK_OK() ((void)0)
#define ECG_RESULT_CHECK_ERR() ((void)0)
#else
#define ECG_RESULT_CHECK_OK()       \
  do {                              \
    if (!ok()) {                    \
      std::abort();                 \
    }                               \
  } while (false)
#define ECG_RESULT_CHECK_ERR()      \
  do {                              \
    if (ok()) {                     \
      std::abort();                 \
    }                               \
  } while (false)
#endif

/// Stable, serialisable failure taxonomy. Values are part of the persisted and
/// CLI-visible contract and must not be renumbered.
enum class ErrorCode : std::uint16_t {
  kNone = 0,

  // Input contract violations.
  kInvalidArgument = 1,
  kMalformedInput = 2,
  kOutOfRange = 3,
  kUnsupportedValue = 4,
  kMissingRequiredField = 5,

  // Numeric domain.
  kNumericOverflow = 20,
  kDivisionByZero = 21,

  // Resource bounds.
  kResourceLimitExceeded = 30,

  // State and identity domain.
  kNotFound = 40,
  kAlreadyExists = 41,
  kStateConflict = 42,
  kEpochMismatch = 43,
  kGenerationRegression = 44,
  kStaleReplayRejected = 45,
  kUnsupportedVersion = 46,

  // Persistence and integrity.
  kIoFailure = 60,
  kIntegrityFailure = 61,
  kInteriorCorruption = 62,
  kTornTail = 63,
  kLockUnavailable = 64,
  kPermissionDenied = 65,

  // Lifecycle.
  kCancelled = 80,
  kShuttingDown = 81,
  kNotImplemented = 82,
  kInternalInvariantViolation = 83,
};

/// Canonical, stable name for an error code (lower snake case, ASCII).
[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

/// Stable category used by the CLI mapping and by operator dashboards.
[[nodiscard]] std::string_view ErrorCodeCategory(ErrorCode code) noexcept;

/// A failure value: code, human-readable message, and the subject the failure
/// is about (a request id, a file path, an evidence source). The subject is what
/// lets an operator answer "which thing failed?" without scraping logs.
class Error {
 public:
  Error() = default;

  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  Error(ErrorCode code, std::string subject, std::string message)
      : code_(code), subject_(std::move(subject)), message_(std::move(message)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& subject() const noexcept { return subject_; }

  /// "subject: message" when a subject is present, otherwise "message".
  [[nodiscard]] std::string ToString() const;

 private:
  ErrorCode code_{ErrorCode::kNone};
  std::string subject_;
  std::string message_;
};

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string subject, std::string message) {
  return Error(code, std::move(subject), std::move(message));
}

/// A value or an error, never both and never neither.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  /// Precondition: ok().
  [[nodiscard]] T& value() & {
    ECG_RESULT_CHECK_OK();
    return std::get<0>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    ECG_RESULT_CHECK_OK();
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    ECG_RESULT_CHECK_OK();
    return std::get<0>(std::move(storage_));
  }

  /// Precondition: !ok().
  [[nodiscard]] const Error& error() const& {
    ECG_RESULT_CHECK_ERR();
    return std::get<1>(storage_);
  }

  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }

 private:
  std::variant<T, Error> storage_;
};

/// Result of an operation that produces no value.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const& {
    ECG_RESULT_CHECK_ERR();
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status OkStatus() { return Status{}; }

/// Propagates the error of an expression, binding its value to a name on
/// success. Used pervasively so failure handling stays visible at call sites.
#define ECG_TRY(temp, expr)         \
  auto temp##_result = (expr);      \
  if (!temp##_result.ok()) {        \
    return temp##_result.error();   \
  }                                 \
  auto& temp = temp##_result.value()

/// Propagates a Status failure in a function that returns a Result.
#define ECG_TRY_STATUS(expr)        \
  do {                              \
    const ::ecg::Status status_ = (expr); \
    if (!status_.ok()) {            \
      return status_.error();       \
    }                               \
  } while (false)

}  // namespace ecg

#endif  // ECG_RESULT_HPP
