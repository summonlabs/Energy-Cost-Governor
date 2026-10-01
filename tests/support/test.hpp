#ifndef ECG_TEST_SUPPORT_HPP
#define ECG_TEST_SUPPORT_HPP

// A small, dependency-free test framework.
//
// It deliberately has no timeout machinery: a hang in this project is a defect to
// diagnose, not a condition to paper over by killing the test. Tests either
// finish or they fail; nothing in this framework can hide a deadlock.
//
// Every assertion records a failure and continues where continuing is safe
// (ECG_CHECK), or stops the current test (ECG_REQUIRE) when later assertions
// would be meaningless or unsafe. Failures name the file and line that produced
// them.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/quantity.hpp"
#include "ecg/result.hpp"

namespace ecgtest {

/// One registered test.
struct TestCase {
  std::string name;
  void (*function)();
};

/// Process-wide registry, populated by static registration.
class Registry {
 public:
  static Registry& Instance() {
    static Registry registry;
    return registry;
  }

  void Add(std::string name, void (*function)()) {
    tests_.push_back(TestCase{std::move(name), function});
  }

  [[nodiscard]] const std::vector<TestCase>& tests() const { return tests_; }

 private:
  std::vector<TestCase> tests_;
};

/// State for the test currently running.
class CurrentTest {
 public:
  static CurrentTest& Instance() {
    static CurrentTest current;
    return current;
  }

  void Begin(const std::string& name) {
    name_ = name;
    failures_ = 0;
    checks_ = 0;
    aborted_ = false;
    messages_.clear();
  }

  void RecordCheck() { ++checks_; }

  void Fail(const char* file, int line, std::string message) {
    ++failures_;
    messages_.push_back(std::string(file) + ":" + std::to_string(line) + ": " + std::move(message));
  }

  void Abort() { aborted_ = true; }

  [[nodiscard]] bool aborted() const { return aborted_; }
  [[nodiscard]] int failures() const { return failures_; }
  [[nodiscard]] int checks() const { return checks_; }
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::vector<std::string>& messages() const { return messages_; }

 private:
  std::string name_;
  int failures_{0};
  int checks_{0};
  bool aborted_{false};
  std::vector<std::string> messages_;
};

/// Deterministic pseudo-random generator for seeded randomised tests.
/// xorshift64* -- small, fast, and reproducible on every platform.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  /// Uniform value in [0, bound).
  [[nodiscard]] std::uint64_t Below(std::uint64_t bound) {
    if (bound == 0) {
      return 0;
    }
    return Next() % bound;
  }

  /// Uniform value in [low, high].
  [[nodiscard]] std::int64_t Range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    const auto span = static_cast<std::uint64_t>(high - low) + 1u;
    return low + static_cast<std::int64_t>(Below(span));
  }

  [[nodiscard]] bool Coin() { return (Next() & 1u) != 0u; }

 private:
  std::uint64_t state_;
};

/// Returns the current process id.
[[nodiscard]] unsigned long ProcessId();

/// Runs a child process to completion and returns its exit code. Child processes
/// report their own detail through files they are given, which keeps this helper
/// free of platform-specific pipe handling.
[[nodiscard]] int RunChildProcess(const std::filesystem::path& executable,
                                  const std::vector<std::string>& arguments);

/// The captured result of a child process.
struct ChildOutput {
  int exit_code{-1};
  std::string out;
  std::string err;
};

/// Runs a child process to completion, capturing its standard output and error.
///
/// This deliberately does not go through a command shell: arguments are passed
/// as an argv array and the streams are redirected with real file handles, so no
/// quoting, escaping, or shell built-in can change what is executed.
[[nodiscard]] ChildOutput RunChildProcessCapture(const std::filesystem::path& executable,
                                                 const std::vector<std::string>& arguments);

/// Reads a whole text file. Returns false when it cannot be read.
[[nodiscard]] bool ReadTextFile(const std::filesystem::path& path, std::string* out);

/// Writes a whole text file.
[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path, std::string_view text);

/// Creates a uniquely named temporary directory and removes it on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& label) {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 256; ++attempt) {
      path_ = base / ("ecg_test_" + label + "_" + std::to_string(ProcessId()) + "_" +
                      std::to_string(attempt));
      std::error_code ec;
      if (std::filesystem::create_directories(path_, ec)) {
        return;
      }
    }
    std::fprintf(stderr, "TempDir: unable to create a temporary directory\n");
    std::abort();
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path File(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

/// Renders a value for failure messages.
[[nodiscard]] inline std::string Describe(const std::string& value) { return "\"" + value + "\""; }
[[nodiscard]] inline std::string Describe(const char* value) {
  return std::string("\"") + (value == nullptr ? "(null)" : value) + "\"";
}
[[nodiscard]] inline std::string Describe(const std::filesystem::path& value) {
  return "\"" + value.string() + "\"";
}
[[nodiscard]] inline std::string Describe(bool value) { return value ? "true" : "false"; }
/// Renders a quantity in its declared unit, which is far more useful in a
/// failure message than a raw scaled integer.
template <class Tag>
[[nodiscard]] inline std::string Describe(const ecg::Quantity<Tag>& value) {
  return ecg::FormatQuantity(value) + " " + std::string(ecg::QuantityTraits<Tag>::kUnit);
}

// Domain enumerations render as their canonical names, so a failure message
// says "indeterminate" rather than "3".
[[nodiscard]] inline std::string Describe(ecg::Outcome value) { return ecg::OutcomeName(value); }
[[nodiscard]] inline std::string Describe(ecg::ReasonSeverity value) {
  return ecg::ReasonSeverityName(value);
}
[[nodiscard]] inline std::string Describe(ecg::ReasonCode value) { return ecg::ReasonCodeName(value); }
[[nodiscard]] inline std::string Describe(ecg::Freshness value) { return ecg::FreshnessName(value); }
[[nodiscard]] inline std::string Describe(ecg::Provenance value) { return ecg::ProvenanceName(value); }
[[nodiscard]] inline std::string Describe(ecg::IncidentSeverity value) {
  return ecg::IncidentSeverityName(value);
}
[[nodiscard]] inline std::string Describe(ecg::RiskPosture value) { return ecg::RiskPostureName(value); }
[[nodiscard]] inline std::string Describe(ecg::CurtailmentPermission value) {
  return ecg::CurtailmentPermissionName(value);
}
[[nodiscard]] inline std::string Describe(ecg::DemandBasis value) { return ecg::DemandBasisName(value); }
[[nodiscard]] inline std::string Describe(ecg::RequestKind value) { return ecg::RequestKindName(value); }
[[nodiscard]] inline std::string Describe(ecg::Rationale value) { return ecg::RationaleName(value); }
[[nodiscard]] inline std::string Describe(ecg::AuthorityKind value) {
  return ecg::AuthorityKindName(value);
}
[[nodiscard]] inline std::string Describe(ecg::ErrorCode value) {
  return std::string(ecg::ErrorCodeName(value));
}

/// Renders any arithmetic value with std::to_string, any enumeration as its
/// underlying value, and any other value with its own ToString(). Keeping this a
/// single template avoids the overload ambiguity that fixed-width aliases create
/// across platforms.
template <class T>
[[nodiscard]] inline std::string Describe(const T& value) {
  if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return value.ToString();
  }
}

}  // namespace ecgtest

// ---------------------------------------------------------------------------
// Registration and assertion macros
// ---------------------------------------------------------------------------

#define ECG_TEST_IMPL2(counter, name)                                       \
  static void ecg_test_body_##counter();                                    \
  namespace {                                                               \
  struct EcgTestRegistrar_##counter {                                       \
    EcgTestRegistrar_##counter() {                                          \
      ::ecgtest::Registry::Instance().Add(name, &ecg_test_body_##counter);  \
    }                                                                       \
  } ecg_test_registrar_##counter;                                           \
  }                                                                         \
  static void ecg_test_body_##counter()

#define ECG_TEST_IMPL(counter, name) ECG_TEST_IMPL2(counter, name)
#define ECG_TEST(name) ECG_TEST_IMPL(__COUNTER__, name)

#define ECG_CHECK(condition)                                           \
  do {                                                                 \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                  \
    if (!(condition)) {                                                \
      ::ecgtest::CurrentTest::Instance().Fail(__FILE__, __LINE__,      \
                                              "check failed: " #condition); \
    }                                                                  \
  } while (false)

// The message is normalised through std::string so that const char*,
// std::string, and std::string_view arguments all work: there is no
// operator+ between std::string and std::string_view.
#define ECG_CHECK_MSG(condition, message)                                        \
  do {                                                                           \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                            \
    if (!(condition)) {                                                          \
      ::ecgtest::CurrentTest::Instance().Fail(                                   \
          __FILE__, __LINE__,                                                    \
          std::string("check failed: " #condition) + " (" +                      \
              std::string(message) + ")");                                       \
    }                                                                            \
  } while (false)

#define ECG_REQUIRE(condition)                                         \
  do {                                                                 \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                  \
    if (!(condition)) {                                                \
      ::ecgtest::CurrentTest::Instance().Fail(__FILE__, __LINE__,      \
                                              "requirement failed: " #condition); \
      ::ecgtest::CurrentTest::Instance().Abort();                      \
      return;                                                          \
    }                                                                  \
  } while (false)

#define ECG_CHECK_EQ(actual, expected)                                            \
  do {                                                                            \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                             \
    const auto& ecg_actual = (actual);                                            \
    const auto& ecg_expected = (expected);                                        \
    if (!(ecg_actual == ecg_expected)) {                                          \
      ::ecgtest::CurrentTest::Instance().Fail(                                    \
          __FILE__, __LINE__,                                                     \
          std::string("expected " #actual " == " #expected " (") +                \
              ::ecgtest::Describe(ecg_actual) + " vs " + ::ecgtest::Describe(ecg_expected) + ")"); \
    }                                                                             \
  } while (false)

#define ECG_CHECK_OK(expr)                                                       \
  do {                                                                           \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                            \
    const auto& ecg_result = (expr);                                             \
    if (!ecg_result.ok()) {                                                      \
      ::ecgtest::CurrentTest::Instance().Fail(                                   \
          __FILE__, __LINE__,                                                    \
          std::string("expected success from " #expr ", got ") +                 \
              std::string(::ecg::ErrorCodeName(ecg_result.error().code())) + ": " + \
              ecg_result.error().ToString());                                    \
    }                                                                            \
  } while (false)

#define ECG_CHECK_ERR(expr, expected_code)                                         \
  do {                                                                             \
    ::ecgtest::CurrentTest::Instance().RecordCheck();                              \
    const auto& ecg_result = (expr);                                               \
    if (ecg_result.ok()) {                                                         \
      ::ecgtest::CurrentTest::Instance().Fail(__FILE__, __LINE__,                  \
                                              "expected failure from " #expr);     \
    } else if (ecg_result.error().code() != (expected_code)) {                     \
      ::ecgtest::CurrentTest::Instance().Fail(                                     \
          __FILE__, __LINE__,                                                      \
          std::string("expected ") + std::string(::ecg::ErrorCodeName(expected_code)) + \
              " from " #expr ", got " +                                          \
              std::string(::ecg::ErrorCodeName(ecg_result.error().code())) + ": " + \
              ecg_result.error().ToString());                                    \
    }                                                                              \
  } while (false)

#endif  // ECG_TEST_SUPPORT_HPP
