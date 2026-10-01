#ifndef ECG_EVIDENCE_HPP
#define ECG_EVIDENCE_HPP

// Evidence: the observed inputs this runtime is allowed to reason about.
//
// Every input carries who produced it, which generation it is, which authority
// epoch it belongs to, when it was observed, and how it was obtained. That
// metadata is not decoration: it is the difference between "the reserve floor
// was 4 MW when we looked" and "the reserve floor is 4 MW".
//
// Three principles are encoded directly in the types:
//   * Observation is not ownership. An authority kind records who owns a datum;
//     this runtime only reads it.
//   * Recovered evidence is not fresh evidence. Provenance is part of the value,
//     and recovered dynamic state can never authorise anything.
//   * Absence is explicit. Missing evidence is std::nullopt, never a zero value
//     that happens to look permissive.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ecg/quantity.hpp"
#include "ecg/result.hpp"
#include "ecg/strong.hpp"
#include "ecg/tariff.hpp"
#include "ecg/time.hpp"

namespace ecg {

/// Which adjacent authority owns a datum. This runtime owns none of them.
enum class AuthorityKind : std::uint8_t {
  kTariffAuthority = 1,
  kMeteringAuthority = 2,
  kCapacityAuthority = 3,
  kReserveAuthority = 4,
  kEfficiencyAuthority = 5,
  kServiceCatalogAuthority = 6,
  kRiskAuthority = 7,
  kIncidentAuthority = 8,
  kTimeZoneAuthority = 9,
  /// Synthetic model inputs used in tests and demonstrations. Never a claim of
  /// real facility telemetry.
  kSyntheticModel = 200,
};

[[nodiscard]] const char* AuthorityKindName(AuthorityKind authority) noexcept;

/// How a value was obtained. This is the guard against promoting recovered or
/// modelled state into current operational truth.
enum class Provenance : std::uint8_t {
  /// Read from the owning authority during this process lifetime.
  kLiveObservation = 0,
  /// Reconstructed from persisted state after a restart or reopen.
  kRecoveredPersistence = 1,
  /// Produced by a synthetic model, not by facility hardware or a real feed.
  kSyntheticModel = 2,
};

[[nodiscard]] const char* ProvenanceName(Provenance provenance) noexcept;

/// Envelope metadata that travels with every observed value.
struct EvidenceMeta {
  AuthorityKind authority{AuthorityKind::kSyntheticModel};
  SourceId source;
  EvidenceGeneration generation;
  Epoch epoch;
  UtcInstant observed_at;
  UtcInstant published_at;
  std::uint32_t payload_version{1};
  Provenance provenance{Provenance::kLiveObservation};

  friend bool operator==(const EvidenceMeta& a, const EvidenceMeta& b) noexcept;
};

/// A value plus the metadata that says how much it can be trusted.
template <class Payload>
struct Evidence {
  EvidenceMeta meta;
  Payload value;
};

/// Evidence categories, used for age limits and reason subjects.
enum class EvidenceCategory : std::uint8_t {
  kTariff = 0,
  kDemand = 1,
  kReserve = 2,
  kEfficiency = 3,
  kServices = 4,
  kCapacity = 5,
  kRisk = 6,
  kIncident = 7,
  kZone = 8,
};

[[nodiscard]] const char* EvidenceCategoryName(EvidenceCategory category) noexcept;

/// Freshness classification. Only kFresh may support an authorisation.
enum class Freshness : std::uint8_t {
  kFresh = 0,
  kMissing = 1,
  kStale = 2,
  kFutureDated = 3,
  kRecovered = 4,
  kEpochMismatch = 5,
  kUnsupportedVersion = 6,
  kConflicting = 7,
};

[[nodiscard]] const char* FreshnessName(Freshness freshness) noexcept;
[[nodiscard]] bool IsFresh(Freshness freshness) noexcept;

/// Age and skew limits per category, from policy.
struct FreshnessLimits {
  DurationSec tariff_max_age;
  DurationSec demand_max_age;
  DurationSec reserve_max_age;
  DurationSec efficiency_max_age;
  DurationSec services_max_age;
  DurationSec capacity_max_age;
  DurationSec risk_max_age;
  DurationSec incident_max_age;
  DurationSec zone_max_age;
  /// How far ahead of "now" an observation timestamp may be before it is
  /// rejected as future dated.
  DurationSec future_skew;

  [[nodiscard]] DurationSec MaxAgeFor(EvidenceCategory category) const noexcept;
};

/// Supported payload schema versions per category. A newer payload is refused
/// rather than partially understood.
inline constexpr std::uint32_t kMaxSupportedPayloadVersion = 1;

/// Classifies one piece of evidence. Absence is classified by the caller.
[[nodiscard]] Freshness ClassifyFreshness(const EvidenceMeta& meta, DurationSec max_age,
                                          DurationSec future_skew, UtcInstant now,
                                          Epoch current_epoch);

// ---------------------------------------------------------------------------
// Payloads. Each represents an observation owned by an adjacent authority.
// ---------------------------------------------------------------------------

/// Metering authority: instantaneous and rolling demand.
struct DemandObservation {
  PowerKw current_demand_kw;
  /// Highest metered peak already recorded inside the active demand window.
  PowerKw rolling_peak_kw;
  /// Metering interval the observation summarises.
  DurationSec interval_length;
  UtcInstant interval_end;
};

/// Reserve authority: how much headroom exists and what the floor is.
struct ReserveState {
  PowerKw current_reserve_kw;
  /// Absolute floor that must remain available.
  PowerKw reserve_floor_kw;
  /// Installed capacity the ratio floor is measured against.
  PowerKw capacity_kw;
  /// Relative floor; the effective floor is the greater of the two.
  RatioPpm reserve_floor_ratio;
};

/// Efficiency authority: measured power usage effectiveness.
struct EfficiencyState {
  RatioPpm pue;
  RatioPpm design_pue;
  Interval measurement_window;
  /// Round-trip efficiency of the storage system, when one exists.
  std::optional<RatioPpm> storage_round_trip_ratio;
};

/// How far a service class may be curtailed, as defined by the service catalog
/// authority. This runtime never invents or extends these terms.
enum class CurtailmentPermission : std::uint8_t {
  kProhibited = 0,
  kWithNotice = 1,
  kUnrestricted = 2,
};

[[nodiscard]] const char* CurtailmentPermissionName(CurtailmentPermission permission) noexcept;

/// Terms for one service class.
struct ServiceClassTerms {
  ServiceClassId id;
  CurtailmentPermission permission{CurtailmentPermission::kProhibited};
  /// Largest fraction of nominal load that may be curtailed.
  RatioPpm max_curtailment_ratio;
  /// Longest single curtailment episode.
  DurationSec max_curtailment_duration;
  /// Notice the class requires before an episode begins.
  DurationSec minimum_notice;
  /// Higher values are more protected; used only for deterministic ordering.
  std::int32_t priority{0};
  /// Nominal load of the class, used to check ratio limits and capacity.
  PowerKw nominal_load_kw;
};

/// Service catalog authority: the set of known classes and their terms.
struct ServiceCatalog {
  std::vector<ServiceClassTerms> classes;

  [[nodiscard]] const ServiceClassTerms* Find(const ServiceClassId& id) const noexcept;
};

/// Capacity authority: what could actually move, and how far.
struct CapacityState {
  /// Load that may be reduced without service impact, within the terms above.
  PowerKw flexible_load_kw;
  /// Load that may be increased again later.
  PowerKw shiftable_headroom_kw;
  PowerKw storage_charge_kw;
  PowerKw storage_discharge_kw;
  EnergyKwh storage_usable_energy_kwh;
};

/// Risk posture published by the risk authority.
enum class RiskPosture : std::uint8_t {
  kNormal = 0,
  kElevated = 1,
  kRestricted = 2,
};

[[nodiscard]] const char* RiskPostureName(RiskPosture posture) noexcept;

/// Operational risk constraints observed from the risk authority.
///
/// Each constraint is optional because a risk authority may publish only some of
/// them. A payload that publishes none is rejected by ValidateEvidenceSet: an
/// empty risk feed must not read as "no constraints apply".
struct RiskState {
  RiskPosture posture{RiskPosture::kNormal};
  std::optional<PowerKwPerMin> max_ramp_kw_per_min;
  /// Longest a service may remain deferred.
  std::optional<DurationSec> max_deferral;
  /// Minimum time between changes affecting the same subject.
  std::optional<DurationSec> minimum_dwell;
  UtcInstant last_change_at;

  [[nodiscard]] bool HasAnyConstraint() const noexcept {
    return max_ramp_kw_per_min.has_value() || max_deferral.has_value() || minimum_dwell.has_value();
  }
};

/// Incident severity published by the incident authority. This runtime observes
/// incident state; it never declares, escalates, or clears it.
enum class IncidentSeverity : std::uint8_t {
  kNone = 0,
  kAdvisory = 1,
  kMajor = 2,
  kCritical = 3,
};

[[nodiscard]] const char* IncidentSeverityName(IncidentSeverity severity) noexcept;

struct IncidentState {
  IncidentSeverity severity{IncidentSeverity::kNone};
  std::vector<ServiceClassId> affected_classes;
  UtcInstant declared_at;

  [[nodiscard]] bool Affects(const ServiceClassId& id) const noexcept;
};

// ---------------------------------------------------------------------------
// Evidence bundle
// ---------------------------------------------------------------------------

/// Bounds on consumed evidence structure.
inline constexpr std::size_t kMaxServiceClasses = 4096;
inline constexpr std::size_t kMaxAffectedClasses = 4096;

/// Everything the engine may look at. Each field is optional because absence is
/// a fact the engine must be able to report, not something to paper over.
struct EvidenceSet {
  std::optional<Evidence<Tariff>> tariff;
  std::optional<Evidence<DemandObservation>> demand;
  std::optional<Evidence<ReserveState>> reserve;
  std::optional<Evidence<EfficiencyState>> efficiency;
  std::optional<Evidence<ServiceCatalog>> services;
  std::optional<Evidence<CapacityState>> capacity;
  std::optional<Evidence<RiskState>> risk;
  std::optional<Evidence<IncidentState>> incident;
  std::optional<Evidence<ZoneRules>> zone;

  /// Authority epoch this bundle belongs to.
  Epoch epoch;
};

/// Structural validation of every present payload. Returns the first failure so
/// the caller can name the offending authority precisely.
[[nodiscard]] Status ValidateEvidenceSet(const EvidenceSet& evidence);

/// The effective reserve floor: the greater of the absolute and ratio floors.
[[nodiscard]] Result<PowerKw> EffectiveReserveFloor(const ReserveState& reserve);

}  // namespace ecg

#endif  // ECG_EVIDENCE_HPP
