#ifndef ECG_REQUEST_HPP
#define ECG_REQUEST_HPP

// Decision requests: the questions this runtime is asked.
//
// A request names an energy-cost-sensitive operating decision, the service class
// it would affect, how much would move, and over which window. It never names a
// device, a workload, or an actuator: those belong to adjacent authorities, and
// a request that reached into them would be an ownership violation.

#include <cstdint>
#include <string>
#include <string_view>

#include "ecg/quantity.hpp"
#include "ecg/result.hpp"
#include "ecg/strong.hpp"
#include "ecg/time.hpp"

namespace ecg {

/// Kinds of decision this runtime is allowed to answer.
enum class RequestKind : std::uint8_t {
  /// Move flexible load out of the requested window into a cheaper one.
  kShiftFlexibleLoad = 1,
  /// Reduce a service class's load for the window.
  kCurtailServiceClass = 2,
  /// Defer batch work that is not service critical.
  kDeferBatchWork = 3,
  /// Charge storage during the window.
  kChargeStorage = 4,
  /// Discharge storage during the window.
  kDischargeStorage = 5,
  /// Pre-cool or pre-condition ahead of a peak.
  kPreCooling = 6,
  /// Restore reserve headroom by reducing load; never blocked by economic tests.
  kReserveRestoration = 7,
};

[[nodiscard]] const char* RequestKindName(RequestKind kind) noexcept;
[[nodiscard]] Result<RequestKind> ParseRequestKind(std::string_view name);

/// Which way the requested action moves metered demand.
enum class EffectDirection : std::uint8_t {
  kReduceLoad = 0,
  kIncreaseLoad = 1,
  kNeutral = 2,
};

[[nodiscard]] EffectDirection DirectionOf(RequestKind kind) noexcept;

/// Why the requester wants the decision. Recorded for audit; it never widens
/// authorisation.
enum class Rationale : std::uint8_t {
  kDemandChargeAvoidance = 0,
  kPriceArbitrage = 1,
  kEfficiencyRebalance = 2,
  kReserveRestoration = 3,
  kObligationCompliance = 4,
};

[[nodiscard]] const char* RationaleName(Rationale rationale) noexcept;
[[nodiscard]] Result<Rationale> ParseRationale(std::string_view name);

/// A complete question. Every field is required except the service class, which
/// is required for kinds that target one.
struct DecisionRequest {
  RequestId request_id;
  ClientId client_id;
  /// Authority epoch the caller believes is current.
  Epoch epoch;
  /// Policy generation the caller evaluated against.
  PolicyGeneration policy_generation;
  RequestKind kind{RequestKind::kShiftFlexibleLoad};
  /// Service class the action affects; empty for kinds that affect none.
  ServiceClassId service_class;
  /// How much load would move. Must be positive.
  PowerKw magnitude_kw;
  /// How long the action would last. Must be positive.
  DurationSec duration;
  /// When the requester wants it to happen.
  Interval desired_window;
  Rationale rationale{Rationale::kPriceArbitrage};
  /// When the request was formed. Used for audit ordering only.
  UtcInstant requested_at;
  /// Client-side retry counter; the ledger uses (client_id, request_id) for
  /// idempotency and records this value for audit.
  std::uint32_t attempt{0};

  [[nodiscard]] bool TargetsServiceClass() const noexcept;
};

/// Structural validation. A failure here is a caller defect, reported as an
/// error rather than as a decision outcome.
[[nodiscard]] Status ValidateRequest(const DecisionRequest& request);

}  // namespace ecg

#endif  // ECG_REQUEST_HPP
