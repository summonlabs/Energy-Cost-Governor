#include "ecg/json_io.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "ecg/engine.hpp"
#include "ecg/hash.hpp"

namespace ecg {
namespace {

[[nodiscard]] JsonValue JsonUint(std::uint64_t value) {
  Result<JsonValue> number = JsonValue::Number(std::to_string(value));
  return number.ok() ? number.value() : JsonValue::String(std::to_string(value));
}

[[nodiscard]] JsonValue JsonInt(std::int64_t value) {
  Result<JsonValue> number = JsonValue::Number(std::to_string(value));
  return number.ok() ? number.value() : JsonValue::String(std::to_string(value));
}

[[nodiscard]] JsonValue JsonBool(bool value) { return JsonValue::Bool(value); }

/// Reads members of a JSON object with strict typing and strict key checking.
class Reader {
 public:
  Reader(const JsonValue& value, std::string context)
      : value_(&value), context_(std::move(context)) {}

  [[nodiscard]] const JsonValue& value() const noexcept { return *value_; }
  [[nodiscard]] const std::string& context() const noexcept { return context_; }

  [[nodiscard]] Error Err(ErrorCode code, std::string message) const {
    return MakeError(code, context_, std::move(message));
  }

  [[nodiscard]] Status ExpectObject() const {
    if (!value_->is_object()) {
      return Err(ErrorCode::kMalformedInput, "expected a JSON object");
    }
    return OkStatus();
  }

  [[nodiscard]] Result<const JsonValue*> Optional(std::string_view key) const {
    if (!value_->is_object()) {
      return Err(ErrorCode::kMalformedInput, "expected a JSON object");
    }
    return value_->Find(key);
  }

  [[nodiscard]] Result<const JsonValue*> Require(std::string_view key) const {
    if (!value_->is_object()) {
      return Err(ErrorCode::kMalformedInput, "expected a JSON object");
    }
    const JsonValue* member = value_->Find(key);
    if (member == nullptr) {
      return Err(ErrorCode::kMissingRequiredField, "missing member '" + std::string(key) + "'");
    }
    return member;
  }

  /// Rejects any member not in the allowed list. An ignored field would be a
  /// decision taken on input nobody validated.
  [[nodiscard]] Status AllowOnly(std::initializer_list<std::string_view> allowed) const {
    if (!value_->is_object()) {
      return Err(ErrorCode::kMalformedInput, "expected a JSON object");
    }
    for (const auto& member : value_->as_object()) {
      bool known = false;
      for (const std::string_view candidate : allowed) {
        if (member.first == candidate) {
          known = true;
          break;
        }
      }
      if (!known) {
        return Err(ErrorCode::kUnsupportedValue, "unknown member '" + member.first + "'");
      }
    }
    return OkStatus();
  }

  [[nodiscard]] Result<std::string> String(std::string_view key) const {
    ECG_TRY(member, Require(key));
    if (!member->is_string()) {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be a string");
    }
    return member->as_string();
  }

  [[nodiscard]] Result<std::string> OptionalString(std::string_view key,
                                                  std::string fallback) const {
    ECG_TRY(member, Optional(key));
    if (member == nullptr) {
      return fallback;
    }
    if (!member->is_string()) {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be a string");
    }
    return member->as_string();
  }

  [[nodiscard]] Result<std::uint64_t> Uint(std::string_view key) const {
    ECG_TRY(member, Require(key));
    return AsUint(*member, key);
  }

  [[nodiscard]] Result<std::uint64_t> OptionalUint(std::string_view key,
                                                   std::uint64_t fallback) const {
    ECG_TRY(member, Optional(key));
    if (member == nullptr) {
      return fallback;
    }
    return AsUint(*member, key);
  }

  [[nodiscard]] Result<bool> Bool(std::string_view key) const {
    ECG_TRY(member, Require(key));
    if (!member->is_bool()) {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be a boolean");
    }
    return member->as_bool();
  }

  [[nodiscard]] Result<bool> OptionalBool(std::string_view key, bool fallback) const {
    ECG_TRY(member, Optional(key));
    if (member == nullptr) {
      return fallback;
    }
    if (!member->is_bool()) {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be a boolean");
    }
    return member->as_bool();
  }

  [[nodiscard]] Result<UtcInstant> Instant(std::string_view key) const {
    ECG_TRY(text, String(key));
    const auto parsed = ParseRfc3339(text);
    if (!parsed.ok()) {
      return Err(parsed.error().code(),
                 "member '" + std::string(key) + "': " + parsed.error().message());
    }
    return parsed.value();
  }

  [[nodiscard]] Result<UtcInstant> OptionalInstant(std::string_view key,
                                                   UtcInstant fallback) const {
    ECG_TRY(member, Optional(key));
    if (member == nullptr) {
      return fallback;
    }
    if (!member->is_string()) {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be a string");
    }
    const auto parsed = ParseRfc3339(member->as_string());
    if (!parsed.ok()) {
      return Err(parsed.error().code(),
                 "member '" + std::string(key) + "': " + parsed.error().message());
    }
    return parsed.value();
  }

  template <class Tag>
  [[nodiscard]] Result<ecg::Quantity<Tag>> Quantity(std::string_view key) const {
    ECG_TRY(member, Require(key));
    return AsQuantity<Tag>(*member, key);
  }

  template <class Tag>
  [[nodiscard]] Result<std::optional<ecg::Quantity<Tag>>> OptionalQuantity(
      std::string_view key) const {
    ECG_TRY(member, Optional(key));
    if (member == nullptr) {
      return std::optional<ecg::Quantity<Tag>>{};
    }
    ECG_TRY(parsed, AsQuantity<Tag>(*member, key));
    return std::optional<ecg::Quantity<Tag>>{parsed};
  }

 private:
  [[nodiscard]] Result<std::uint64_t> AsUint(const JsonValue& member, std::string_view key) const {
    std::string literal;
    if (member.is_number()) {
      literal = member.as_number_text();
    } else if (member.is_string()) {
      literal = member.as_string();
    } else {
      return Err(ErrorCode::kMalformedInput, "member '" + std::string(key) + "' must be an integer");
    }
    if (literal.empty() || literal.size() > 20) {
      return Err(ErrorCode::kOutOfRange, "member '" + std::string(key) + "' is not a 64-bit integer");
    }
    std::uint64_t value = 0;
    for (const char c : literal) {
      if (c < '0' || c > '9') {
        return Err(ErrorCode::kMalformedInput,
                   "member '" + std::string(key) + "' must be a non-negative integer");
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10u) {
        return Err(ErrorCode::kOutOfRange,
                   "member '" + std::string(key) + "' does not fit in 64 bits");
      }
      value = value * 10u + digit;
    }
    return value;
  }

  template <class Tag>
  [[nodiscard]] Result<ecg::Quantity<Tag>> AsQuantity(const JsonValue& member,
                                                      std::string_view key) const {
    std::string literal;
    if (member.is_number()) {
      literal = member.as_number_text();
    } else if (member.is_string()) {
      literal = member.as_string();
    } else {
      return Err(ErrorCode::kMalformedInput,
                 "member '" + std::string(key) + "' must be a decimal number");
    }
    const auto parsed = ParseQuantity<Tag>(literal);
    if (!parsed.ok()) {
      return Err(parsed.error().code(),
                 "member '" + std::string(key) + "': " + parsed.error().message());
    }
    return parsed.value();
  }

  const JsonValue* value_;
  std::string context_;
};

template <class Enum, class NameFn, class Container>
[[nodiscard]] Result<Enum> EnumByName(std::string_view text, NameFn name_fn, const Container& values,
                                     const char* kind) {
  for (const Enum value : values) {
    if (text == name_fn(value)) {
      return value;
    }
  }
  return MakeError(ErrorCode::kUnsupportedValue, kind,
                   "unknown value '" + std::string(text) + "'");
}

constexpr AuthorityKind kAuthorityKinds[] = {
    AuthorityKind::kTariffAuthority,       AuthorityKind::kMeteringAuthority,
    AuthorityKind::kCapacityAuthority,     AuthorityKind::kReserveAuthority,
    AuthorityKind::kEfficiencyAuthority,   AuthorityKind::kServiceCatalogAuthority,
    AuthorityKind::kRiskAuthority,         AuthorityKind::kIncidentAuthority,
    AuthorityKind::kTimeZoneAuthority,     AuthorityKind::kSyntheticModel};

constexpr Provenance kProvenances[] = {Provenance::kLiveObservation,
                                       Provenance::kRecoveredPersistence,
                                       Provenance::kSyntheticModel};

constexpr RiskPosture kRiskPostures[] = {RiskPosture::kNormal, RiskPosture::kElevated,
                                         RiskPosture::kRestricted};

constexpr IncidentSeverity kIncidentSeverities[] = {
    IncidentSeverity::kNone, IncidentSeverity::kAdvisory, IncidentSeverity::kMajor,
    IncidentSeverity::kCritical};

constexpr CurtailmentPermission kCurtailmentPermissions[] = {
    CurtailmentPermission::kProhibited, CurtailmentPermission::kWithNotice,
    CurtailmentPermission::kUnrestricted};

constexpr DemandBasis kDemandBases[] = {DemandBasis::kPeakIntervalAverage, DemandBasis::kMaxInterval};

constexpr Outcome kOutcomes[] = {Outcome::kAllowed, Outcome::kRefused, Outcome::kDeferred,
                                 Outcome::kIndeterminate};

constexpr ReasonSeverity kSeverities[] = {ReasonSeverity::kInfo, ReasonSeverity::kNotice,
                                          ReasonSeverity::kBlocker};

constexpr ReasonCode kReasonCodes[] = {
    ReasonCode::kNone,
    ReasonCode::kAuthorized,
    ReasonCode::kIntentBounded,
    ReasonCode::kEconomicsFavorable,
    ReasonCode::kDemandChargeAvoided,
    ReasonCode::kPriceCoverageVerified,
    ReasonCode::kShiftTargetSelected,
    ReasonCode::kServiceTermsSatisfied,
    ReasonCode::kReserveHeadroomSufficient,
    ReasonCode::kIncidentClear,
    ReasonCode::kPolicyGenerationCurrent,
    ReasonCode::kEvidenceMissing,
    ReasonCode::kEvidenceStale,
    ReasonCode::kEvidenceFutureDated,
    ReasonCode::kEvidenceRecovered,
    ReasonCode::kEvidenceEpochMismatch,
    ReasonCode::kEvidenceUnsupportedVersion,
    ReasonCode::kEvidenceConflicting,
    ReasonCode::kEvidenceInvalid,
    ReasonCode::kPolicyGenerationStale,
    ReasonCode::kRequestEpochMismatch,
    ReasonCode::kPriceCoverageGap,
    ReasonCode::kPriceCoverageAmbiguous,
    ReasonCode::kZoneRulesMissing,
    ReasonCode::kEconomicsOverflow,
    ReasonCode::kResourceLimitExceeded,
    ReasonCode::kTariffValidityMiss,
    ReasonCode::kStorageEfficiencyMissing,
    ReasonCode::kDemandWindowUnresolved,
    ReasonCode::kReserveFloorBreach,
    ReasonCode::kServiceClassUnknown,
    ReasonCode::kServiceCurtailmentProhibited,
    ReasonCode::kServiceNoticeInsufficient,
    ReasonCode::kServiceDurationExceeded,
    ReasonCode::kServiceRatioExceeded,
    ReasonCode::kCapacityInsufficient,
    ReasonCode::kRiskPostureRestricted,
    ReasonCode::kRampLimitExceeded,
    ReasonCode::kDwellNotElapsed,
    ReasonCode::kIncidentCritical,
    ReasonCode::kWindowInPast,
    ReasonCode::kIntentDurationExceedsPolicy,
    ReasonCode::kDemandChargeIncrease,
    ReasonCode::kEconomicallyUnfavorable,
    ReasonCode::kStorageEnergyInsufficient,
    ReasonCode::kStoragePowerInsufficient,
    ReasonCode::kDeferralLimitExceeded,
    ReasonCode::kMagnitudeExceedsPolicy,
    ReasonCode::kIncidentActive,
    ReasonCode::kWindowBeyondHorizon,
    ReasonCode::kDemandThresholdNotProjected,
    ReasonCode::kShiftTargetNotYetAvailable};

constexpr RequestKind kRequestKinds[] = {
    RequestKind::kShiftFlexibleLoad, RequestKind::kCurtailServiceClass,
    RequestKind::kDeferBatchWork,    RequestKind::kChargeStorage,
    RequestKind::kDischargeStorage,  RequestKind::kPreCooling,
    RequestKind::kReserveRestoration};

constexpr Rationale kRationales[] = {
    Rationale::kDemandChargeAvoidance, Rationale::kPriceArbitrage, Rationale::kEfficiencyRebalance,
    Rationale::kReserveRestoration,    Rationale::kObligationCompliance};

[[nodiscard]] Result<Interval> ParseInterval(const JsonValue& value, const std::string& context) {
  Reader reader(value, context);
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"begin", "end"}));
  ECG_TRY(begin, reader.Instant("begin"));
  ECG_TRY(end, reader.Instant("end"));
  const auto interval = Interval::Make(begin, end);
  if (!interval.ok()) {
    return reader.Err(interval.error().code(), interval.error().message());
  }
  return interval.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitive serialisers
// ---------------------------------------------------------------------------

JsonValue ToJson(UtcInstant instant) { return JsonValue::String(FormatRfc3339(instant)); }

JsonValue ToJson(const Interval& interval) {
  JsonValue::Object members;
  members.emplace_back("begin", ToJson(interval.begin()));
  members.emplace_back("end", ToJson(interval.end()));
  return JsonValue::ObjectValue(std::move(members));
}

// ---------------------------------------------------------------------------
// Evidence metadata and payloads
// ---------------------------------------------------------------------------

JsonValue ToJson(const EvidenceMeta& meta) {
  JsonValue::Object members;
  members.emplace_back("authority", JsonValue::String(AuthorityKindName(meta.authority)));
  members.emplace_back("source", JsonValue::String(meta.source.value()));
  members.emplace_back("generation", JsonUint(meta.generation.raw()));
  members.emplace_back("epoch", JsonUint(meta.epoch.raw()));
  members.emplace_back("observed_at", ToJson(meta.observed_at));
  members.emplace_back("published_at", ToJson(meta.published_at));
  members.emplace_back("payload_version", JsonUint(meta.payload_version));
  members.emplace_back("provenance", JsonValue::String(ProvenanceName(meta.provenance)));
  return JsonValue::ObjectValue(std::move(members));
}

Result<EvidenceMeta> ParseEvidenceMeta(const JsonValue& value) {
  Reader reader(value, "evidence.meta");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"authority", "source", "generation", "epoch", "observed_at",
                                   "published_at", "payload_version", "provenance"}));
  ECG_TRY(authority_name, reader.String("authority"));
  ECG_TRY(authority, EnumByName<AuthorityKind>(authority_name, AuthorityKindName, kAuthorityKinds,
                                               "authority_kind"));
  ECG_TRY(source_text, reader.String("source"));
  ECG_TRY(source, SourceId::Parse(source_text));
  ECG_TRY(generation, reader.Uint("generation"));
  ECG_TRY(epoch, reader.Uint("epoch"));
  ECG_TRY(observed_at, reader.Instant("observed_at"));
  ECG_TRY(published_at, reader.Instant("published_at"));
  ECG_TRY(payload_version, reader.OptionalUint("payload_version", 1));
  ECG_TRY(provenance_name, reader.OptionalString("provenance", "live_observation"));
  ECG_TRY(provenance, EnumByName<Provenance>(provenance_name, ProvenanceName, kProvenances,
                                             "provenance"));

  EvidenceMeta meta;
  meta.authority = authority;
  meta.source = source;
  meta.generation = EvidenceGeneration::FromRaw(generation);
  meta.epoch = Epoch::FromRaw(epoch);
  meta.observed_at = observed_at;
  meta.published_at = published_at;
  meta.payload_version = static_cast<std::uint32_t>(payload_version);
  meta.provenance = provenance;
  return meta;
}

JsonValue ToJson(const Tariff& tariff) {
  JsonValue::Object members;
  members.emplace_back("id", JsonValue::String(tariff.id.value()));
  members.emplace_back("revision", JsonUint(tariff.revision.raw()));
  members.emplace_back("epoch", JsonUint(tariff.epoch.raw()));
  members.emplace_back("currency", JsonValue::String(tariff.currency));
  members.emplace_back("validity", ToJson(tariff.validity));
  members.emplace_back("published_at", ToJson(tariff.published_at));

  JsonValue::Array prices;
  prices.reserve(tariff.prices.size());
  for (const PriceInterval& interval : tariff.prices) {
    JsonValue::Object entry;
    entry.emplace_back("window", ToJson(interval.window));
    entry.emplace_back("price", JsonQuantity(interval.price));
    prices.push_back(JsonValue::ObjectValue(std::move(entry)));
  }
  members.emplace_back("prices", JsonValue::ArrayValue(std::move(prices)));

  JsonValue::Array rules;
  rules.reserve(tariff.demand_charges.size());
  for (const DemandChargeRule& rule : tariff.demand_charges) {
    JsonValue::Object entry;
    entry.emplace_back("id", JsonValue::String(rule.id.value()));
    entry.emplace_back("weekday_mask", JsonUint(rule.recurrence.weekday_mask));
    entry.emplace_back("start_second_of_day", JsonInt(rule.recurrence.start_second_of_day));
    entry.emplace_back("duration_seconds", JsonInt(rule.recurrence.duration_seconds));
    entry.emplace_back("averaging_interval_s", JsonQuantity(rule.averaging_interval));
    entry.emplace_back("threshold_kw", JsonQuantity(rule.threshold_kw));
    entry.emplace_back("charge_per_kw", JsonQuantity(rule.charge_per_kw));
    entry.emplace_back("basis", JsonValue::String(DemandBasisName(rule.basis)));
    entry.emplace_back("validity", ToJson(rule.validity));
    rules.push_back(JsonValue::ObjectValue(std::move(entry)));
  }
  members.emplace_back("demand_charges", JsonValue::ArrayValue(std::move(rules)));
  return JsonValue::ObjectValue(std::move(members));
}

Result<Tariff> ParseTariff(const JsonValue& value) {
  Reader reader(value, "evidence.tariff");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"id", "revision", "epoch", "currency", "validity", "published_at",
                                   "prices", "demand_charges"}));
  ECG_TRY(id_text, reader.String("id"));
  ECG_TRY(id, TariffId::Parse(id_text));
  ECG_TRY(revision, reader.Uint("revision"));
  ECG_TRY(epoch, reader.Uint("epoch"));
  ECG_TRY(currency, reader.String("currency"));
  ECG_TRY(validity_value, reader.Require("validity"));
  ECG_TRY(validity, ParseInterval(*validity_value, "evidence.tariff.validity"));
  ECG_TRY(published_at, reader.Instant("published_at"));

  Tariff tariff;
  tariff.id = id;
  tariff.revision = TariffRevision::FromRaw(revision);
  tariff.epoch = Epoch::FromRaw(epoch);
  tariff.currency = currency;
  tariff.validity = validity;
  tariff.published_at = published_at;

  ECG_TRY(prices_value, reader.Require("prices"));
  if (!prices_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'prices' must be an array");
  }
  for (const JsonValue& entry : prices_value->as_array()) {
    Reader entry_reader(entry, "evidence.tariff.prices[]");
    ECG_TRY_STATUS(entry_reader.AllowOnly({"window", "price"}));
    ECG_TRY(window_value, entry_reader.Require("window"));
    ECG_TRY(window, ParseInterval(*window_value, "evidence.tariff.prices[].window"));
    ECG_TRY(price, entry_reader.Quantity<PriceTag>("price"));
    PriceInterval interval;
    interval.window = window;
    interval.price = price;
    tariff.prices.push_back(interval);
    if (tariff.prices.size() > kMaxPriceIntervals) {
      return reader.Err(ErrorCode::kResourceLimitExceeded, "more price intervals than the bound");
    }
  }

  ECG_TRY(rules_value, reader.Require("demand_charges"));
  if (!rules_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'demand_charges' must be an array");
  }
  for (const JsonValue& entry : rules_value->as_array()) {
    Reader entry_reader(entry, "evidence.tariff.demand_charges[]");
    ECG_TRY_STATUS(entry_reader.AllowOnly({"id", "weekday_mask", "start_second_of_day",
                                           "duration_seconds", "averaging_interval_s",
                                           "threshold_kw", "charge_per_kw", "basis", "validity"}));
    ECG_TRY(rule_id_text, entry_reader.String("id"));
    ECG_TRY(rule_id, DemandWindowId::Parse(rule_id_text));
    ECG_TRY(mask, entry_reader.Uint("weekday_mask"));
    if (mask > 0x7Fu) {
      return entry_reader.Err(ErrorCode::kOutOfRange, "weekday_mask must be within [0, 127]");
    }
    ECG_TRY(start_second, entry_reader.Quantity<DurationTag>("start_second_of_day"));
    ECG_TRY(duration_seconds, entry_reader.Quantity<DurationTag>("duration_seconds"));
    ECG_TRY(averaging, entry_reader.Quantity<DurationTag>("averaging_interval_s"));
    ECG_TRY(threshold, entry_reader.Quantity<PowerKwTag>("threshold_kw"));
    ECG_TRY(charge, entry_reader.Quantity<MoneyTag>("charge_per_kw"));
    ECG_TRY(basis_name, entry_reader.OptionalString("basis", "peak_interval_average"));
    ECG_TRY(basis, EnumByName<DemandBasis>(basis_name, DemandBasisName, kDemandBases, "demand_basis"));
    ECG_TRY(rule_validity_value, entry_reader.Require("validity"));
    ECG_TRY(rule_validity, ParseInterval(*rule_validity_value, "evidence.tariff.demand_charges[].validity"));

    DemandChargeRule rule;
    rule.id = rule_id;
    rule.recurrence.weekday_mask = static_cast<std::uint8_t>(mask);
    rule.recurrence.start_second_of_day = static_cast<std::int32_t>(start_second.raw());
    rule.recurrence.duration_seconds = static_cast<std::int32_t>(duration_seconds.raw());
    rule.averaging_interval = averaging;
    rule.threshold_kw = threshold;
    rule.charge_per_kw = charge;
    rule.basis = basis;
    rule.validity = rule_validity;
    tariff.demand_charges.push_back(rule);
    if (tariff.demand_charges.size() > kMaxDemandChargeRules) {
      return reader.Err(ErrorCode::kResourceLimitExceeded, "more demand-charge rules than the bound");
    }
  }
  return tariff;
}

JsonValue ToJson(const DemandObservation& observation) {
  JsonValue::Object members;
  members.emplace_back("current_demand_kw", JsonQuantity(observation.current_demand_kw));
  members.emplace_back("rolling_peak_kw", JsonQuantity(observation.rolling_peak_kw));
  members.emplace_back("interval_length_s", JsonQuantity(observation.interval_length));
  members.emplace_back("interval_end", ToJson(observation.interval_end));
  return JsonValue::ObjectValue(std::move(members));
}

Result<DemandObservation> ParseDemandObservation(const JsonValue& value) {
  Reader reader(value, "evidence.demand");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"current_demand_kw", "rolling_peak_kw", "interval_length_s",
                                   "interval_end"}));
  DemandObservation observation;
  ECG_TRY(current, reader.Quantity<PowerKwTag>("current_demand_kw"));
  ECG_TRY(peak, reader.Quantity<PowerKwTag>("rolling_peak_kw"));
  ECG_TRY(length, reader.Quantity<DurationTag>("interval_length_s"));
  ECG_TRY(end, reader.Instant("interval_end"));
  observation.current_demand_kw = current;
  observation.rolling_peak_kw = peak;
  observation.interval_length = length;
  observation.interval_end = end;
  return observation;
}

JsonValue ToJson(const ReserveState& reserve) {
  JsonValue::Object members;
  members.emplace_back("current_reserve_kw", JsonQuantity(reserve.current_reserve_kw));
  members.emplace_back("reserve_floor_kw", JsonQuantity(reserve.reserve_floor_kw));
  members.emplace_back("capacity_kw", JsonQuantity(reserve.capacity_kw));
  members.emplace_back("reserve_floor_ratio", JsonQuantity(reserve.reserve_floor_ratio));
  return JsonValue::ObjectValue(std::move(members));
}

Result<ReserveState> ParseReserveState(const JsonValue& value) {
  Reader reader(value, "evidence.reserve");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"current_reserve_kw", "reserve_floor_kw", "capacity_kw",
                                   "reserve_floor_ratio"}));
  ReserveState reserve;
  ECG_TRY(current, reader.Quantity<PowerKwTag>("current_reserve_kw"));
  ECG_TRY(floor, reader.Quantity<PowerKwTag>("reserve_floor_kw"));
  ECG_TRY(capacity, reader.Quantity<PowerKwTag>("capacity_kw"));
  ECG_TRY(ratio, reader.Quantity<RatioTag>("reserve_floor_ratio"));
  reserve.current_reserve_kw = current;
  reserve.reserve_floor_kw = floor;
  reserve.capacity_kw = capacity;
  reserve.reserve_floor_ratio = ratio;
  return reserve;
}

JsonValue ToJson(const EfficiencyState& efficiency) {
  JsonValue::Object members;
  members.emplace_back("pue", JsonQuantity(efficiency.pue));
  members.emplace_back("design_pue", JsonQuantity(efficiency.design_pue));
  members.emplace_back("measurement_window", ToJson(efficiency.measurement_window));
  if (efficiency.storage_round_trip_ratio.has_value()) {
    members.emplace_back("storage_round_trip_ratio",
                         JsonQuantity(*efficiency.storage_round_trip_ratio));
  }
  return JsonValue::ObjectValue(std::move(members));
}

Result<EfficiencyState> ParseEfficiencyState(const JsonValue& value) {
  Reader reader(value, "evidence.efficiency");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly(
      {"pue", "design_pue", "measurement_window", "storage_round_trip_ratio"}));
  EfficiencyState efficiency;
  ECG_TRY(pue, reader.Quantity<RatioTag>("pue"));
  ECG_TRY(design, reader.Quantity<RatioTag>("design_pue"));
  ECG_TRY(window_value, reader.Require("measurement_window"));
  ECG_TRY(window, ParseInterval(*window_value, "evidence.efficiency.measurement_window"));
  ECG_TRY(round_trip, reader.OptionalQuantity<RatioTag>("storage_round_trip_ratio"));
  efficiency.pue = pue;
  efficiency.design_pue = design;
  efficiency.measurement_window = window;
  efficiency.storage_round_trip_ratio = round_trip;
  return efficiency;
}

// ---------------------------------------------------------------------------
// Remaining evidence payloads
// ---------------------------------------------------------------------------

namespace {

/// Reads a signed integer member that may arrive as a JSON number or a quoted
/// decimal string.
[[nodiscard]] Result<std::int64_t> ParseIntegerMember(const JsonValue& member,
                                                      const std::string& key,
                                                      const Reader* reader) {
  std::string literal;
  if (member.is_number()) {
    literal = member.as_number_text();
  } else if (member.is_string()) {
    literal = member.as_string();
  } else {
    return reader->Err(ErrorCode::kMalformedInput, "member '" + key + "' must be an integer");
  }
  if (literal.empty() || literal.size() > 20) {
    return reader->Err(ErrorCode::kOutOfRange, "member '" + key + "' is not a 64-bit integer");
  }
  std::size_t index = 0;
  bool negative = false;
  if (literal[index] == '-' || literal[index] == '+') {
    negative = literal[index] == '-';
    ++index;
  }
  if (index >= literal.size()) {
    return reader->Err(ErrorCode::kMalformedInput, "member '" + key + "' must be an integer");
  }
  const auto quantity = ParseQuantity<DurationTag>(literal.substr(index));
  if (!quantity.ok()) {
    return reader->Err(ErrorCode::kMalformedInput, "member '" + key + "' must be an integer");
  }
  if (quantity.value().raw() > 9223372036854775807LL - (negative ? 1 : 0)) {
    return reader->Err(ErrorCode::kOutOfRange, "member '" + key + "' does not fit in 64 bits");
  }
  const std::int64_t magnitude = quantity.value().raw();
  return negative ? -magnitude : magnitude;
}

/// Serialises an evidence envelope, or JSON null when the evidence is absent.
template <class Payload, class DumpFn>
[[nodiscard]] JsonValue EnvelopeToJson(const std::optional<Evidence<Payload>>& source,
                                       DumpFn dump) {
  if (!source.has_value()) {
    return JsonValue::Null();
  }
  JsonValue::Object members;
  members.emplace_back("meta", ToJson(source->meta));
  members.emplace_back("value", dump(source->value));
  return JsonValue::ObjectValue(std::move(members));
}

/// Parses an evidence envelope into a target slot. Payload is deduced from the
/// target so the call site does not have to name it twice.
template <class Payload, class ParseFn>
[[nodiscard]] Status EnvelopeFromJson(const JsonValue& value, const std::string& context,
                                      ParseFn parse, std::optional<Evidence<Payload>>* target) {
  Reader reader(value, context);
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"meta", "value"}));
  ECG_TRY(meta_value, reader.Require("meta"));
  ECG_TRY(meta, ParseEvidenceMeta(*meta_value));
  ECG_TRY(payload_value, reader.Require("value"));
  ECG_TRY(payload, parse(*payload_value));
  Evidence<Payload> evidence;
  evidence.meta = meta;
  evidence.value = std::move(payload);
  *target = std::move(evidence);
  return OkStatus();
}

}  // namespace

JsonValue ToJson(const ServiceCatalog& catalog) {
  JsonValue::Array classes;
  classes.reserve(catalog.classes.size());
  for (const ServiceClassTerms& terms : catalog.classes) {
    JsonValue::Object entry;
    entry.emplace_back("id", JsonValue::String(terms.id.value()));
    entry.emplace_back("permission",
                       JsonValue::String(CurtailmentPermissionName(terms.permission)));
    entry.emplace_back("max_curtailment_ratio", JsonQuantity(terms.max_curtailment_ratio));
    entry.emplace_back("max_curtailment_duration_s", JsonQuantity(terms.max_curtailment_duration));
    entry.emplace_back("minimum_notice_s", JsonQuantity(terms.minimum_notice));
    entry.emplace_back("priority", JsonInt(terms.priority));
    entry.emplace_back("nominal_load_kw", JsonQuantity(terms.nominal_load_kw));
    classes.push_back(JsonValue::ObjectValue(std::move(entry)));
  }
  JsonValue::Object members;
  members.emplace_back("classes", JsonValue::ArrayValue(std::move(classes)));
  return JsonValue::ObjectValue(std::move(members));
}

Result<ServiceCatalog> ParseServiceCatalog(const JsonValue& value) {
  Reader reader(value, "evidence.services");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"classes"}));
  ECG_TRY(classes_value, reader.Require("classes"));
  if (!classes_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'classes' must be an array");
  }
  ServiceCatalog catalog;
  for (const JsonValue& entry : classes_value->as_array()) {
    Reader entry_reader(entry, "evidence.services.classes[]");
    ECG_TRY_STATUS(entry_reader.AllowOnly({"id", "permission", "max_curtailment_ratio",
                                           "max_curtailment_duration_s", "minimum_notice_s",
                                           "priority", "nominal_load_kw"}));
    ECG_TRY(id_text, entry_reader.String("id"));
    ECG_TRY(id, ServiceClassId::Parse(id_text));
    ECG_TRY(permission_name, entry_reader.String("permission"));
    ECG_TRY(permission, EnumByName<CurtailmentPermission>(permission_name,
                                                          CurtailmentPermissionName,
                                                          kCurtailmentPermissions,
                                                          "curtailment_permission"));
    ECG_TRY(ratio, entry_reader.Quantity<RatioTag>("max_curtailment_ratio"));
    ECG_TRY(max_duration, entry_reader.Quantity<DurationTag>("max_curtailment_duration_s"));
    ECG_TRY(notice, entry_reader.Quantity<DurationTag>("minimum_notice_s"));
    ECG_TRY(priority_raw, entry_reader.Require("priority"));
    std::int64_t priority = 0;
    {
      std::string literal = priority_raw->is_number() ? priority_raw->as_number_text()
                            : priority_raw->is_string() ? priority_raw->as_string()
                                                        : std::string();
      if (literal.empty() || literal.size() > 11) {
        return entry_reader.Err(ErrorCode::kMalformedInput, "member 'priority' must be an integer");
      }
      std::size_t index = 0;
      bool negative = false;
      if (literal[index] == '-') {
        negative = true;
        ++index;
      }
      if (index >= literal.size()) {
        return entry_reader.Err(ErrorCode::kMalformedInput, "member 'priority' must be an integer");
      }
      std::int64_t magnitude = 0;
      for (; index < literal.size(); ++index) {
        const char c = literal[index];
        if (c < '0' || c > '9') {
          return entry_reader.Err(ErrorCode::kMalformedInput,
                                  "member 'priority' must be an integer");
        }
        magnitude = magnitude * 10 + (c - '0');
      }
      priority = negative ? -magnitude : magnitude;
    }
    ECG_TRY(nominal, entry_reader.Quantity<PowerKwTag>("nominal_load_kw"));

    ServiceClassTerms terms;
    terms.id = id;
    terms.permission = permission;
    terms.max_curtailment_ratio = ratio;
    terms.max_curtailment_duration = max_duration;
    terms.minimum_notice = notice;
    terms.priority = static_cast<std::int32_t>(priority);
    terms.nominal_load_kw = nominal;
    catalog.classes.push_back(std::move(terms));
    if (catalog.classes.size() > kMaxServiceClasses) {
      return reader.Err(ErrorCode::kResourceLimitExceeded, "more service classes than the bound");
    }
  }
  return catalog;
}

JsonValue ToJson(const CapacityState& capacity) {
  JsonValue::Object members;
  members.emplace_back("flexible_load_kw", JsonQuantity(capacity.flexible_load_kw));
  members.emplace_back("shiftable_headroom_kw", JsonQuantity(capacity.shiftable_headroom_kw));
  members.emplace_back("storage_charge_kw", JsonQuantity(capacity.storage_charge_kw));
  members.emplace_back("storage_discharge_kw", JsonQuantity(capacity.storage_discharge_kw));
  members.emplace_back("storage_usable_energy_kwh", JsonQuantity(capacity.storage_usable_energy_kwh));
  return JsonValue::ObjectValue(std::move(members));
}

Result<CapacityState> ParseCapacityState(const JsonValue& value) {
  Reader reader(value, "evidence.capacity");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"flexible_load_kw", "shiftable_headroom_kw", "storage_charge_kw",
                                   "storage_discharge_kw", "storage_usable_energy_kwh"}));
  CapacityState capacity;
  ECG_TRY(flexible, reader.Quantity<PowerKwTag>("flexible_load_kw"));
  ECG_TRY(headroom, reader.Quantity<PowerKwTag>("shiftable_headroom_kw"));
  ECG_TRY(charge, reader.Quantity<PowerKwTag>("storage_charge_kw"));
  ECG_TRY(discharge, reader.Quantity<PowerKwTag>("storage_discharge_kw"));
  ECG_TRY(energy, reader.Quantity<EnergyKwhTag>("storage_usable_energy_kwh"));
  capacity.flexible_load_kw = flexible;
  capacity.shiftable_headroom_kw = headroom;
  capacity.storage_charge_kw = charge;
  capacity.storage_discharge_kw = discharge;
  capacity.storage_usable_energy_kwh = energy;
  return capacity;
}

JsonValue ToJson(const RiskState& risk) {
  JsonValue::Object members;
  members.emplace_back("posture", JsonValue::String(RiskPostureName(risk.posture)));
  if (risk.max_ramp_kw_per_min.has_value()) {
    members.emplace_back("max_ramp_kw_per_min", JsonQuantity(*risk.max_ramp_kw_per_min));
  }
  if (risk.max_deferral.has_value()) {
    members.emplace_back("max_deferral_s", JsonQuantity(*risk.max_deferral));
  }
  if (risk.minimum_dwell.has_value()) {
    members.emplace_back("minimum_dwell_s", JsonQuantity(*risk.minimum_dwell));
  }
  members.emplace_back("last_change_at", ToJson(risk.last_change_at));
  return JsonValue::ObjectValue(std::move(members));
}

Result<RiskState> ParseRiskState(const JsonValue& value) {
  Reader reader(value, "evidence.risk");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly(
      {"posture", "max_ramp_kw_per_min", "max_deferral_s", "minimum_dwell_s", "last_change_at"}));
  ECG_TRY(posture_name, reader.OptionalString("posture", "normal"));
  ECG_TRY(posture, EnumByName<RiskPosture>(posture_name, RiskPostureName, kRiskPostures, "risk_posture"));
  ECG_TRY(ramp, reader.OptionalQuantity<PowerRateTag>("max_ramp_kw_per_min"));
  ECG_TRY(deferral, reader.OptionalQuantity<DurationTag>("max_deferral_s"));
  ECG_TRY(dwell, reader.OptionalQuantity<DurationTag>("minimum_dwell_s"));
  ECG_TRY(last_change, reader.OptionalInstant("last_change_at", UtcInstant::FromRaw(0)));
  RiskState risk;
  risk.posture = posture;
  risk.max_ramp_kw_per_min = ramp;
  risk.max_deferral = deferral;
  risk.minimum_dwell = dwell;
  risk.last_change_at = last_change;
  return risk;
}

JsonValue ToJson(const IncidentState& incident) {
  JsonValue::Object members;
  members.emplace_back("severity", JsonValue::String(IncidentSeverityName(incident.severity)));
  JsonValue::Array affected;
  affected.reserve(incident.affected_classes.size());
  for (const ServiceClassId& id : incident.affected_classes) {
    affected.push_back(JsonValue::String(id.value()));
  }
  members.emplace_back("affected_classes", JsonValue::ArrayValue(std::move(affected)));
  members.emplace_back("declared_at", ToJson(incident.declared_at));
  return JsonValue::ObjectValue(std::move(members));
}

Result<IncidentState> ParseIncidentState(const JsonValue& value) {
  Reader reader(value, "evidence.incident");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"severity", "affected_classes", "declared_at"}));
  ECG_TRY(severity_name, reader.OptionalString("severity", "none"));
  ECG_TRY(severity, EnumByName<IncidentSeverity>(severity_name, IncidentSeverityName,
                                                 kIncidentSeverities, "incident_severity"));
  ECG_TRY(affected_value, reader.Require("affected_classes"));
  if (!affected_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'affected_classes' must be an array");
  }
  IncidentState incident;
  incident.severity = severity;
  for (const JsonValue& entry : affected_value->as_array()) {
    if (!entry.is_string()) {
      return reader.Err(ErrorCode::kMalformedInput, "'affected_classes' entries must be strings");
    }
    ECG_TRY(id, ServiceClassId::Parse(entry.as_string()));
    incident.affected_classes.push_back(id);
    if (incident.affected_classes.size() > kMaxAffectedClasses) {
      return reader.Err(ErrorCode::kResourceLimitExceeded, "more affected classes than the bound");
    }
  }
  ECG_TRY(declared_at, reader.OptionalInstant("declared_at", UtcInstant::FromRaw(0)));
  incident.declared_at = declared_at;
  return incident;
}

JsonValue ToJson(const ZoneRules& zone) {
  JsonValue::Array transitions;
  transitions.reserve(zone.transitions().size());
  for (const ZoneTransition& transition : zone.transitions()) {
    JsonValue::Object entry;
    entry.emplace_back("at", ToJson(transition.at));
    entry.emplace_back("offset_seconds", JsonInt(transition.offset_seconds));
    transitions.push_back(JsonValue::ObjectValue(std::move(entry)));
  }
  JsonValue::Object members;
  members.emplace_back("id", JsonValue::String(zone.id().value()));
  members.emplace_back("initial_offset_seconds", JsonInt(zone.initial_offset_seconds()));
  members.emplace_back("transitions", JsonValue::ArrayValue(std::move(transitions)));
  return JsonValue::ObjectValue(std::move(members));
}

Result<ZoneRules> ParseZoneRules(const JsonValue& value) {
  Reader reader(value, "evidence.zone");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"id", "initial_offset_seconds", "transitions"}));
  ECG_TRY(id_text, reader.OptionalString("id", "unnamed"));
  ECG_TRY(id, ZoneId::Parse(id_text));
  ECG_TRY(initial_raw, reader.Require("initial_offset_seconds"));
  ECG_TRY(initial, ParseIntegerMember(*initial_raw, "initial_offset_seconds", &reader));
  ECG_TRY(transitions_value, reader.Require("transitions"));
  if (!transitions_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'transitions' must be an array");
  }
  std::vector<ZoneTransition> transitions;
  for (const JsonValue& entry : transitions_value->as_array()) {
    Reader entry_reader(entry, "evidence.zone.transitions[]");
    ECG_TRY_STATUS(entry_reader.AllowOnly({"at", "offset_seconds"}));
    ECG_TRY(at, entry_reader.Instant("at"));
    ECG_TRY(offset_raw, entry_reader.Require("offset_seconds"));
    ECG_TRY(offset, ParseIntegerMember(*offset_raw, "offset_seconds", &entry_reader));
    ZoneTransition transition;
    transition.at = at;
    transition.offset_seconds = static_cast<std::int32_t>(offset);
    transitions.push_back(transition);
  }
  const auto rules = ZoneRules::Make(id, static_cast<std::int32_t>(initial), std::move(transitions));
  if (!rules.ok()) {
    return reader.Err(rules.error().code(), rules.error().message());
  }
  return rules.value();
}

JsonValue ToJson(const EvidenceSet& evidence) {
  JsonValue::Object members;
  members.emplace_back("epoch", JsonUint(evidence.epoch.raw()));
  members.emplace_back("tariff", EnvelopeToJson(evidence.tariff, [](const Tariff& v) { return ToJson(v); }));
  members.emplace_back("demand",
                       EnvelopeToJson(evidence.demand, [](const DemandObservation& v) { return ToJson(v); }));
  members.emplace_back("reserve",
                       EnvelopeToJson(evidence.reserve, [](const ReserveState& v) { return ToJson(v); }));
  members.emplace_back("efficiency",
                       EnvelopeToJson(evidence.efficiency, [](const EfficiencyState& v) { return ToJson(v); }));
  members.emplace_back("services",
                       EnvelopeToJson(evidence.services, [](const ServiceCatalog& v) { return ToJson(v); }));
  members.emplace_back("capacity",
                       EnvelopeToJson(evidence.capacity, [](const CapacityState& v) { return ToJson(v); }));
  members.emplace_back("risk", EnvelopeToJson(evidence.risk, [](const RiskState& v) { return ToJson(v); }));
  members.emplace_back("incident",
                       EnvelopeToJson(evidence.incident, [](const IncidentState& v) { return ToJson(v); }));
  members.emplace_back("zone", EnvelopeToJson(evidence.zone, [](const ZoneRules& v) { return ToJson(v); }));
  return JsonValue::ObjectValue(std::move(members));
}

Result<EvidenceSet> ParseEvidenceSet(const JsonValue& value) {
  Reader reader(value, "evidence");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"epoch", "tariff", "demand", "reserve", "efficiency", "services",
                                   "capacity", "risk", "incident", "zone"}));
  ECG_TRY(epoch, reader.Uint("epoch"));
  EvidenceSet evidence;
  evidence.epoch = Epoch::FromRaw(epoch);

  const auto parse_slot = [&](std::string_view key, auto* target, auto parse_fn) -> Status {
    ECG_TRY(member, reader.Optional(key));
    if (member == nullptr) {
      return OkStatus();
    }
    if (member->is_null()) {
      return OkStatus();
    }
    return EnvelopeFromJson(*member, "evidence." + std::string(key), parse_fn, target);
  };

  ECG_TRY_STATUS(parse_slot("tariff", &evidence.tariff,
                            [](const JsonValue& v) { return ParseTariff(v); }));
  ECG_TRY_STATUS(parse_slot("demand", &evidence.demand,
                            [](const JsonValue& v) { return ParseDemandObservation(v); }));
  ECG_TRY_STATUS(parse_slot("reserve", &evidence.reserve,
                            [](const JsonValue& v) { return ParseReserveState(v); }));
  ECG_TRY_STATUS(parse_slot("efficiency", &evidence.efficiency,
                            [](const JsonValue& v) { return ParseEfficiencyState(v); }));
  ECG_TRY_STATUS(parse_slot("services", &evidence.services,
                            [](const JsonValue& v) { return ParseServiceCatalog(v); }));
  ECG_TRY_STATUS(parse_slot("capacity", &evidence.capacity,
                            [](const JsonValue& v) { return ParseCapacityState(v); }));
  ECG_TRY_STATUS(parse_slot("risk", &evidence.risk,
                            [](const JsonValue& v) { return ParseRiskState(v); }));
  ECG_TRY_STATUS(parse_slot("incident", &evidence.incident,
                            [](const JsonValue& v) { return ParseIncidentState(v); }));
  ECG_TRY_STATUS(parse_slot("zone", &evidence.zone,
                            [](const JsonValue& v) { return ParseZoneRules(v); }));
  return evidence;
}

// ---------------------------------------------------------------------------
// Policy, requests, and decision components
// ---------------------------------------------------------------------------

JsonValue ToJson(const PolicySet& policy) {
  JsonValue::Object limits;
  limits.emplace_back("tariff_max_age_s", JsonQuantity(policy.limits.tariff_max_age));
  limits.emplace_back("demand_max_age_s", JsonQuantity(policy.limits.demand_max_age));
  limits.emplace_back("reserve_max_age_s", JsonQuantity(policy.limits.reserve_max_age));
  limits.emplace_back("efficiency_max_age_s", JsonQuantity(policy.limits.efficiency_max_age));
  limits.emplace_back("services_max_age_s", JsonQuantity(policy.limits.services_max_age));
  limits.emplace_back("capacity_max_age_s", JsonQuantity(policy.limits.capacity_max_age));
  limits.emplace_back("risk_max_age_s", JsonQuantity(policy.limits.risk_max_age));
  limits.emplace_back("incident_max_age_s", JsonQuantity(policy.limits.incident_max_age));
  limits.emplace_back("zone_max_age_s", JsonQuantity(policy.limits.zone_max_age));
  limits.emplace_back("future_skew_s", JsonQuantity(policy.limits.future_skew));

  JsonValue::Object members;
  members.emplace_back("id", JsonValue::String(policy.id.value()));
  members.emplace_back("generation", JsonUint(policy.generation.raw()));
  members.emplace_back("epoch", JsonUint(policy.epoch.raw()));
  members.emplace_back("evidence_generation", JsonUint(policy.evidence_generation.raw()));
  members.emplace_back("limits", JsonValue::ObjectValue(std::move(limits)));
  members.emplace_back("decision_horizon_s", JsonQuantity(policy.decision_horizon));
  members.emplace_back("reaction_lead_time_s", JsonQuantity(policy.reaction_lead_time));
  members.emplace_back("shift_search_horizon_s", JsonQuantity(policy.shift_search_horizon));
  members.emplace_back("incident_recheck_interval_s", JsonQuantity(policy.incident_recheck_interval));
  members.emplace_back("max_intent_magnitude_kw", JsonQuantity(policy.max_intent_magnitude_kw));
  members.emplace_back("max_intent_duration_s", JsonQuantity(policy.max_intent_duration));
  members.emplace_back("reserve_safety_margin", JsonQuantity(policy.reserve_safety_margin));
  members.emplace_back("minimum_economic_benefit", JsonQuantity(policy.minimum_economic_benefit));
  members.emplace_back("require_positive_economic_benefit",
                       JsonBool(policy.require_positive_economic_benefit));
  members.emplace_back("prohibit_demand_charge_increase",
                       JsonBool(policy.prohibit_demand_charge_increase));
  members.emplace_back("defer_early_demand_requests",
                       JsonBool(policy.defer_early_demand_requests));
  members.emplace_back("max_service_curtailment_ratio",
                       JsonQuantity(policy.max_service_curtailment_ratio));
  members.emplace_back("max_reasons", JsonUint(policy.max_reasons));
  members.emplace_back("max_price_slices", JsonUint(policy.max_price_slices));
  return JsonValue::ObjectValue(std::move(members));
}

Result<PolicySet> ParsePolicySet(const JsonValue& value) {
  Reader reader(value, "policy");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"id", "generation", "epoch", "evidence_generation", "limits",
                                   "decision_horizon_s", "reaction_lead_time_s",
                                   "shift_search_horizon_s", "incident_recheck_interval_s",
                                   "max_intent_magnitude_kw", "max_intent_duration_s",
                                   "reserve_safety_margin", "minimum_economic_benefit",
                                   "require_positive_economic_benefit",
                                   "prohibit_demand_charge_increase",
                                   "defer_early_demand_requests", "max_service_curtailment_ratio",
                                   "max_reasons", "max_price_slices"}));
  PolicySet policy = DefaultPolicy();

  ECG_TRY(id_text, reader.OptionalString("id", policy.id.value()));
  ECG_TRY(id, PolicyId::Parse(id_text));
  policy.id = id;
  ECG_TRY(generation, reader.OptionalUint("generation", policy.generation.raw()));
  policy.generation = PolicyGeneration::FromRaw(generation);
  ECG_TRY(epoch, reader.OptionalUint("epoch", policy.epoch.raw()));
  policy.epoch = Epoch::FromRaw(epoch);
  ECG_TRY(evidence_generation, reader.OptionalUint("evidence_generation",
                                                   policy.evidence_generation.raw()));
  policy.evidence_generation = EvidenceGeneration::FromRaw(evidence_generation);

  ECG_TRY(limits_member, reader.Optional("limits"));
  if (limits_member != nullptr) {
    Reader limits(*limits_member, "policy.limits");
    ECG_TRY_STATUS(limits.ExpectObject());
    ECG_TRY_STATUS(limits.AllowOnly({"tariff_max_age_s", "demand_max_age_s", "reserve_max_age_s",
                                     "efficiency_max_age_s", "services_max_age_s",
                                     "capacity_max_age_s", "risk_max_age_s", "incident_max_age_s",
                                     "zone_max_age_s", "future_skew_s"}));
    ECG_TRY(v, limits.OptionalQuantity<DurationTag>("tariff_max_age_s"));
    if (v.has_value()) policy.limits.tariff_max_age = *v;
    ECG_TRY(v2, limits.OptionalQuantity<DurationTag>("demand_max_age_s"));
    if (v2.has_value()) policy.limits.demand_max_age = *v2;
    ECG_TRY(v3, limits.OptionalQuantity<DurationTag>("reserve_max_age_s"));
    if (v3.has_value()) policy.limits.reserve_max_age = *v3;
    ECG_TRY(v4, limits.OptionalQuantity<DurationTag>("efficiency_max_age_s"));
    if (v4.has_value()) policy.limits.efficiency_max_age = *v4;
    ECG_TRY(v5, limits.OptionalQuantity<DurationTag>("services_max_age_s"));
    if (v5.has_value()) policy.limits.services_max_age = *v5;
    ECG_TRY(v6, limits.OptionalQuantity<DurationTag>("capacity_max_age_s"));
    if (v6.has_value()) policy.limits.capacity_max_age = *v6;
    ECG_TRY(v7, limits.OptionalQuantity<DurationTag>("risk_max_age_s"));
    if (v7.has_value()) policy.limits.risk_max_age = *v7;
    ECG_TRY(v8, limits.OptionalQuantity<DurationTag>("incident_max_age_s"));
    if (v8.has_value()) policy.limits.incident_max_age = *v8;
    ECG_TRY(v9, limits.OptionalQuantity<DurationTag>("zone_max_age_s"));
    if (v9.has_value()) policy.limits.zone_max_age = *v9;
    ECG_TRY(v10, limits.OptionalQuantity<DurationTag>("future_skew_s"));
    if (v10.has_value()) policy.limits.future_skew = *v10;
  }

  ECG_TRY(a, reader.OptionalQuantity<DurationTag>("decision_horizon_s"));
  if (a.has_value()) policy.decision_horizon = *a;
  ECG_TRY(b, reader.OptionalQuantity<DurationTag>("reaction_lead_time_s"));
  if (b.has_value()) policy.reaction_lead_time = *b;
  ECG_TRY(c, reader.OptionalQuantity<DurationTag>("shift_search_horizon_s"));
  if (c.has_value()) policy.shift_search_horizon = *c;
  ECG_TRY(d, reader.OptionalQuantity<DurationTag>("incident_recheck_interval_s"));
  if (d.has_value()) policy.incident_recheck_interval = *d;
  ECG_TRY(e, reader.OptionalQuantity<PowerKwTag>("max_intent_magnitude_kw"));
  if (e.has_value()) policy.max_intent_magnitude_kw = *e;
  ECG_TRY(f, reader.OptionalQuantity<DurationTag>("max_intent_duration_s"));
  if (f.has_value()) policy.max_intent_duration = *f;
  ECG_TRY(g, reader.OptionalQuantity<RatioTag>("reserve_safety_margin"));
  if (g.has_value()) policy.reserve_safety_margin = *g;
  ECG_TRY(h, reader.OptionalQuantity<MoneyTag>("minimum_economic_benefit"));
  if (h.has_value()) policy.minimum_economic_benefit = *h;
  ECG_TRY(i, reader.OptionalBool("require_positive_economic_benefit",
                                 policy.require_positive_economic_benefit));
  policy.require_positive_economic_benefit = i;
  ECG_TRY(j, reader.OptionalBool("prohibit_demand_charge_increase",
                                 policy.prohibit_demand_charge_increase));
  policy.prohibit_demand_charge_increase = j;
  ECG_TRY(k, reader.OptionalBool("defer_early_demand_requests",
                                 policy.defer_early_demand_requests));
  policy.defer_early_demand_requests = k;
  ECG_TRY(l, reader.OptionalQuantity<RatioTag>("max_service_curtailment_ratio"));
  if (l.has_value()) policy.max_service_curtailment_ratio = *l;
  ECG_TRY(m, reader.OptionalUint("max_reasons", policy.max_reasons));
  policy.max_reasons = static_cast<std::size_t>(m);
  ECG_TRY(n, reader.OptionalUint("max_price_slices", policy.max_price_slices));
  policy.max_price_slices = static_cast<std::size_t>(n);

  const Status status = ValidatePolicy(policy);
  if (!status.ok()) {
    return reader.Err(status.error().code(), status.error().message());
  }
  return policy;
}

JsonValue ToJson(const DecisionRequest& request) {
  JsonValue::Object members;
  members.emplace_back("request_id", JsonValue::String(request.request_id.ToString()));
  members.emplace_back("client_id", JsonValue::String(request.client_id.value()));
  members.emplace_back("epoch", JsonUint(request.epoch.raw()));
  members.emplace_back("policy_generation", JsonUint(request.policy_generation.raw()));
  members.emplace_back("kind", JsonValue::String(RequestKindName(request.kind)));
  members.emplace_back("service_class", JsonValue::String(request.service_class.value()));
  members.emplace_back("magnitude_kw", JsonQuantity(request.magnitude_kw));
  members.emplace_back("duration_s", JsonQuantity(request.duration));
  members.emplace_back("window", ToJson(request.desired_window));
  members.emplace_back("rationale", JsonValue::String(RationaleName(request.rationale)));
  members.emplace_back("requested_at", ToJson(request.requested_at));
  members.emplace_back("attempt", JsonUint(request.attempt));
  return JsonValue::ObjectValue(std::move(members));
}

Result<DecisionRequest> ParseDecisionRequest(const JsonValue& value) {
  Reader reader(value, "request");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"request_id", "client_id", "epoch", "policy_generation", "kind",
                                   "service_class", "magnitude_kw", "duration_s", "window",
                                   "rationale", "requested_at", "attempt"}));
  ECG_TRY(request_id_text, reader.String("request_id"));
  ECG_TRY(request_id, RequestId::Parse(request_id_text));
  ECG_TRY(client_text, reader.String("client_id"));
  ECG_TRY(client, ClientId::Parse(client_text));
  ECG_TRY(epoch, reader.Uint("epoch"));
  ECG_TRY(policy_generation, reader.Uint("policy_generation"));
  ECG_TRY(kind_name, reader.String("kind"));
  ECG_TRY(kind, EnumByName<RequestKind>(kind_name, RequestKindName, kRequestKinds, "request_kind"));
  ECG_TRY(service_text, reader.OptionalString("service_class", ""));
  ECG_TRY(magnitude, reader.Quantity<PowerKwTag>("magnitude_kw"));
  ECG_TRY(duration, reader.Quantity<DurationTag>("duration_s"));
  ECG_TRY(window_value, reader.Require("window"));
  ECG_TRY(window, ParseInterval(*window_value, "request.window"));
  ECG_TRY(rationale_name, reader.OptionalString("rationale", "price_arbitrage"));
  ECG_TRY(rationale, EnumByName<Rationale>(rationale_name, RationaleName, kRationales, "rationale"));
  ECG_TRY(requested_at, reader.OptionalInstant("requested_at", UtcInstant::FromRaw(0)));
  ECG_TRY(attempt, reader.OptionalUint("attempt", 0));

  DecisionRequest request;
  request.request_id = request_id;
  request.client_id = client;
  request.epoch = Epoch::FromRaw(epoch);
  request.policy_generation = PolicyGeneration::FromRaw(policy_generation);
  request.kind = kind;
  if (!service_text.empty()) {
    ECG_TRY(service, ServiceClassId::Parse(service_text));
    request.service_class = service;
  }
  request.magnitude_kw = magnitude;
  request.duration = duration;
  request.desired_window = window;
  request.rationale = rationale;
  request.requested_at = requested_at;
  request.attempt = static_cast<std::uint32_t>(attempt);
  return request;
}

JsonValue ToJson(const EvidenceRef& ref) {
  JsonValue::Object members;
  members.emplace_back("authority", JsonValue::String(AuthorityKindName(ref.authority)));
  members.emplace_back("source", JsonValue::String(ref.source.value()));
  members.emplace_back("generation", JsonUint(ref.generation.raw()));
  members.emplace_back("observed_at", ToJson(ref.observed_at));
  members.emplace_back("freshness", JsonValue::String(FreshnessName(ref.freshness)));
  return JsonValue::ObjectValue(std::move(members));
}

Result<EvidenceRef> ParseEvidenceRef(const JsonValue& value) {
  Reader reader(value, "evidence_ref");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"authority", "source", "generation", "observed_at", "freshness"}));
  ECG_TRY(authority_name, reader.String("authority"));
  ECG_TRY(authority, EnumByName<AuthorityKind>(authority_name, AuthorityKindName, kAuthorityKinds,
                                               "authority_kind"));
  ECG_TRY(source_text, reader.String("source"));
  ECG_TRY(source, SourceId::Parse(source_text));
  ECG_TRY(generation, reader.Uint("generation"));
  ECG_TRY(observed_at, reader.Instant("observed_at"));
  ECG_TRY(freshness_name, reader.OptionalString("freshness", "missing"));
  EvidenceRef ref;
  ref.authority = authority;
  ref.source = source;
  ref.generation = EvidenceGeneration::FromRaw(generation);
  ref.observed_at = observed_at;
  if (freshness_name == "fresh") ref.freshness = Freshness::kFresh;
  else if (freshness_name == "stale") ref.freshness = Freshness::kStale;
  else if (freshness_name == "missing") ref.freshness = Freshness::kMissing;
  else if (freshness_name == "future_dated") ref.freshness = Freshness::kFutureDated;
  else if (freshness_name == "recovered") ref.freshness = Freshness::kRecovered;
  else if (freshness_name == "epoch_mismatch") ref.freshness = Freshness::kEpochMismatch;
  else if (freshness_name == "unsupported_version") ref.freshness = Freshness::kUnsupportedVersion;
  else if (freshness_name == "conflicting") ref.freshness = Freshness::kConflicting;
  else {
    return reader.Err(ErrorCode::kUnsupportedValue,
                      "unknown freshness '" + freshness_name + "'");
  }
  return ref;
}

JsonValue ToJson(const Reason& reason) {
  JsonValue::Object members;
  members.emplace_back("code", JsonValue::String(ReasonCodeName(reason.code)));
  members.emplace_back("class", JsonValue::String(
      reason.severity == ReasonSeverity::kInfo      ? "supporting"
      : reason.severity == ReasonSeverity::kNotice  ? "deferred"
      : ClassifyReasonCode(reason.code) == ReasonClass::kRefused ? "refused" : "indeterminate"));
  members.emplace_back("severity", JsonValue::String(ReasonSeverityName(reason.severity)));
  members.emplace_back("subject", JsonValue::String(reason.subject));
  members.emplace_back("detail", JsonValue::String(reason.detail));
  if (reason.evidence.has_value()) {
    members.emplace_back("evidence", ToJson(*reason.evidence));
  }
  return JsonValue::ObjectValue(std::move(members));
}

Result<Reason> ParseReason(const JsonValue& value) {
  Reader reader(value, "reason");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"code", "class", "severity", "subject", "detail", "evidence"}));
  ECG_TRY(code_name, reader.String("code"));
  ECG_TRY(code, EnumByName<ReasonCode>(code_name, ReasonCodeName, kReasonCodes, "reason_code"));
  ECG_TRY(severity_name, reader.OptionalString("severity", "info"));
  ECG_TRY(severity, EnumByName<ReasonSeverity>(severity_name, ReasonSeverityName, kSeverities,
                                               "reason_severity"));
  ECG_TRY(subject, reader.String("subject"));
  ECG_TRY(detail, reader.String("detail"));
  ECG_TRY(evidence_member, reader.Optional("evidence"));
  Reason reason;
  reason.code = code;
  reason.severity = severity;
  reason.subject = subject;
  reason.detail = detail;
  if (evidence_member != nullptr) {
    ECG_TRY(evidence, ParseEvidenceRef(*evidence_member));
    reason.evidence = evidence;
  }
  return reason;
}

JsonValue ToJson(const BoundedIntent& intent) {
  JsonValue::Object members;
  members.emplace_back("id", JsonValue::String(intent.id.ToString()));
  members.emplace_back("kind", JsonValue::String(RequestKindName(intent.kind)));
  members.emplace_back("service_class", JsonValue::String(intent.service_class.value()));
  members.emplace_back("magnitude_limit_kw", JsonQuantity(intent.magnitude_limit_kw));
  members.emplace_back("duration_limit_s", JsonQuantity(intent.duration_limit));
  members.emplace_back("window", ToJson(intent.window));
  members.emplace_back("expires_at", ToJson(intent.expires_at));
  members.emplace_back("target_authority",
                       JsonValue::String(AuthorityKindName(intent.target_authority)));
  members.emplace_back("advisory_only", JsonBool(intent.advisory_only));
  return JsonValue::ObjectValue(std::move(members));
}

Result<BoundedIntent> ParseBoundedIntent(const JsonValue& value) {
  Reader reader(value, "decision.intent");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"id", "kind", "service_class", "magnitude_limit_kw",
                                   "duration_limit_s", "window", "expires_at", "target_authority",
                                   "advisory_only"}));
  ECG_TRY(id_text, reader.String("id"));
  ECG_TRY(id, IntentId::Parse(id_text));
  ECG_TRY(kind_name, reader.String("kind"));
  ECG_TRY(kind, EnumByName<RequestKind>(kind_name, RequestKindName, kRequestKinds, "request_kind"));
  ECG_TRY(service_text, reader.OptionalString("service_class", ""));
  ECG_TRY(magnitude, reader.Quantity<PowerKwTag>("magnitude_limit_kw"));
  ECG_TRY(duration, reader.Quantity<DurationTag>("duration_limit_s"));
  ECG_TRY(window_value, reader.Require("window"));
  ECG_TRY(window, ParseInterval(*window_value, "decision.intent.window"));
  ECG_TRY(expires_at, reader.Instant("expires_at"));
  ECG_TRY(authority_name, reader.String("target_authority"));
  ECG_TRY(authority, EnumByName<AuthorityKind>(authority_name, AuthorityKindName, kAuthorityKinds,
                                               "authority_kind"));
  ECG_TRY(advisory, reader.OptionalBool("advisory_only", true));
  BoundedIntent intent;
  intent.id = id;
  intent.kind = kind;
  if (!service_text.empty()) {
    ECG_TRY(service, ServiceClassId::Parse(service_text));
    intent.service_class = service;
  }
  intent.magnitude_limit_kw = magnitude;
  intent.duration_limit = duration;
  intent.window = window;
  intent.expires_at = expires_at;
  intent.target_authority = authority;
  intent.advisory_only = advisory;
  return intent;
}

JsonValue ToJson(const EconomicAssessment& economics) {
  JsonValue::Object members;
  members.emplace_back("evaluated", JsonBool(economics.evaluated));
  members.emplace_back("baseline_cost", JsonQuantity(economics.baseline_cost));
  members.emplace_back("candidate_cost", JsonQuantity(economics.candidate_cost));
  members.emplace_back("gross_savings", JsonQuantity(economics.gross_savings));
  members.emplace_back("demand_charge_avoided", JsonQuantity(economics.demand_charge_avoided));
  members.emplace_back("baseline_price", JsonQuantity(economics.baseline_price));
  members.emplace_back("candidate_price", JsonQuantity(economics.candidate_price));
  if (economics.shift_target.has_value()) {
    members.emplace_back("shift_target", ToJson(*economics.shift_target));
  }
  members.emplace_back("price_slices", JsonUint(economics.price_slices));
  members.emplace_back("favorable", JsonBool(economics.favorable));
  return JsonValue::ObjectValue(std::move(members));
}

Result<EconomicAssessment> ParseEconomicAssessment(const JsonValue& value) {
  Reader reader(value, "decision.economics");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"evaluated", "baseline_cost", "candidate_cost", "gross_savings",
                                   "demand_charge_avoided", "baseline_price", "candidate_price",
                                   "shift_target", "price_slices", "favorable"}));
  EconomicAssessment economics;
  ECG_TRY(evaluated, reader.Bool("evaluated"));
  ECG_TRY(baseline, reader.Quantity<MoneyTag>("baseline_cost"));
  ECG_TRY(candidate, reader.Quantity<MoneyTag>("candidate_cost"));
  ECG_TRY(savings, reader.Quantity<MoneyTag>("gross_savings"));
  ECG_TRY(avoided, reader.Quantity<MoneyTag>("demand_charge_avoided"));
  ECG_TRY(baseline_price, reader.Quantity<PriceTag>("baseline_price"));
  ECG_TRY(candidate_price, reader.Quantity<PriceTag>("candidate_price"));
  ECG_TRY(target_member, reader.Optional("shift_target"));
  ECG_TRY(slices, reader.Uint("price_slices"));
  ECG_TRY(favorable, reader.Bool("favorable"));
  economics.evaluated = evaluated;
  economics.baseline_cost = baseline;
  economics.candidate_cost = candidate;
  economics.gross_savings = savings;
  economics.demand_charge_avoided = avoided;
  economics.baseline_price = baseline_price;
  economics.candidate_price = candidate_price;
  if (target_member != nullptr) {
    ECG_TRY(target, ParseInterval(*target_member, "decision.economics.shift_target"));
    economics.shift_target = target;
  }
  economics.price_slices = static_cast<std::uint32_t>(slices);
  economics.favorable = favorable;
  return economics;
}

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

JsonValue ToJson(const Decision& decision) {
  JsonValue::Array reasons;
  reasons.reserve(decision.reasons.size());
  for (const Reason& reason : decision.reasons) {
    reasons.push_back(ToJson(reason));
  }
  JsonValue::Array dependencies;
  dependencies.reserve(decision.evidence_dependencies.size());
  for (const EvidenceRef& ref : decision.evidence_dependencies) {
    dependencies.push_back(ToJson(ref));
  }

  JsonValue::Object members;
  members.emplace_back("id", JsonValue::String(decision.id.ToString()));
  members.emplace_back("request_id", JsonValue::String(decision.request_id.ToString()));
  members.emplace_back("client_id", JsonValue::String(decision.client_id.value()));
  members.emplace_back("epoch", JsonUint(decision.epoch.raw()));
  members.emplace_back("policy_id", JsonValue::String(decision.policy_id.value()));
  members.emplace_back("policy_generation", JsonUint(decision.policy_generation.raw()));
  members.emplace_back("kind", JsonValue::String(RequestKindName(decision.kind)));
  members.emplace_back("service_class", JsonValue::String(decision.service_class.value()));
  members.emplace_back("outcome", JsonValue::String(OutcomeName(decision.outcome)));
  members.emplace_back("reasons", JsonValue::ArrayValue(std::move(reasons)));
  members.emplace_back("evidence_dependencies", JsonValue::ArrayValue(std::move(dependencies)));
  if (decision.intent.has_value()) {
    members.emplace_back("intent", ToJson(*decision.intent));
  }
  if (decision.reconsider_at.has_value()) {
    members.emplace_back("reconsider_at", ToJson(*decision.reconsider_at));
  }
  members.emplace_back("economics", ToJson(decision.economics));
  members.emplace_back("digest", JsonValue::String(decision.digest.Hex()));
  members.emplace_back("decided_at", ToJson(decision.decided_at));
  return JsonValue::ObjectValue(std::move(members));
}

Result<Decision> ParseDecision(const JsonValue& value) {
  Reader reader(value, "decision");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"id", "request_id", "client_id", "epoch", "policy_id",
                                   "policy_generation", "kind", "service_class", "outcome",
                                   "reasons", "evidence_dependencies", "intent", "reconsider_at",
                                   "economics", "digest", "decided_at"}));
  ECG_TRY(id_text, reader.String("id"));
  ECG_TRY(id, DecisionId::Parse(id_text));
  ECG_TRY(request_id_text, reader.String("request_id"));
  ECG_TRY(request_id, RequestId::Parse(request_id_text));
  ECG_TRY(client_text, reader.String("client_id"));
  ECG_TRY(client, ClientId::Parse(client_text));
  ECG_TRY(epoch, reader.Uint("epoch"));
  ECG_TRY(policy_id_text, reader.String("policy_id"));
  ECG_TRY(policy_id, PolicyId::Parse(policy_id_text));
  ECG_TRY(policy_generation, reader.Uint("policy_generation"));
  ECG_TRY(kind_name, reader.String("kind"));
  ECG_TRY(kind, EnumByName<RequestKind>(kind_name, RequestKindName, kRequestKinds, "request_kind"));
  ECG_TRY(service_text, reader.OptionalString("service_class", ""));
  ECG_TRY(outcome_name, reader.String("outcome"));
  ECG_TRY(outcome, EnumByName<Outcome>(outcome_name, OutcomeName, kOutcomes, "outcome"));

  Decision decision;
  decision.id = id;
  decision.request_id = request_id;
  decision.client_id = client;
  decision.epoch = Epoch::FromRaw(epoch);
  decision.policy_id = policy_id;
  decision.policy_generation = PolicyGeneration::FromRaw(policy_generation);
  decision.kind = kind;
  if (!service_text.empty()) {
    ECG_TRY(service, ServiceClassId::Parse(service_text));
    decision.service_class = service;
  }
  decision.outcome = outcome;

  ECG_TRY(reasons_value, reader.Require("reasons"));
  if (!reasons_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'reasons' must be an array");
  }
  for (const JsonValue& entry : reasons_value->as_array()) {
    ECG_TRY(reason, ParseReason(entry));
    decision.reasons.push_back(std::move(reason));
    if (decision.reasons.size() > kMaxReasonsPerDecision) {
      return reader.Err(ErrorCode::kResourceLimitExceeded, "more reasons than the bound");
    }
  }

  ECG_TRY(dependencies_value, reader.Require("evidence_dependencies"));
  if (!dependencies_value->is_array()) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'evidence_dependencies' must be an array");
  }
  for (const JsonValue& entry : dependencies_value->as_array()) {
    ECG_TRY(ref, ParseEvidenceRef(entry));
    decision.evidence_dependencies.push_back(ref);
  }

  ECG_TRY(intent_member, reader.Optional("intent"));
  if (intent_member != nullptr) {
    ECG_TRY(intent, ParseBoundedIntent(*intent_member));
    decision.intent = intent;
  }
  ECG_TRY(reconsider_member, reader.Optional("reconsider_at"));
  if (reconsider_member != nullptr) {
    if (!reconsider_member->is_string()) {
      return reader.Err(ErrorCode::kMalformedInput, "member 'reconsider_at' must be a string");
    }
    const auto parsed = ParseRfc3339(reconsider_member->as_string());
    if (!parsed.ok()) {
      return reader.Err(parsed.error().code(), parsed.error().message());
    }
    decision.reconsider_at = parsed.value();
  }

  ECG_TRY(economics_value, reader.Require("economics"));
  ECG_TRY(economics, ParseEconomicAssessment(*economics_value));
  decision.economics = economics;

  ECG_TRY(digest_text, reader.String("digest"));
  std::uint64_t digest_raw = 0;
  if (!ParseHex64(digest_text, &digest_raw)) {
    return reader.Err(ErrorCode::kMalformedInput, "member 'digest' must be 16 hexadecimal digits");
  }
  decision.digest = Digest64::FromRaw(digest_raw);
  ECG_TRY(decided_at, reader.Instant("decided_at"));
  decision.decided_at = decided_at;

  // Self-verification: a decision that does not hash to its own declared digest
  // has been altered in transit or in storage, and is refused rather than used.
  const Digest64 recomputed = ComputeDecisionDigest(decision);
  if (!(recomputed == decision.digest)) {
    return reader.Err(ErrorCode::kIntegrityFailure,
                      "decision digest mismatch: declared " + decision.digest.Hex() +
                          ", recomputed " + recomputed.Hex());
  }
  if (!(decision.id == DecisionId::FromRaw(decision.digest.raw()))) {
    return reader.Err(ErrorCode::kIntegrityFailure,
                      "decision id does not match its content digest");
  }
  return decision;
}

// ---------------------------------------------------------------------------
// Persisted state
// ---------------------------------------------------------------------------

JsonValue ToJson(const StateDocument& state) {
  JsonValue::Object members;
  members.emplace_back("epoch", JsonUint(state.epoch.raw()));
  members.emplace_back("policy", ToJson(state.policy));
  members.emplace_back("evidence", ToJson(state.evidence));
  members.emplace_back("updated_at", ToJson(state.updated_at));
  return JsonValue::ObjectValue(std::move(members));
}

Result<StateDocument> ParseStateDocument(const JsonValue& value) {
  Reader reader(value, "state");
  ECG_TRY_STATUS(reader.ExpectObject());
  ECG_TRY_STATUS(reader.AllowOnly({"epoch", "policy", "evidence", "updated_at"}));
  ECG_TRY(epoch, reader.Uint("epoch"));
  ECG_TRY(policy_value, reader.Require("policy"));
  ECG_TRY(policy, ParsePolicySet(*policy_value));
  ECG_TRY(evidence_value, reader.Require("evidence"));
  ECG_TRY(evidence, ParseEvidenceSet(*evidence_value));
  ECG_TRY(updated_at, reader.OptionalInstant("updated_at", UtcInstant::FromRaw(0)));

  StateDocument state;
  state.epoch = Epoch::FromRaw(epoch);
  state.policy = policy;
  state.evidence = evidence;
  state.updated_at = updated_at;
  return state;
}

void MarkEvidenceRecovered(EvidenceSet* evidence) {
  const auto mark = [](auto& slot) {
    using SlotType = typename std::decay_t<decltype(slot)>::value_type;
    (void)sizeof(SlotType);
    if (slot.has_value()) {
      slot->meta.provenance = Provenance::kRecoveredPersistence;
    }
  };
  mark(evidence->tariff);
  mark(evidence->demand);
  mark(evidence->reserve);
  mark(evidence->efficiency);
  mark(evidence->services);
  mark(evidence->capacity);
  mark(evidence->risk);
  mark(evidence->incident);
  mark(evidence->zone);
}

// ---------------------------------------------------------------------------
// Human-readable report
// ---------------------------------------------------------------------------

std::string FormatDecisionReport(const Decision& decision) {
  std::string out;
  out.reserve(1024);
  out += "outcome: ";
  out += OutcomeName(decision.outcome);
  out += "\n";
  out += "decision_id: ";
  out += decision.id.ToString();
  out += "\n";
  out += "request_id: ";
  out += decision.request_id.ToString();
  out += "\n";
  out += "client_id: ";
  out += decision.client_id.value();
  out += "\n";
  out += "kind: ";
  out += RequestKindName(decision.kind);
  out += "\n";
  if (!decision.service_class.empty()) {
    out += "service_class: ";
    out += decision.service_class.value();
    out += "\n";
  }
  out += "policy: ";
  out += decision.policy_id.value();
  out += "@";
  out += decision.policy_generation.ToString();
  out += " epoch=";
  out += decision.epoch.ToString();
  out += "\n";
  out += "decided_at: ";
  out += FormatRfc3339(decision.decided_at);
  out += "\n";

  out += "reasons:\n";
  if (decision.reasons.empty()) {
    out += "  (none)\n";
  }
  for (const Reason& reason : decision.reasons) {
    out += "  [";
    out += ReasonSeverityName(reason.severity);
    out += "] ";
    out += ReasonCodeName(reason.code);
    out += " ";
    out += reason.subject;
    out += ": ";
    out += reason.detail;
    if (reason.evidence.has_value()) {
      out += " (evidence ";
      out += reason.evidence->source.value();
      out += "#";
      out += reason.evidence->generation.ToString();
      out += " ";
      out += FreshnessName(reason.evidence->freshness);
      out += " observed ";
      out += FormatRfc3339(reason.evidence->observed_at);
      out += ")";
    }
    out += "\n";
  }

  if (decision.intent.has_value()) {
    const BoundedIntent& intent = *decision.intent;
    out += "intent: ";
    out += RequestKindName(intent.kind);
    out += " at most ";
    out += FormatQuantity(intent.magnitude_limit_kw);
    out += " kW for ";
    out += FormatQuantity(intent.duration_limit);
    out += " s during [";
    out += FormatRfc3339(intent.window.begin());
    out += ", ";
    out += FormatRfc3339(intent.window.end());
    out += ") expires ";
    out += FormatRfc3339(intent.expires_at);
    out += " target=";
    out += AuthorityKindName(intent.target_authority);
    out += " advisory_only=";
    out += intent.advisory_only ? "true" : "false";
    out += "\n";
  }
  if (decision.reconsider_at.has_value()) {
    out += "reconsider_at: ";
    out += FormatRfc3339(*decision.reconsider_at);
    out += "\n";
  }

  const EconomicAssessment& economics = decision.economics;
  out += "economics: ";
  if (!economics.evaluated) {
    out += "not evaluated\n";
  } else {
    out += "baseline=";
    out += FormatQuantity(economics.baseline_cost);
    out += " candidate=";
    out += FormatQuantity(economics.candidate_cost);
    out += " savings=";
    out += FormatQuantity(economics.gross_savings);
    out += " demand_charge_avoided=";
    out += FormatQuantity(economics.demand_charge_avoided);
    out += " baseline_price=";
    out += FormatQuantity(economics.baseline_price);
    out += " candidate_price=";
    out += FormatQuantity(economics.candidate_price);
    out += " favorable=";
    out += economics.favorable ? "true" : "false";
    if (economics.shift_target.has_value()) {
      out += " shift_target=[";
      out += FormatRfc3339(economics.shift_target->begin());
      out += ", ";
      out += FormatRfc3339(economics.shift_target->end());
      out += ")";
    }
    out += "\n";
  }
  out += "digest: ";
  out += decision.digest.Hex();
  out += "\n";
  return out;
}

}  // namespace ecg




