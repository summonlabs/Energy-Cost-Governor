#ifndef ECG_JSON_IO_HPP
#define ECG_JSON_IO_HPP

// Serialisation between the domain model and the strict JSON dialect.
//
// Two properties matter here and are enforced by tests:
//   * Round-trip exactness. parse(dump(v)) reproduces v, including every
//     generation, provenance flag, and fixed-point digit.
//   * Self-verifying decisions. Parsing a decision recomputes its content digest
//     and rejects the value when it does not match, so a journal entry whose
//     bytes were altered cannot be read back as a valid decision.
//
// The dialect is closed: unknown object members are rejected rather than
// ignored, because an ignored field is a decision made on inputs nobody
// validated.

#include <string>
#include <string_view>

#include "ecg/decision.hpp"
#include "ecg/evidence.hpp"
#include "ecg/json.hpp"
#include "ecg/policy.hpp"
#include "ecg/request.hpp"

namespace ecg {

// -- shared helpers ---------------------------------------------------------

[[nodiscard]] JsonValue ToJson(UtcInstant instant);
[[nodiscard]] JsonValue ToJson(const Interval& interval);

/// Renders a quantity as a JSON number carrying its exact decimal literal.
template <class Tag>
[[nodiscard]] JsonValue JsonQuantity(Quantity<Tag> value) {
  Result<JsonValue> number = JsonValue::Number(FormatQuantity(value));
  if (!number.ok()) {
    return JsonValue::String(FormatQuantity(value));
  }
  return number.value();
}

// -- evidence ---------------------------------------------------------------

[[nodiscard]] JsonValue ToJson(const EvidenceMeta& meta);
[[nodiscard]] Result<EvidenceMeta> ParseEvidenceMeta(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const Tariff& tariff);
[[nodiscard]] Result<Tariff> ParseTariff(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const DemandObservation& observation);
[[nodiscard]] Result<DemandObservation> ParseDemandObservation(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const ReserveState& reserve);
[[nodiscard]] Result<ReserveState> ParseReserveState(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const EfficiencyState& efficiency);
[[nodiscard]] Result<EfficiencyState> ParseEfficiencyState(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const ServiceCatalog& catalog);
[[nodiscard]] Result<ServiceCatalog> ParseServiceCatalog(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const CapacityState& capacity);
[[nodiscard]] Result<CapacityState> ParseCapacityState(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const RiskState& risk);
[[nodiscard]] Result<RiskState> ParseRiskState(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const IncidentState& incident);
[[nodiscard]] Result<IncidentState> ParseIncidentState(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const ZoneRules& zone);
[[nodiscard]] Result<ZoneRules> ParseZoneRules(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const EvidenceSet& evidence);
[[nodiscard]] Result<EvidenceSet> ParseEvidenceSet(const JsonValue& value);

// -- policy -----------------------------------------------------------------

[[nodiscard]] JsonValue ToJson(const PolicySet& policy);
[[nodiscard]] Result<PolicySet> ParsePolicySet(const JsonValue& value);

// -- requests and decisions -------------------------------------------------

[[nodiscard]] JsonValue ToJson(const DecisionRequest& request);
[[nodiscard]] Result<DecisionRequest> ParseDecisionRequest(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const EvidenceRef& ref);
[[nodiscard]] Result<EvidenceRef> ParseEvidenceRef(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const Reason& reason);
[[nodiscard]] Result<Reason> ParseReason(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const BoundedIntent& intent);
[[nodiscard]] Result<BoundedIntent> ParseBoundedIntent(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const EconomicAssessment& economics);
[[nodiscard]] Result<EconomicAssessment> ParseEconomicAssessment(const JsonValue& value);

[[nodiscard]] JsonValue ToJson(const Decision& decision);

/// Parses a decision and verifies both its content digest and its identity.
[[nodiscard]] Result<Decision> ParseDecision(const JsonValue& value);

// -- persisted state --------------------------------------------------------

/// Everything needed to resume: the authority epoch, the policy revision in
/// force, and the last observed evidence. Evidence parsed from here is returned
/// with provenance kRecoveredPersistence so it can never be treated as fresh.
struct StateDocument {
  Epoch epoch;
  PolicySet policy;
  EvidenceSet evidence;
  UtcInstant updated_at;
};

[[nodiscard]] JsonValue ToJson(const StateDocument& state);
[[nodiscard]] Result<StateDocument> ParseStateDocument(const JsonValue& value);

/// Marks every present evidence payload as recovered from persistence.
void MarkEvidenceRecovered(EvidenceSet* evidence);

// -- human-readable traces --------------------------------------------------

/// Multi-line, deterministic explanation of a decision, used by the CLI.
[[nodiscard]] std::string FormatDecisionReport(const Decision& decision);

}  // namespace ecg

#endif  // ECG_JSON_IO_HPP
