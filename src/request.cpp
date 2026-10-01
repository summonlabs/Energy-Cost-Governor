#include "ecg/request.hpp"

namespace ecg {

const char* RequestKindName(RequestKind kind) noexcept {
  switch (kind) {
    case RequestKind::kShiftFlexibleLoad: return "shift_flexible_load";
    case RequestKind::kCurtailServiceClass: return "curtail_service_class";
    case RequestKind::kDeferBatchWork: return "defer_batch_work";
    case RequestKind::kChargeStorage: return "charge_storage";
    case RequestKind::kDischargeStorage: return "discharge_storage";
    case RequestKind::kPreCooling: return "pre_cooling";
    case RequestKind::kReserveRestoration: return "reserve_restoration";
  }
  return "unknown";
}

Result<RequestKind> ParseRequestKind(std::string_view name) {
  constexpr RequestKind kKinds[] = {
      RequestKind::kShiftFlexibleLoad, RequestKind::kCurtailServiceClass,
      RequestKind::kDeferBatchWork,    RequestKind::kChargeStorage,
      RequestKind::kDischargeStorage,  RequestKind::kPreCooling,
      RequestKind::kReserveRestoration};
  for (const RequestKind kind : kKinds) {
    if (name == RequestKindName(kind)) {
      return kind;
    }
  }
  return MakeError(ErrorCode::kUnsupportedValue, "request_kind",
                   "unknown request kind '" + std::string(name) + "'");
}

EffectDirection DirectionOf(RequestKind kind) noexcept {
  switch (kind) {
    case RequestKind::kShiftFlexibleLoad:
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kDeferBatchWork:
    case RequestKind::kReserveRestoration:
      return EffectDirection::kReduceLoad;
    case RequestKind::kChargeStorage:
    case RequestKind::kPreCooling:
      return EffectDirection::kIncreaseLoad;
    case RequestKind::kDischargeStorage:
      return EffectDirection::kNeutral;
  }
  return EffectDirection::kNeutral;
}

const char* RationaleName(Rationale rationale) noexcept {
  switch (rationale) {
    case Rationale::kDemandChargeAvoidance: return "demand_charge_avoidance";
    case Rationale::kPriceArbitrage: return "price_arbitrage";
    case Rationale::kEfficiencyRebalance: return "efficiency_rebalance";
    case Rationale::kReserveRestoration: return "reserve_restoration";
    case Rationale::kObligationCompliance: return "obligation_compliance";
  }
  return "unknown";
}

Result<Rationale> ParseRationale(std::string_view name) {
  constexpr Rationale kRationales[] = {
      Rationale::kDemandChargeAvoidance, Rationale::kPriceArbitrage,
      Rationale::kEfficiencyRebalance,   Rationale::kReserveRestoration,
      Rationale::kObligationCompliance};
  for (const Rationale rationale : kRationales) {
    if (name == RationaleName(rationale)) {
      return rationale;
    }
  }
  return MakeError(ErrorCode::kUnsupportedValue, "rationale",
                   "unknown rationale '" + std::string(name) + "'");
}

bool DecisionRequest::TargetsServiceClass() const noexcept {
  switch (kind) {
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kDeferBatchWork:
    case RequestKind::kPreCooling:
    case RequestKind::kShiftFlexibleLoad:
      return true;
    case RequestKind::kChargeStorage:
    case RequestKind::kDischargeStorage:
    case RequestKind::kReserveRestoration:
      return false;
  }
  return false;
}

Status ValidateRequest(const DecisionRequest& request) {
  if (request.request_id.is_unset()) {
    return MakeError(ErrorCode::kMissingRequiredField, "request", "request id is unset");
  }
  if (request.client_id.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, "request:" + request.request_id.ToString(),
                     "client id is empty");
  }
  if (request.policy_generation.is_zero()) {
    return MakeError(ErrorCode::kMissingRequiredField, "request:" + request.request_id.ToString(),
                     "policy generation is unset");
  }
  if (!request.magnitude_kw.is_positive()) {
    return MakeError(ErrorCode::kInvalidArgument, "request:" + request.request_id.ToString(),
                     "magnitude must be positive");
  }
  if (request.duration.raw() <= 0) {
    return MakeError(ErrorCode::kInvalidArgument, "request:" + request.request_id.ToString(),
                     "duration must be positive");
  }
  if (request.TargetsServiceClass() && request.service_class.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, "request:" + request.request_id.ToString(),
                     std::string("request kind '") + RequestKindName(request.kind) +
                         "' requires a service class");
  }
  if (!request.TargetsServiceClass() && !request.service_class.empty()) {
    return MakeError(ErrorCode::kInvalidArgument, "request:" + request.request_id.ToString(),
                     std::string("request kind '") + RequestKindName(request.kind) +
                         "' does not take a service class");
  }
  if (request.desired_window.duration().raw() <= 0) {
    return MakeError(ErrorCode::kInvalidArgument, "request:" + request.request_id.ToString(),
                     "desired window must span at least one second");
  }
  if (request.duration < request.desired_window.duration()) {
    return MakeError(ErrorCode::kInvalidArgument, "request:" + request.request_id.ToString(),
                     "duration is shorter than the desired window; the window would be only partly "
                     "covered by the action");
  }
  return OkStatus();
}

}  // namespace ecg
