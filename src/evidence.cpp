#include "ecg/evidence.hpp"

#include "ecg/hash.hpp"

namespace ecg {

const char* AuthorityKindName(AuthorityKind authority) noexcept {
  switch (authority) {
    case AuthorityKind::kTariffAuthority: return "tariff_authority";
    case AuthorityKind::kMeteringAuthority: return "metering_authority";
    case AuthorityKind::kCapacityAuthority: return "capacity_authority";
    case AuthorityKind::kReserveAuthority: return "reserve_authority";
    case AuthorityKind::kEfficiencyAuthority: return "efficiency_authority";
    case AuthorityKind::kServiceCatalogAuthority: return "service_catalog_authority";
    case AuthorityKind::kRiskAuthority: return "risk_authority";
    case AuthorityKind::kIncidentAuthority: return "incident_authority";
    case AuthorityKind::kTimeZoneAuthority: return "time_zone_authority";
    case AuthorityKind::kSyntheticModel: return "synthetic_model";
  }
  return "unknown";
}

const char* ProvenanceName(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::kLiveObservation: return "live_observation";
    case Provenance::kRecoveredPersistence: return "recovered_persistence";
    case Provenance::kSyntheticModel: return "synthetic_model";
  }
  return "unknown";
}

const char* EvidenceCategoryName(EvidenceCategory category) noexcept {
  switch (category) {
    case EvidenceCategory::kTariff: return "tariff";
    case EvidenceCategory::kDemand: return "demand";
    case EvidenceCategory::kReserve: return "reserve";
    case EvidenceCategory::kEfficiency: return "efficiency";
    case EvidenceCategory::kServices: return "services";
    case EvidenceCategory::kCapacity: return "capacity";
    case EvidenceCategory::kRisk: return "risk";
    case EvidenceCategory::kIncident: return "incident";
    case EvidenceCategory::kZone: return "zone";
  }
  return "unknown";
}

const char* FreshnessName(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::kFresh: return "fresh";
    case Freshness::kMissing: return "missing";
    case Freshness::kStale: return "stale";
    case Freshness::kFutureDated: return "future_dated";
    case Freshness::kRecovered: return "recovered";
    case Freshness::kEpochMismatch: return "epoch_mismatch";
    case Freshness::kUnsupportedVersion: return "unsupported_version";
    case Freshness::kConflicting: return "conflicting";
  }
  return "unknown";
}

bool IsFresh(Freshness freshness) noexcept { return freshness == Freshness::kFresh; }

const char* CurtailmentPermissionName(CurtailmentPermission permission) noexcept {
  switch (permission) {
    case CurtailmentPermission::kProhibited: return "prohibited";
    case CurtailmentPermission::kWithNotice: return "with_notice";
    case CurtailmentPermission::kUnrestricted: return "unrestricted";
  }
  return "unknown";
}

const char* RiskPostureName(RiskPosture posture) noexcept {
  switch (posture) {
    case RiskPosture::kNormal: return "normal";
    case RiskPosture::kElevated: return "elevated";
    case RiskPosture::kRestricted: return "restricted";
  }
  return "unknown";
}

const char* IncidentSeverityName(IncidentSeverity severity) noexcept {
  switch (severity) {
    case IncidentSeverity::kNone: return "none";
    case IncidentSeverity::kAdvisory: return "advisory";
    case IncidentSeverity::kMajor: return "major";
    case IncidentSeverity::kCritical: return "critical";
  }
  return "unknown";
}

bool operator==(const EvidenceMeta& a, const EvidenceMeta& b) noexcept {
  return a.authority == b.authority && a.source == b.source && a.generation == b.generation &&
         a.epoch == b.epoch && a.observed_at == b.observed_at && a.published_at == b.published_at &&
         a.payload_version == b.payload_version && a.provenance == b.provenance;
}

DurationSec FreshnessLimits::MaxAgeFor(EvidenceCategory category) const noexcept {
  switch (category) {
    case EvidenceCategory::kTariff: return tariff_max_age;
    case EvidenceCategory::kDemand: return demand_max_age;
    case EvidenceCategory::kReserve: return reserve_max_age;
    case EvidenceCategory::kEfficiency: return efficiency_max_age;
    case EvidenceCategory::kServices: return services_max_age;
    case EvidenceCategory::kCapacity: return capacity_max_age;
    case EvidenceCategory::kRisk: return risk_max_age;
    case EvidenceCategory::kIncident: return incident_max_age;
    case EvidenceCategory::kZone: return zone_max_age;
  }
  return DurationSec::FromRaw(0);
}

Freshness ClassifyFreshness(const EvidenceMeta& meta, DurationSec max_age, DurationSec future_skew,
                            UtcInstant now, Epoch current_epoch) {
  if (meta.payload_version > kMaxSupportedPayloadVersion) {
    return Freshness::kUnsupportedVersion;
  }
  if (meta.provenance == Provenance::kRecoveredPersistence) {
    // Recovered state is never promoted to current truth, however recent it is.
    return Freshness::kRecovered;
  }
  if (!(meta.epoch == current_epoch)) {
    return Freshness::kEpochMismatch;
  }
  if (now < meta.observed_at) {
    const auto skew = ExceedsBy(now, meta.observed_at, future_skew);
    if (!skew.ok() || skew.value()) {
      return Freshness::kFutureDated;
    }
  }
  if (meta.observed_at < now) {
    const auto age = ExceedsBy(meta.observed_at, now, max_age);
    if (!age.ok() || age.value()) {
      return Freshness::kStale;
    }
  }
  return Freshness::kFresh;
}

const ServiceClassTerms* ServiceCatalog::Find(const ServiceClassId& id) const noexcept {
  for (const ServiceClassTerms& terms : classes) {
    if (terms.id == id) {
      return &terms;
    }
  }
  return nullptr;
}

bool IncidentState::Affects(const ServiceClassId& id) const noexcept {
  for (const ServiceClassId& affected : affected_classes) {
    if (affected == id) {
      return true;
    }
  }
  return false;
}

Result<PowerKw> EffectiveReserveFloor(const ReserveState& reserve) {
  ECG_TRY(ratio_floor, CheckedMulDiv(reserve.capacity_kw.raw(), reserve.reserve_floor_ratio.raw(),
                                     1000000));
  const std::int64_t effective =
      ratio_floor > reserve.reserve_floor_kw.raw() ? ratio_floor : reserve.reserve_floor_kw.raw();
  return QuantityFromRaw<PowerKwTag>(effective);
}

Status ValidateEvidenceSet(const EvidenceSet& evidence) {
  if (evidence.tariff.has_value()) {
    // Structure only: an internally contradictory schedule is admitted and then
    // reported as conflicting evidence, so the operator sees it in the trace.
    const Status tariff_status = ValidateTariffStructure(evidence.tariff->value);
    if (!tariff_status.ok()) {
      return tariff_status.error();
    }
  }
  if (evidence.services.has_value()) {
    const ServiceCatalog& catalog = evidence.services->value;
    if (catalog.classes.size() > kMaxServiceClasses) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "services",
                       "more than " + std::to_string(kMaxServiceClasses) + " service classes");
    }
    for (std::size_t i = 0; i < catalog.classes.size(); ++i) {
      const ServiceClassTerms& terms = catalog.classes[i];
      if (terms.id.empty()) {
        return MakeError(ErrorCode::kMissingRequiredField, "services",
                         "service class at index " + std::to_string(i) + " has an empty id");
      }
      if (terms.max_curtailment_ratio.raw() < 0 || terms.max_curtailment_ratio.raw() > 1000000) {
        return MakeError(ErrorCode::kOutOfRange, "services:" + terms.id.ToString(),
                         "max_curtailment_ratio must be within [0, 1]");
      }
      if (terms.nominal_load_kw.is_negative()) {
        return MakeError(ErrorCode::kInvalidArgument, "services:" + terms.id.ToString(),
                         "nominal load must not be negative");
      }
      for (std::size_t j = i + 1; j < catalog.classes.size(); ++j) {
        if (catalog.classes[j].id == terms.id) {
          return MakeError(ErrorCode::kAlreadyExists, "services",
                           "duplicate service class id '" + terms.id.ToString() + "'");
        }
      }
    }
  }
  if (evidence.reserve.has_value()) {
    const ReserveState& reserve = evidence.reserve->value;
    if (reserve.capacity_kw.is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "reserve", "capacity must not be negative");
    }
    if (reserve.reserve_floor_kw.is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "reserve", "reserve floor must not be negative");
    }
    if (reserve.reserve_floor_ratio.raw() < 0 || reserve.reserve_floor_ratio.raw() > 1000000) {
      return MakeError(ErrorCode::kOutOfRange, "reserve", "reserve_floor_ratio must be within [0, 1]");
    }
  }
  if (evidence.efficiency.has_value()) {
    const EfficiencyState& efficiency = evidence.efficiency->value;
    if (!efficiency.pue.is_positive()) {
      return MakeError(ErrorCode::kOutOfRange, "efficiency", "PUE must be positive");
    }
    if (efficiency.storage_round_trip_ratio.has_value() &&
        (!efficiency.storage_round_trip_ratio->is_positive() ||
         efficiency.storage_round_trip_ratio->raw() > 1000000)) {
      return MakeError(ErrorCode::kOutOfRange, "efficiency",
                       "storage round-trip ratio must be within (0, 1]");
    }
  }
  if (evidence.capacity.has_value()) {
    const CapacityState& capacity = evidence.capacity->value;
    if (capacity.flexible_load_kw.is_negative() || capacity.shiftable_headroom_kw.is_negative() ||
        capacity.storage_charge_kw.is_negative() || capacity.storage_discharge_kw.is_negative() ||
        capacity.storage_usable_energy_kwh.is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "capacity", "capacity values must not be negative");
    }
  }
  if (evidence.risk.has_value()) {
    const RiskState& risk = evidence.risk->value;
    if (!risk.HasAnyConstraint()) {
      return MakeError(ErrorCode::kInvalidArgument, "risk",
                       "risk evidence carries no constraint at all; an empty risk feed must not read "
                       "as permission");
    }
    if (risk.max_ramp_kw_per_min.has_value() && risk.max_ramp_kw_per_min->is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "risk", "max ramp must not be negative");
    }
  }
  if (evidence.incident.has_value()) {
    if (evidence.incident->value.affected_classes.size() > kMaxAffectedClasses) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "incident",
                       "more than " + std::to_string(kMaxAffectedClasses) + " affected classes");
    }
  }
  if (evidence.demand.has_value()) {
    if (evidence.demand->value.interval_length.raw() <= 0) {
      return MakeError(ErrorCode::kInvalidArgument, "demand", "metering interval must be positive");
    }
    if (evidence.demand->value.current_demand_kw.is_negative()) {
      return MakeError(ErrorCode::kInvalidArgument, "demand", "demand must not be negative");
    }
  }
  return OkStatus();
}

}  // namespace ecg
