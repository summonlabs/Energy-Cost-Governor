#ifndef ECG_ENGINE_HPP
#define ECG_ENGINE_HPP

// The decision engine.
//
// GovernorEngine::Evaluate is a pure function of (policy, request, evidence,
// now). It holds no mutable state, takes no locks, allocates only bounded
// storage, performs no I/O, and contacts no other runtime. That is what makes it
// safe to call concurrently from many threads and what makes a recorded decision
// replayable: identical inputs produce an identical DecisionId, byte for byte.

#include <cstddef>
#include <vector>

#include "ecg/decision.hpp"
#include "ecg/evidence.hpp"
#include "ecg/policy.hpp"
#include "ecg/request.hpp"
#include "ecg/result.hpp"

namespace ecg {

/// Public input bounds, so a caller can size buffers without reading internals.
inline constexpr std::size_t kMaxReasonsPerDecision = 4096;

class GovernorEngine {
 public:
  /// The policy is validated on construction; an invalid policy is rejected
  /// rather than silently defaulted.
  [[nodiscard]] static Result<GovernorEngine> Make(PolicySet policy);

  [[nodiscard]] const PolicySet& policy() const noexcept { return policy_; }

  /// Answers one decision request. Thread-safe and side-effect free.
  ///
  /// Failures are of three kinds, and they are deliberately distinct:
  ///   * A structurally invalid request, or an evidence bundle that violates its
  ///     own schema, is an error (kInvalidArgument, kOutOfRange, ...).
  ///   * An unanswerable question is Indeterminate, not an error.
  ///   * A prohibited action is Refused, not an error.
  [[nodiscard]] Result<Decision> Evaluate(const DecisionRequest& request,
                                          const EvidenceSet& evidence, UtcInstant now) const;

  /// Evaluates a batch in the given order. Deterministic and independent of the
  /// number of threads a caller wraps around it.
  [[nodiscard]] Result<std::vector<Decision>> EvaluateBatch(
      const std::vector<DecisionRequest>& requests, const EvidenceSet& evidence,
      UtcInstant now) const;

 private:
  explicit GovernorEngine(PolicySet policy) : policy_(std::move(policy)) {}

  PolicySet policy_;
};

[[nodiscard]] Result<GovernorEngine> MakeEngine(PolicySet policy);

}  // namespace ecg

#endif  // ECG_ENGINE_HPP
