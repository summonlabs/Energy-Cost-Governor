#include "ecg/engine.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "ecg/hash.hpp"

namespace ecg {
namespace {

constexpr const char* kSubjectRequest = "request";
constexpr const char* kSubjectWindow = "window";
constexpr const char* kSubjectTariff = "tariff";
constexpr const char* kSubjectReserve = "reserve";
constexpr const char* kSubjectCapacity = "capacity";
constexpr const char* kSubjectRisk = "risk";
constexpr const char* kSubjectIncident = "incident";
constexpr const char* kSubjectEconomics = "economics";
constexpr const char* kSubjectDemandCharge = "demand_charge";
constexpr const char* kSubjectPolicy = "policy";

[[nodiscard]] std::string EvidenceSubject(EvidenceCategory category) {
  std::string subject = "evidence.";
  subject += EvidenceCategoryName(category);
  return subject;
}

[[nodiscard]] std::string ServiceSubject(const ServiceClassId& id) {
  std::string subject = "service_class=";
  subject += id.value();
  return subject;
}

/// Evidence categories a request kind cannot be answered without. A kind absent
/// from a list is one whose question does not depend on that evidence, which is
/// how reserve restoration stays answerable when the tariff feed is down.
[[nodiscard]] std::vector<EvidenceCategory> RequiredCategories(RequestKind kind) {
  switch (kind) {
    case RequestKind::kShiftFlexibleLoad:
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kPreCooling:
      return {EvidenceCategory::kTariff,     EvidenceCategory::kReserve,
              EvidenceCategory::kCapacity,   EvidenceCategory::kServices,
              EvidenceCategory::kRisk,       EvidenceCategory::kIncident};
    case RequestKind::kDeferBatchWork:
      return {EvidenceCategory::kTariff, EvidenceCategory::kServices,
              EvidenceCategory::kRisk,   EvidenceCategory::kIncident};
    case RequestKind::kChargeStorage:
    case RequestKind::kDischargeStorage:
      return {EvidenceCategory::kTariff,     EvidenceCategory::kReserve,
              EvidenceCategory::kCapacity,   EvidenceCategory::kEfficiency,
              EvidenceCategory::kRisk,       EvidenceCategory::kIncident};
    case RequestKind::kReserveRestoration:
      return {EvidenceCategory::kReserve, EvidenceCategory::kCapacity,
              EvidenceCategory::kRisk,    EvidenceCategory::kIncident};
  }
  return {};
}

[[nodiscard]] bool RequiresCategory(RequestKind kind, EvidenceCategory category) {
  const std::vector<EvidenceCategory> categories = RequiredCategories(kind);
  return std::find(categories.begin(), categories.end(), category) != categories.end();
}

/// Direction of the action with respect to the reserve margin. Storage discharge
/// supplies power, so it improves headroom like a load reduction does.
[[nodiscard]] EffectDirection ReserveDirectionOf(RequestKind kind) noexcept {
  if (kind == RequestKind::kDischargeStorage) {
    return EffectDirection::kReduceLoad;
  }
  return DirectionOf(kind);
}

[[nodiscard]] ReasonCode CodeForFreshness(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::kFresh: return ReasonCode::kPriceCoverageVerified;  // Not reachable.
    case Freshness::kMissing: return ReasonCode::kEvidenceMissing;
    case Freshness::kStale: return ReasonCode::kEvidenceStale;
    case Freshness::kFutureDated: return ReasonCode::kEvidenceFutureDated;
    case Freshness::kRecovered: return ReasonCode::kEvidenceRecovered;
    case Freshness::kEpochMismatch: return ReasonCode::kEvidenceEpochMismatch;
    case Freshness::kUnsupportedVersion: return ReasonCode::kEvidenceUnsupportedVersion;
    case Freshness::kConflicting: return ReasonCode::kEvidenceConflicting;
  }
  return ReasonCode::kEvidenceMissing;
}

template <class Payload>
struct Slot {
  const Evidence<Payload>* evidence{nullptr};
  Freshness freshness{Freshness::kMissing};

  [[nodiscard]] bool usable() const noexcept { return evidence != nullptr && IsFresh(freshness); }
  [[nodiscard]] const Payload& value() const noexcept { return evidence->value; }
  [[nodiscard]] EvidenceRef ref() const { return MakeEvidenceRef(*evidence, freshness); }
};

struct WindowCost {
  MoneyMicros cost;
  PriceMicrosPerKwh average_price;
  std::uint32_t slices{0};
};

class Evaluation {
 public:
  Evaluation(const PolicySet& policy, const DecisionRequest& request, const EvidenceSet& evidence,
             UtcInstant now)
      : policy_(policy), request_(request), evidence_(evidence), now_(now) {}

  [[nodiscard]] Result<Decision> Run();

 private:
  // -- slots -------------------------------------------------------------
  void InitSlots();
  template <class Payload>
  [[nodiscard]] Slot<Payload> MakeSlot(const std::optional<Evidence<Payload>>& source,
                                       EvidenceCategory category) const;
  void CheckSlot(const Slot<Tariff>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<DemandObservation>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<ReserveState>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<EfficiencyState>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<ServiceCatalog>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<CapacityState>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<RiskState>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<IncidentState>& slot, EvidenceCategory category);
  void CheckSlot(const Slot<ZoneRules>& slot, EvidenceCategory category);

  // -- reasons -----------------------------------------------------------
  void Add(ReasonCode code, std::string subject, std::string detail);
  void Add(ReasonCode code, std::string subject, std::string detail, const EvidenceRef& evidence);
  void DeferUntil(UtcInstant instant);

  // -- rules -------------------------------------------------------------
  void RuleRequestContract();
  void RulePolicyGeneration();
  void RuleEvidence();
  void RuleWindowCoverage();
  void RuleServiceTerms();
  void RuleReserveFloor();
  void RuleCapacity();
  void RuleRisk();
  void RuleIncident();
  void RuleDemandCharge();
  void RuleEconomics();
  void RuleIntentBounding();
  void Finalize();

  // -- helpers -----------------------------------------------------------
  [[nodiscard]] bool TariffRequired() const {
    return RequiresCategory(request_.kind, EvidenceCategory::kTariff);
  }
  [[nodiscard]] bool DemandAnalysisApplies() const;
  [[nodiscard]] Result<WindowCost> CostOfWindow(const Tariff& tariff, const Interval& window,
                                                PowerKw power) const;
  [[nodiscard]] Result<std::optional<Interval>> FindBestTargetWindow(const Tariff& tariff,
                                                                     UtcInstant not_before,
                                                                     DurationSec duration,
                                                                     bool cheapest) const;
  [[nodiscard]] std::optional<BoundedIntent> BuildIntent() const;
  [[nodiscard]] std::vector<EvidenceRef> BuildEvidenceDependencies() const;

  const PolicySet& policy_;
  const DecisionRequest& request_;
  const EvidenceSet& evidence_;
  UtcInstant now_;

  Decision decision_;
  std::vector<Reason> reasons_;

  Slot<Tariff> tariff_;
  Slot<DemandObservation> demand_;
  Slot<ReserveState> reserve_;
  Slot<EfficiencyState> efficiency_;
  Slot<ServiceCatalog> services_;
  Slot<CapacityState> capacity_;
  Slot<RiskState> risk_;
  Slot<IncidentState> incident_;
  Slot<ZoneRules> zone_;

  PowerKw bounded_magnitude_;
  DurationSec bounded_duration_;
  MoneyMicros demand_charge_avoided_;
  bool price_coverage_ok_{false};
  bool economics_computed_{false};
};

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

template <class Payload>
Slot<Payload> Evaluation::MakeSlot(const std::optional<Evidence<Payload>>& source,
                                   EvidenceCategory category) const {
  Slot<Payload> slot;
  if (!source.has_value()) {
    slot.freshness = Freshness::kMissing;
    return slot;
  }
  slot.evidence = &*source;
  slot.freshness = ClassifyFreshness(source->meta, policy_.MaxAgeFor(category),
                                     policy_.limits.future_skew, now_, policy_.epoch);
  return slot;
}

void Evaluation::InitSlots() {
  tariff_ = MakeSlot(evidence_.tariff, EvidenceCategory::kTariff);
  demand_ = MakeSlot(evidence_.demand, EvidenceCategory::kDemand);
  reserve_ = MakeSlot(evidence_.reserve, EvidenceCategory::kReserve);
  efficiency_ = MakeSlot(evidence_.efficiency, EvidenceCategory::kEfficiency);
  services_ = MakeSlot(evidence_.services, EvidenceCategory::kServices);
  capacity_ = MakeSlot(evidence_.capacity, EvidenceCategory::kCapacity);
  risk_ = MakeSlot(evidence_.risk, EvidenceCategory::kRisk);
  incident_ = MakeSlot(evidence_.incident, EvidenceCategory::kIncident);
  zone_ = MakeSlot(evidence_.zone, EvidenceCategory::kZone);

  // Payload-level validity is a freshness downgrade, not a separate state: an
  // internally contradictory payload is not usable evidence.
  // A schedule that contradicts itself is not usable evidence, whatever its age.
  if (tariff_.usable() && !ValidateTariff(tariff_.value()).ok()) {
    tariff_.freshness = Freshness::kConflicting;
  }
}

void Evaluation::CheckSlot(const Slot<Tariff>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category),
        std::string(EvidenceCategoryName(category)) + " evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string(EvidenceCategoryName(category)) + " evidence from source '" +
          slot.evidence->meta.source.ToString() + "' is " + FreshnessName(slot.freshness) +
          " (observed " + FormatRfc3339(slot.evidence->meta.observed_at) + ", provenance " +
          ProvenanceName(slot.evidence->meta.provenance) + ")",
      slot.ref());
}

void Evaluation::CheckSlot(const Slot<DemandObservation>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "demand evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("demand evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<ReserveState>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "reserve evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("reserve evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<EfficiencyState>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "efficiency evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("efficiency evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<ServiceCatalog>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category),
        "service catalog evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("service catalog evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<CapacityState>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "capacity evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("capacity evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<RiskState>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "risk evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("risk evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<IncidentState>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kEvidenceMissing, EvidenceSubject(category), "incident evidence was not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("incident evidence is ") + FreshnessName(slot.freshness), slot.ref());
}

void Evaluation::CheckSlot(const Slot<ZoneRules>& slot, EvidenceCategory category) {
  if (slot.usable()) {
    return;
  }
  if (slot.evidence == nullptr) {
    Add(ReasonCode::kZoneRulesMissing, EvidenceSubject(category),
        "time-zone rules are required to resolve a wall-clock demand window and were not supplied");
    return;
  }
  Add(CodeForFreshness(slot.freshness), EvidenceSubject(category),
      std::string("time-zone rules are ") + FreshnessName(slot.freshness), slot.ref());
}

// ---------------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------------

void Evaluation::Add(ReasonCode code, std::string subject, std::string detail) {
  Reason reason;
  reason.code = code;
  reason.severity = DefaultSeverityFor(code);
  reason.subject = std::move(subject);
  reason.detail = std::move(detail);
  reasons_.push_back(std::move(reason));
}

void Evaluation::Add(ReasonCode code, std::string subject, std::string detail,
                     const EvidenceRef& evidence) {
  Reason reason;
  reason.code = code;
  reason.severity = DefaultSeverityFor(code);
  reason.subject = std::move(subject);
  reason.detail = std::move(detail);
  reason.evidence = evidence;
  reasons_.push_back(std::move(reason));
}

void Evaluation::DeferUntil(UtcInstant instant) {
  if (!decision_.reconsider_at.has_value() || instant < *decision_.reconsider_at) {
    decision_.reconsider_at = instant;
  }
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

void Evaluation::RuleRequestContract() {
  bounded_magnitude_ = request_.magnitude_kw;
  bounded_duration_ = request_.duration;

  if (policy_.max_intent_magnitude_kw < bounded_magnitude_) {
    bounded_magnitude_ = policy_.max_intent_magnitude_kw;
    Add(ReasonCode::kIntentBounded, kSubjectRequest,
        "requested magnitude " + FormatQuantity(request_.magnitude_kw) +
            " kW exceeds the policy bound; the intent is bounded to " +
            FormatQuantity(policy_.max_intent_magnitude_kw) + " kW");
  }
  if (policy_.max_intent_duration < bounded_duration_) {
    Add(ReasonCode::kIntentDurationExceedsPolicy, kSubjectRequest,
        "requested duration " + FormatQuantity(request_.duration) +
            " s exceeds the policy bound of " + FormatQuantity(policy_.max_intent_duration) +
            " s; a shortened action is not the action that was asked for");
  }
}

void Evaluation::RulePolicyGeneration() {
  if (!(request_.policy_generation == policy_.generation)) {
    Add(ReasonCode::kPolicyGenerationStale, kSubjectPolicy,
        "request was formed against policy generation " + request_.policy_generation.ToString() +
            " but the engine holds generation " + policy_.generation.ToString());
    return;
  }
  Add(ReasonCode::kPolicyGenerationCurrent, kSubjectPolicy,
      "request and engine agree on policy generation " + policy_.generation.ToString());
}

void Evaluation::RuleEvidence() {
  if (!(request_.epoch == policy_.epoch)) {
    Add(ReasonCode::kRequestEpochMismatch, kSubjectPolicy,
        "request epoch " + request_.epoch.ToString() + " differs from the policy epoch " +
            policy_.epoch.ToString());
  }
  for (const EvidenceCategory category : RequiredCategories(request_.kind)) {
    switch (category) {
      case EvidenceCategory::kTariff: CheckSlot(tariff_, category); break;
      case EvidenceCategory::kDemand: CheckSlot(demand_, category); break;
      case EvidenceCategory::kReserve: CheckSlot(reserve_, category); break;
      case EvidenceCategory::kEfficiency: CheckSlot(efficiency_, category); break;
      case EvidenceCategory::kServices: CheckSlot(services_, category); break;
      case EvidenceCategory::kCapacity: CheckSlot(capacity_, category); break;
      case EvidenceCategory::kRisk: CheckSlot(risk_, category); break;
      case EvidenceCategory::kIncident: CheckSlot(incident_, category); break;
      case EvidenceCategory::kZone: CheckSlot(zone_, category); break;
    }
  }
}

void Evaluation::RuleWindowCoverage() {
  if (!(now_ < request_.desired_window.end())) {
    Add(ReasonCode::kWindowInPast, kSubjectWindow,
        "the desired window ended at " + FormatRfc3339(request_.desired_window.end()) +
            ", which is not after now (" + FormatRfc3339(now_) + ")");
    return;
  }

  const auto horizon_end = AddSeconds(now_, policy_.decision_horizon);
  if (horizon_end.ok() && horizon_end.value() < request_.desired_window.begin()) {
    const auto reconsider = SubSeconds(request_.desired_window.begin(), policy_.decision_horizon);
    if (reconsider.ok()) {
      DeferUntil(reconsider.value());
    }
    Add(ReasonCode::kWindowBeyondHorizon, kSubjectWindow,
        "the desired window begins at " + FormatRfc3339(request_.desired_window.begin()) +
            ", beyond the decision horizon ending at " + FormatRfc3339(horizon_end.value()));
    return;
  }

  if (!TariffRequired() || !tariff_.usable()) {
    return;
  }
  const Tariff& tariff = tariff_.value();
  if (!tariff.validity.contains(request_.desired_window)) {
    Add(ReasonCode::kTariffValidityMiss, kSubjectTariff,
        "the tariff is valid from " + FormatRfc3339(tariff.validity.begin()) + " to " +
            FormatRfc3339(tariff.validity.end()) + " and does not cover the desired window",
        tariff_.ref());
    return;
  }

  const auto slices = PriceSlicesForWindow(tariff, request_.desired_window);
  if (!slices.ok()) {
    switch (slices.error().code()) {
      case ErrorCode::kStateConflict:
        Add(ReasonCode::kPriceCoverageAmbiguous, kSubjectTariff, slices.error().message(),
            tariff_.ref());
        return;
      case ErrorCode::kInternalInvariantViolation:
        Add(ReasonCode::kEvidenceConflicting, kSubjectTariff, slices.error().message(), tariff_.ref());
        return;
      default:
        Add(ReasonCode::kPriceCoverageGap, kSubjectTariff, slices.error().message(), tariff_.ref());
        return;
    }
  }
  if (slices.value().size() > policy_.max_price_slices) {
    Add(ReasonCode::kResourceLimitExceeded, kSubjectTariff,
        "the desired window spans " + std::to_string(slices.value().size()) +
            " price slices, above the policy bound of " + std::to_string(policy_.max_price_slices));
    return;
  }
  price_coverage_ok_ = true;
  Add(ReasonCode::kPriceCoverageVerified, kSubjectTariff,
      "the price schedule covers the desired window in " + std::to_string(slices.value().size()) +
          " slice(s)",
      tariff_.ref());
}

void Evaluation::RuleServiceTerms() {
  if (!request_.TargetsServiceClass() || !services_.usable()) {
    return;
  }
  const std::string subject = ServiceSubject(request_.service_class);
  const ServiceClassTerms* terms = services_.value().Find(request_.service_class);
  if (terms == nullptr) {
    Add(ReasonCode::kServiceClassUnknown, subject,
        "the current service catalog does not define this class; this runtime does not invent "
        "service terms for classes it has not been told about",
        services_.ref());
    return;
  }

  bool satisfied = true;
  if (DirectionOf(request_.kind) == EffectDirection::kReduceLoad) {
    switch (terms->permission) {
      case CurtailmentPermission::kProhibited:
        satisfied = false;
        Add(ReasonCode::kServiceCurtailmentProhibited, subject,
            "service terms prohibit curtailment of this class", services_.ref());
        break;
      case CurtailmentPermission::kWithNotice: {
        const auto notice = ElapsedBetween(now_, request_.desired_window.begin());
        if (!notice.ok() || notice.value() < terms->minimum_notice) {
          satisfied = false;
          Add(ReasonCode::kServiceNoticeInsufficient, subject,
              "the class requires " + FormatQuantity(terms->minimum_notice) +
                  " s of notice; the desired window gives " +
                  (notice.ok() ? FormatQuantity(notice.value()) : std::string("no")) + " s",
              services_.ref());
        }
        break;
      }
      case CurtailmentPermission::kUnrestricted:
        break;
    }

    if (terms->max_curtailment_duration < request_.duration) {
      satisfied = false;
      Add(ReasonCode::kServiceDurationExceeded, subject,
          "the class may be curtailed for at most " + FormatQuantity(terms->max_curtailment_duration) +
              " s, but the request asks for " + FormatQuantity(request_.duration) + " s",
          services_.ref());
    }

    if (terms->nominal_load_kw.is_positive()) {
      const auto ratio = CheckedMulDiv(bounded_magnitude_.raw(), 1000000,
                                       terms->nominal_load_kw.raw());
      if (!ratio.ok()) {
        satisfied = false;
        Add(ReasonCode::kEconomicsOverflow, subject,
            "the curtailment ratio could not be computed exactly: " + ratio.error().message());
      } else {
        const std::int64_t limit = std::min(terms->max_curtailment_ratio.raw(),
                                            policy_.max_service_curtailment_ratio.raw());
        if (ratio.value() > limit) {
          satisfied = false;
          Add(ReasonCode::kServiceRatioExceeded, subject,
              "curtailing " + FormatQuantity(bounded_magnitude_) + " kW is " +
                  FormatQuantity(RatioPpm::FromRaw(ratio.value())) +
                  " of nominal load, above the permitted " + FormatQuantity(RatioPpm::FromRaw(limit)),
              services_.ref());
        }
      }
    }
  }

  if (satisfied) {
    Add(ReasonCode::kServiceTermsSatisfied, subject,
        "service terms for this class permit the requested action", services_.ref());
  }
}

void Evaluation::RuleReserveFloor() {
  if (!reserve_.usable()) {
    return;
  }
  const ReserveState& reserve = reserve_.value();
  const auto floor = EffectiveReserveFloor(reserve);
  if (!floor.ok()) {
    Add(ReasonCode::kEvidenceInvalid, kSubjectReserve, floor.error().message(), reserve_.ref());
    return;
  }
  const auto margin =
      CheckedMulDiv(reserve.capacity_kw.raw(), policy_.reserve_safety_margin.raw(), 1000000);
  if (!margin.ok()) {
    Add(ReasonCode::kResourceLimitExceeded, kSubjectReserve,
        "the reserve margin could not be computed exactly: " + margin.error().message(),
        reserve_.ref());
    return;
  }
  const auto required = CheckedAdd(floor.value(), PowerKw::FromRaw(margin.value()));
  if (!required.ok()) {
    Add(ReasonCode::kResourceLimitExceeded, kSubjectReserve,
        "the required reserve could not be computed exactly: " + required.error().message(),
        reserve_.ref());
    return;
  }

  const EffectDirection direction = ReserveDirectionOf(request_.kind);
  const auto delta = direction == EffectDirection::kIncreaseLoad
                         ? CheckedNegate(bounded_magnitude_)
                         : CheckedAdd(PowerKw::FromRaw(0), bounded_magnitude_);
  if (!delta.ok()) {
    Add(ReasonCode::kResourceLimitExceeded, kSubjectReserve,
        "the reserve delta could not be computed exactly: " + delta.error().message(),
        reserve_.ref());
    return;
  }
  const auto after = CheckedAdd(reserve.current_reserve_kw, delta.value());
  if (!after.ok()) {
    Add(ReasonCode::kResourceLimitExceeded, kSubjectReserve,
        "the post-action reserve could not be computed exactly: " + after.error().message(),
        reserve_.ref());
    return;
  }

  if (after.value() < required.value()) {
    Add(ReasonCode::kReserveFloorBreach, kSubjectReserve,
        "reserve after the action would be " + FormatQuantity(after.value()) +
            " kW, below the required " + FormatQuantity(required.value()) + " kW (floor " +
            FormatQuantity(floor.value()) + " kW plus a policy margin of " +
            FormatQuantity(PowerKw::FromRaw(margin.value())) + " kW)",
        reserve_.ref());
    return;
  }
  Add(ReasonCode::kReserveHeadroomSufficient, kSubjectReserve,
      "reserve after the action would be " + FormatQuantity(after.value()) +
          " kW, at or above the required " + FormatQuantity(required.value()) + " kW",
      reserve_.ref());
}

void Evaluation::RuleCapacity() {
  if (!capacity_.usable()) {
    return;
  }
  const CapacityState& capacity = capacity_.value();
  switch (request_.kind) {
    case RequestKind::kChargeStorage: {
      if (bounded_magnitude_ > capacity.storage_charge_kw) {
        Add(ReasonCode::kStoragePowerInsufficient, kSubjectCapacity,
            "charging at " + FormatQuantity(bounded_magnitude_) +
                " kW exceeds the published charge power of " +
                FormatQuantity(capacity.storage_charge_kw) + " kW",
            capacity_.ref());
        return;
      }
      const auto energy = CheckedMulDiv(bounded_magnitude_.raw(), bounded_duration_.raw(), 3600);
      if (!energy.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectCapacity,
            "the storage energy could not be computed exactly: " + energy.error().message());
        return;
      }
      if (EnergyKwh::FromRaw(energy.value()) > capacity.storage_usable_energy_kwh) {
        Add(ReasonCode::kStorageEnergyInsufficient, kSubjectCapacity,
            "the action would store " + FormatQuantity(EnergyKwh::FromRaw(energy.value())) +
                " kWh, above the usable " + FormatQuantity(capacity.storage_usable_energy_kwh) +
                " kWh",
            capacity_.ref());
        return;
      }
      break;
    }
    case RequestKind::kDischargeStorage: {
      if (bounded_magnitude_ > capacity.storage_discharge_kw) {
        Add(ReasonCode::kStoragePowerInsufficient, kSubjectCapacity,
            "discharging at " + FormatQuantity(bounded_magnitude_) +
                " kW exceeds the published discharge power of " +
                FormatQuantity(capacity.storage_discharge_kw) + " kW",
            capacity_.ref());
        return;
      }
      break;
    }
    default: {
      const EffectDirection direction = DirectionOf(request_.kind);
      if (direction == EffectDirection::kReduceLoad) {
        if (bounded_magnitude_ > capacity.flexible_load_kw) {
          Add(ReasonCode::kCapacityInsufficient, kSubjectCapacity,
              "the action needs " + FormatQuantity(bounded_magnitude_) +
                  " kW of flexible load but only " + FormatQuantity(capacity.flexible_load_kw) +
                  " kW is published",
              capacity_.ref());
          return;
        }
      } else if (direction == EffectDirection::kIncreaseLoad) {
        if (bounded_magnitude_ > capacity.shiftable_headroom_kw) {
          Add(ReasonCode::kCapacityInsufficient, kSubjectCapacity,
              "the action needs " + FormatQuantity(bounded_magnitude_) +
                  " kW of headroom but only " + FormatQuantity(capacity.shiftable_headroom_kw) +
                  " kW is published",
              capacity_.ref());
          return;
        }
      }
      break;
    }
  }
}

void Evaluation::RuleRisk() {
  if (!risk_.usable()) {
    return;
  }
  const RiskState& risk = risk_.value();
  const EffectDirection direction = DirectionOf(request_.kind);
  const bool essential =
      request_.kind == RequestKind::kReserveRestoration || request_.kind == RequestKind::kDischargeStorage;

  switch (risk.posture) {
    case RiskPosture::kRestricted:
      if (!essential) {
        Add(ReasonCode::kRiskPostureRestricted, kSubjectRisk,
            "the risk authority reports a restricted posture; only reserve restoration and storage "
            "discharge may proceed",
            risk_.ref());
        return;
      }
      break;
    case RiskPosture::kElevated:
      if (direction == EffectDirection::kIncreaseLoad) {
        Add(ReasonCode::kRiskPostureRestricted, kSubjectRisk,
            "the risk authority reports an elevated posture; actions that raise load are refused",
            risk_.ref());
        return;
      }
      break;
    case RiskPosture::kNormal:
      break;
  }

  if (risk.max_ramp_kw_per_min.has_value()) {
    const auto rate = CheckedMulDiv(bounded_magnitude_.raw(), 60, request_.duration.raw());
    if (!rate.ok()) {
      Add(ReasonCode::kEconomicsOverflow, kSubjectRisk,
          "the ramp rate could not be computed exactly: " + rate.error().message());
      return;
    }
    const auto published = risk.max_ramp_kw_per_min->raw();
    if (rate.value() > published) {
      Add(ReasonCode::kRampLimitExceeded, kSubjectRisk,
          "the action ramps at " + FormatQuantity(PowerKwPerMin::FromRaw(rate.value())) +
              " kW/min, above the published limit of " +
              FormatQuantity(PowerKwPerMin::FromRaw(published)) + " kW/min",
          risk_.ref());
      return;
    }
  }

  if (risk.minimum_dwell.has_value() && risk.minimum_dwell->raw() > 0 &&
      !risk.last_change_at.is_zero()) {
    const auto since = ElapsedBetween(risk.last_change_at, now_);
    if (!since.ok() || since.value() < *risk.minimum_dwell) {
      Add(ReasonCode::kDwellNotElapsed, kSubjectRisk,
          "the last change affecting this subject was " +
              (since.ok() ? FormatQuantity(since.value()) : std::string("before the representable range")) +
              " s ago, inside the published dwell of " +
              FormatQuantity(*risk.minimum_dwell) + " s",
          risk_.ref());
      return;
    }
  }

  if (request_.kind == RequestKind::kDeferBatchWork && risk.max_deferral.has_value() &&
      *risk.max_deferral < request_.duration) {
    Add(ReasonCode::kDeferralLimitExceeded, kSubjectRisk,
        "the request would defer work for " + FormatQuantity(request_.duration) +
            " s, above the published deferral limit of " + FormatQuantity(*risk.max_deferral) + " s",
        risk_.ref());
    return;
  }
}

void Evaluation::RuleIncident() {
  if (!incident_.usable()) {
    return;
  }
  const IncidentState& incident = incident_.value();
  const bool affected = request_.TargetsServiceClass() && incident.Affects(request_.service_class);
  const EffectDirection direction = DirectionOf(request_.kind);
  const bool essential =
      request_.kind == RequestKind::kReserveRestoration || request_.kind == RequestKind::kDischargeStorage;

  switch (incident.severity) {
    case IncidentSeverity::kCritical:
      if (!essential) {
        Add(ReasonCode::kIncidentCritical, kSubjectIncident,
            "the incident authority reports a critical incident; only reserve restoration and "
            "storage discharge may proceed",
            incident_.ref());
      }
      break;
    case IncidentSeverity::kMajor:
      if (affected) {
        Add(ReasonCode::kIncidentCritical, kSubjectIncident,
            "the target service class is affected by a major incident", incident_.ref());
      } else if (!essential) {
        const auto reconsider = AddSeconds(now_, policy_.incident_recheck_interval);
        if (reconsider.ok()) {
          DeferUntil(reconsider.value());
        }
        Add(ReasonCode::kIncidentActive, kSubjectIncident,
            "a major incident is active; energy-cost actions are reconsidered when it clears",
            incident_.ref());
      }
      break;
    case IncidentSeverity::kAdvisory:
      if (affected || direction == EffectDirection::kIncreaseLoad) {
        const auto reconsider = AddSeconds(now_, policy_.incident_recheck_interval);
        if (reconsider.ok()) {
          DeferUntil(reconsider.value());
        }
        Add(ReasonCode::kIncidentActive, kSubjectIncident,
            "an advisory incident is active and this action would raise load or touch an affected "
            "class",
            incident_.ref());
      }
      break;
    case IncidentSeverity::kNone:
      Add(ReasonCode::kIncidentClear, kSubjectIncident, "the incident authority reports no incident",
          incident_.ref());
      break;
  }
}

bool Evaluation::DemandAnalysisApplies() const {
  if (!tariff_.usable() || tariff_.value().demand_charges.empty()) {
    return false;
  }
  if (request_.rationale == Rationale::kDemandChargeAvoidance) {
    return true;
  }
  return DirectionOf(request_.kind) == EffectDirection::kIncreaseLoad &&
         policy_.prohibit_demand_charge_increase;
}

void Evaluation::RuleDemandCharge() {
  if (!DemandAnalysisApplies()) {
    return;
  }
  if (!zone_.usable()) {
    CheckSlot(zone_, EvidenceCategory::kZone);
    return;
  }
  if (!demand_.usable()) {
    CheckSlot(demand_, EvidenceCategory::kDemand);
    return;
  }

  const Tariff& tariff = tariff_.value();
  const DemandObservation& observed = demand_.value();
  const EffectDirection direction = DirectionOf(request_.kind);
  const PowerKw baseline_peak = observed.rolling_peak_kw < observed.current_demand_kw
                                    ? observed.current_demand_kw
                                    : observed.rolling_peak_kw;

  PowerKw projected_with = baseline_peak;
  if (direction == EffectDirection::kReduceLoad) {
    const auto reduced = CheckedSub(observed.current_demand_kw, bounded_magnitude_);
    if (!reduced.ok()) {
      Add(ReasonCode::kEconomicsOverflow, kSubjectDemandCharge, reduced.error().message());
      return;
    }
    projected_with = observed.rolling_peak_kw < reduced.value() ? reduced.value()
                                                               : observed.rolling_peak_kw;
  } else if (direction == EffectDirection::kIncreaseLoad) {
    const auto increased = CheckedAdd(observed.current_demand_kw, bounded_magnitude_);
    if (!increased.ok()) {
      Add(ReasonCode::kEconomicsOverflow, kSubjectDemandCharge, increased.error().message());
      return;
    }
    projected_with = observed.rolling_peak_kw < increased.value() ? increased.value()
                                                                  : observed.rolling_peak_kw;
  }

  bool matched_rule = false;
  for (const DemandChargeRule& rule : tariff.demand_charges) {
    const auto search_begin = SubSeconds(request_.desired_window.begin(), rule.averaging_interval);
    if (!search_begin.ok()) {
      continue;
    }
    const auto search = Interval::Make(search_begin.value(), request_.desired_window.end());
    if (!search.ok()) {
      continue;
    }
    const auto clipped = search.value().Intersect(rule.validity);
    if (!clipped.ok() || !clipped.value().has_value()) {
      continue;
    }
    const auto occurrences = ExpandRecurring(rule.recurrence, zone_.value(), *clipped.value());
    if (!occurrences.ok()) {
      Add(ReasonCode::kDemandWindowUnresolved, kSubjectDemandCharge,
          "the demand window '" + rule.id.ToString() +
              "' could not be expanded: " + occurrences.error().message(),
          zone_.ref());
      return;
    }
    bool overlaps = false;
    for (const Interval& occurrence : occurrences.value()) {
      if (occurrence.overlaps(request_.desired_window)) {
        overlaps = true;
        break;
      }
    }
    if (!overlaps) {
      continue;
    }
    matched_rule = true;

    // Billed peak above the threshold, with and without the action.
    const std::int64_t without_delta = baseline_peak.raw() - rule.threshold_kw.raw();
    const std::int64_t with_delta = projected_with.raw() - rule.threshold_kw.raw();
    const std::int64_t billed_without = without_delta > 0 ? without_delta : 0;
    const std::int64_t billed_with = with_delta > 0 ? with_delta : 0;

    if (direction == EffectDirection::kIncreaseLoad) {
      if (policy_.prohibit_demand_charge_increase && billed_with > billed_without) {
        Add(ReasonCode::kDemandChargeIncrease, kSubjectDemandCharge,
            "the action would raise the metered peak inside demand window '" + rule.id.ToString() +
                "' to " + FormatQuantity(projected_with) + " kW, above the " +
                FormatQuantity(rule.threshold_kw) + " kW threshold, incurring " +
                FormatQuantity(MoneyMicros::FromRaw((billed_with - billed_without) *
                                                    rule.charge_per_kw.raw())) +
                " in demand charges",
            tariff_.ref());
        return;
      }
      continue;
    }

    const std::int64_t avoided_kw = billed_without - billed_with;
    if (avoided_kw > 0) {
      const auto avoided = CheckedMulDiv(avoided_kw, rule.charge_per_kw.raw(), 1);
      if (!avoided.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectDemandCharge,
            "the avoided demand charge could not be computed exactly: " + avoided.error().message());
        return;
      }
      const auto total = CheckedAdd(demand_charge_avoided_, MoneyMicros::FromRaw(avoided.value()));
      if (!total.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectDemandCharge,
            "the avoided demand charge overflowed: " + total.error().message());
        return;
      }
      demand_charge_avoided_ = total.value();
      Add(ReasonCode::kDemandChargeAvoided, kSubjectDemandCharge,
          "the action avoids " + FormatQuantity(PowerKw::FromRaw(avoided_kw)) +
              " kW of billed peak in demand window '" + rule.id.ToString() + "', worth " +
              FormatQuantity(MoneyMicros::FromRaw(avoided.value())),
          tariff_.ref());
    }
  }

  if (!matched_rule) {
    // An action outside every demand-charge window cannot raise a billed peak,
    // so there is nothing to prohibit. Only a request whose stated rationale is
    // demand-charge avoidance is unanswerable here, because its justification
    // cannot be established from the tariff at all.
    if (request_.rationale == Rationale::kDemandChargeAvoidance) {
      Add(ReasonCode::kDemandWindowUnresolved, kSubjectDemandCharge,
          "the request justifies itself by demand-charge avoidance, but no demand-charge window "
          "covers the desired window",
          tariff_.ref());
    }
    return;
  }

  if (request_.rationale == Rationale::kDemandChargeAvoidance &&
      DirectionOf(request_.kind) == EffectDirection::kReduceLoad && policy_.defer_early_demand_requests) {
    const auto latest_useful = SubSeconds(request_.desired_window.begin(), policy_.reaction_lead_time);
    if (latest_useful.ok() && now_ < latest_useful.value()) {
      DeferUntil(latest_useful.value());
      Add(ReasonCode::kDemandThresholdNotProjected, kSubjectDemandCharge,
          "a demand-charge-driven decision is only meaningful within the reaction lead time of " +
              FormatQuantity(policy_.reaction_lead_time) + " s before the window; reconsider at " +
              FormatRfc3339(latest_useful.value()),
          demand_.ref());
    }
  }
}

Result<WindowCost> Evaluation::CostOfWindow(const Tariff& tariff, const Interval& window,
                                            PowerKw power) const {
  const auto slices = PriceSlicesForWindow(tariff, window);
  if (!slices.ok()) {
    return slices.error();
  }
  WindowCost result;
  std::int64_t total_cost = 0;
  std::int64_t total_energy = 0;
  for (const PriceSlice& slice : slices.value()) {
    const std::int64_t seconds = slice.window.duration().raw();
    if (seconds <= 0) {
      continue;
    }
    ECG_TRY(energy_raw, CheckedMulDiv(power.raw(), seconds, 3600));
    const auto energy = QuantityFromRaw<EnergyKwhTag>(energy_raw);
    if (!energy.ok()) {
      return energy.error();
    }
    ECG_TRY(cost_raw, CheckedMulDiv(slice.price.raw(), energy.value().raw(), 1000));
    const auto cost = QuantityFromRaw<MoneyTag>(cost_raw);
    if (!cost.ok()) {
      return cost.error();
    }
    ECG_TRY(sum, CheckedAdd(MoneyMicros::FromRaw(total_cost), cost.value()));
    total_cost = sum.raw();
    ECG_TRY(energy_sum, CheckedAdd(EnergyKwh::FromRaw(total_energy), energy.value()));
    total_energy = energy_sum.raw();
  }
  result.cost = MoneyMicros::FromRaw(total_cost);
  result.slices = static_cast<std::uint32_t>(slices.value().size());
  if (total_energy > 0) {
    ECG_TRY(average_raw, CheckedMulDiv(total_cost, 1000, total_energy));
    const auto average = QuantityFromRaw<PriceTag>(average_raw);
    if (!average.ok()) {
      return average.error();
    }
    result.average_price = average.value();
  }
  return result;
}

Result<std::optional<Interval>> Evaluation::FindBestTargetWindow(const Tariff& tariff,
                                                                 UtcInstant not_before,
                                                                 DurationSec duration,
                                                                 bool cheapest) const {
  const auto search_end = AddSeconds(not_before, policy_.shift_search_horizon);
  if (!search_end.ok()) {
    return search_end.error();
  }
  const auto horizon_end = AddSeconds(now_, policy_.decision_horizon);
  if (!horizon_end.ok()) {
    return horizon_end.error();
  }
  const UtcInstant end = search_end.value() < horizon_end.value() ? search_end.value() : horizon_end.value();
  if (!(not_before < end)) {
    return std::optional<Interval>{};
  }
  const auto range = Interval::Make(not_before, end);
  if (!range.ok()) {
    return std::optional<Interval>{};
  }
  const std::vector<PriceSlice> slices = CoveredPriceSlices(tariff, range.value());
  const PriceSlice* best = nullptr;
  for (const PriceSlice& slice : slices) {
    if (best == nullptr) {
      best = &slice;
      continue;
    }
    if (cheapest ? slice.price < best->price : best->price < slice.price) {
      best = &slice;
    }
  }
  if (best == nullptr) {
    return std::optional<Interval>{};
  }
  const auto target_end = AddSeconds(best->window.begin(), duration);
  if (!target_end.ok()) {
    return target_end.error();
  }
  const UtcInstant clipped_end = target_end.value() < best->window.end() ? target_end.value()
                                                                        : best->window.end();
  if (!(best->window.begin() < clipped_end)) {
    return std::optional<Interval>{};
  }
  const auto target = Interval::Make(best->window.begin(), clipped_end);
  if (!target.ok()) {
    return std::optional<Interval>{};
  }
  return std::optional<Interval>{target.value()};
}

void Evaluation::RuleEconomics() {
  if (!TariffRequired() || !tariff_.usable() || !price_coverage_ok_) {
    return;
  }
  const Tariff& tariff = tariff_.value();

  const auto baseline = CostOfWindow(tariff, request_.desired_window, bounded_magnitude_);
  if (!baseline.ok()) {
    Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
        "the baseline cost could not be computed exactly: " + baseline.error().message(),
        tariff_.ref());
    return;
  }
  economics_computed_ = true;
  EconomicAssessment& economics = decision_.economics;
  economics.evaluated = true;
  economics.baseline_cost = baseline.value().cost;
  economics.baseline_price = baseline.value().average_price;
  economics.price_slices = baseline.value().slices;
  economics.candidate_cost = baseline.value().cost;

  switch (request_.kind) {
    case RequestKind::kCurtailServiceClass:
    case RequestKind::kReserveRestoration: {
      economics.candidate_cost = MoneyMicros::FromRaw(0);
      economics.candidate_price = PriceMicrosPerKwh::FromRaw(0);
      break;
    }
    case RequestKind::kChargeStorage: {
      if (!efficiency_.usable() || !efficiency_.value().storage_round_trip_ratio.has_value()) {
        if (efficiency_.evidence != nullptr) {
          Add(ReasonCode::kStorageEfficiencyMissing, kSubjectEconomics,
              "storage economics cannot be evaluated without a published round-trip efficiency",
              efficiency_.ref());
        } else {
          Add(ReasonCode::kStorageEfficiencyMissing, kSubjectEconomics,
              "storage economics cannot be evaluated without a published round-trip efficiency");
        }
        return;
      }
      const RatioPpm round_trip = *efficiency_.value().storage_round_trip_ratio;
      const auto target = FindBestTargetWindow(tariff, request_.desired_window.end(),
                                               bounded_duration_, false);
      if (!target.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the discharge window could not be located: " + target.error().message(), tariff_.ref());
        return;
      }
      if (!target.value().has_value()) {
        break;  // No discharge window inside the horizon: savings cannot accrue.
      }
      const Interval discharge = *target.value();
      const auto displaced = CostOfWindow(tariff, discharge, bounded_magnitude_);
      if (!displaced.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the displaced cost could not be computed exactly: " + displaced.error().message(),
            tariff_.ref());
        return;
      }
      // Drawing more energy than is delivered, by exactly the round-trip loss.
      const auto drawn = CheckedScale(bounded_magnitude_, 1000000, round_trip.raw());
      if (!drawn.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the round-trip correction could not be applied exactly: " + drawn.error().message());
        return;
      }
      const auto charged = CostOfWindow(tariff, request_.desired_window, drawn.value());
      if (!charged.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the charging cost could not be computed exactly: " + charged.error().message(),
            tariff_.ref());
        return;
      }
      economics.shift_target = discharge;
      economics.baseline_cost = displaced.value().cost;
      economics.baseline_price = displaced.value().average_price;
      economics.candidate_cost = charged.value().cost;
      economics.candidate_price = charged.value().average_price;
      break;
    }
    case RequestKind::kDischargeStorage: {
      // The stored energy was purchased earlier; discharging avoids buying at the
      // current price. Prior purchase cost is sunk and is not modelled as a saving.
      economics.candidate_cost = MoneyMicros::FromRaw(0);
      economics.candidate_price = PriceMicrosPerKwh::FromRaw(0);
      break;
    }
    default: {
      const auto target = FindBestTargetWindow(tariff, request_.desired_window.end(),
                                               bounded_duration_, true);
      if (!target.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the shift target could not be located: " + target.error().message(), tariff_.ref());
        return;
      }
      if (!target.value().has_value()) {
        break;
      }
      const Interval shifted = *target.value();
      const auto cost = CostOfWindow(tariff, shifted, bounded_magnitude_);
      if (!cost.ok()) {
        Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
            "the shifted cost could not be computed exactly: " + cost.error().message(),
            tariff_.ref());
        return;
      }
      economics.shift_target = shifted;
      economics.candidate_cost = cost.value().cost;
      economics.candidate_price = cost.value().average_price;
      Add(ReasonCode::kShiftTargetSelected, kSubjectEconomics,
          "a cheaper window was located at " + FormatRfc3339(shifted.begin()) + " (source " +
              FormatRfc3339(request_.desired_window.begin()) + ")", tariff_.ref());
      break;
    }
  }

  const auto savings = CheckedSub(economics.baseline_cost, economics.candidate_cost);
  if (!savings.ok()) {
    Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
        "the projected saving could not be computed exactly: " + savings.error().message());
    return;
  }
  economics.gross_savings = savings.value();

  const auto total = CheckedAdd(economics.gross_savings, demand_charge_avoided_);
  if (!total.ok()) {
    Add(ReasonCode::kEconomicsOverflow, kSubjectEconomics,
        "the projected benefit could not be computed exactly: " + total.error().message());
    return;
  }
  economics.demand_charge_avoided = demand_charge_avoided_;
  economics.favorable = !(total.value() < policy_.minimum_economic_benefit);

  if (economics.favorable) {
    Add(ReasonCode::kEconomicsFavorable, kSubjectEconomics,
        "projected benefit " + FormatQuantity(total.value()) + " meets the policy minimum of " +
            FormatQuantity(policy_.minimum_economic_benefit),
        tariff_.ref());
    return;
  }
  if (policy_.require_positive_economic_benefit && request_.kind != RequestKind::kReserveRestoration) {
    Add(ReasonCode::kEconomicallyUnfavorable, kSubjectEconomics,
        "projected benefit " + FormatQuantity(total.value()) +
            " is below the policy minimum of " + FormatQuantity(policy_.minimum_economic_benefit) +
            "; the action is operationally possible but not worth taking",
        tariff_.ref());
  }
}

std::optional<BoundedIntent> Evaluation::BuildIntent() const {
  if (!bounded_magnitude_.is_positive() || bounded_duration_.raw() <= 0) {
    return std::nullopt;
  }
  const auto end = AddSeconds(request_.desired_window.begin(), bounded_duration_);
  if (!end.ok()) {
    return std::nullopt;
  }
  const auto window = Interval::Make(request_.desired_window.begin(), end.value());
  if (!window.ok()) {
    return std::nullopt;
  }
  BoundedIntent intent;
  intent.kind = request_.kind;
  intent.service_class = request_.service_class;
  intent.magnitude_limit_kw = bounded_magnitude_;
  intent.duration_limit = bounded_duration_;
  intent.window = window.value();
  intent.expires_at = request_.desired_window.end();
  intent.target_authority = TargetAuthorityFor(request_.kind);
  intent.advisory_only = true;
  return intent;
}

std::vector<EvidenceRef> Evaluation::BuildEvidenceDependencies() const {
  std::vector<EvidenceRef> refs;
  const auto append = [&refs](const auto& slot) {
    if (slot.evidence != nullptr) {
      refs.push_back(slot.ref());
    }
  };
  append(tariff_);
  append(demand_);
  append(reserve_);
  append(efficiency_);
  append(services_);
  append(capacity_);
  append(risk_);
  append(incident_);
  append(zone_);
  return refs;
}

void Evaluation::RuleIntentBounding() {
  // Intent construction happens in Finalize, once the outcome is known. This
  // rule exists to keep the pipeline order explicit and auditable.
}

void Evaluation::Finalize() {
  CanonicalizeReasons(&reasons_);
  if (reasons_.size() > policy_.max_reasons) {
    return;  // Reported by Run after the fact.
  }
  decision_.outcome = OutcomeFromReasons(reasons_);
  decision_.reasons = reasons_;
  decision_.evidence_dependencies = BuildEvidenceDependencies();

  if (decision_.outcome == Outcome::kAllowed) {
    decision_.intent = BuildIntent();
  }
  if (decision_.outcome == Outcome::kDeferred && !decision_.reconsider_at.has_value()) {
    // Defensive: every deferral must name when to ask again. Falling back to the
    // start of the action window keeps the contract even if a future rule forgets.
    decision_.reconsider_at = request_.desired_window.begin();
  }
}

Result<Decision> Evaluation::Run() {
  decision_.request_id = request_.request_id;
  decision_.client_id = request_.client_id;
  decision_.epoch = request_.epoch;
  decision_.policy_id = policy_.id;
  decision_.policy_generation = policy_.generation;
  decision_.kind = request_.kind;
  decision_.service_class = request_.service_class;
  decision_.decided_at = now_;

  InitSlots();

  RuleRequestContract();
  RulePolicyGeneration();
  RuleEvidence();
  RuleWindowCoverage();
  RuleServiceTerms();
  RuleReserveFloor();
  RuleCapacity();
  RuleRisk();
  RuleIncident();
  RuleDemandCharge();
  RuleEconomics();
  RuleIntentBounding();

  Finalize();

  if (reasons_.size() > policy_.max_reasons) {
    return MakeError(ErrorCode::kResourceLimitExceeded, "decision",
                     "the trace produced " + std::to_string(reasons_.size()) +
                         " reasons, above the policy bound of " +
                         std::to_string(policy_.max_reasons));
  }

  decision_.digest = ComputeDecisionDigest(decision_);
  decision_.id = DecisionId::FromRaw(decision_.digest.raw());
  if (decision_.intent.has_value()) {
    DigestBuilder builder;
    builder.Update(decision_.digest.Hex());
    builder.Update("intent");
    decision_.intent->id = IntentId::FromRaw(builder.raw());
  }
  return decision_;
}

}  // namespace

Result<GovernorEngine> GovernorEngine::Make(PolicySet policy) {
  const Status status = ValidatePolicy(policy);
  if (!status.ok()) {
    return status.error();
  }
  return GovernorEngine(std::move(policy));
}

Result<Decision> GovernorEngine::Evaluate(const DecisionRequest& request, const EvidenceSet& evidence,
                                          UtcInstant now) const {
  const Status request_status = ValidateRequest(request);
  if (!request_status.ok()) {
    return request_status.error();
  }
  const Status evidence_status = ValidateEvidenceSet(evidence);
  if (!evidence_status.ok()) {
    return evidence_status.error();
  }
  Evaluation evaluation(policy_, request, evidence, now);
  return evaluation.Run();
}

Result<std::vector<Decision>> GovernorEngine::EvaluateBatch(
    const std::vector<DecisionRequest>& requests, const EvidenceSet& evidence, UtcInstant now) const {
  std::vector<Decision> decisions;
  decisions.reserve(requests.size());
  for (const DecisionRequest& request : requests) {
    ECG_TRY(decision, Evaluate(request, evidence, now));
    decisions.push_back(std::move(decision));
  }
  return decisions;
}

Result<GovernorEngine> MakeEngine(PolicySet policy) { return GovernorEngine::Make(std::move(policy)); }

}  // namespace ecg
