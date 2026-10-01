#ifndef ECG_STRONG_HPP
#define ECG_STRONG_HPP

// Strongly typed identities, generations, epochs, and revisions.
//
// The governing failure mode this header removes is silent interchange: a
// policy generation used where an evidence generation was meant, a tariff
// revision compared against an epoch, or a service class identifier accepted
// where a source identifier was expected. Each of those is a compile error here.
//
// Two identity shapes exist on purpose:
//   * Id<Tag>      -- runtime-assigned 64-bit identities (decisions, requests).
//   * NameId<Tag>  -- bounded, validated external identities (tariff, service
//                     class, evidence source, policy). These are minted by
//                     adjacent authorities, so this runtime validates their
//                     shape but never invents or reinterprets them.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ecg/result.hpp"

namespace ecg {

// ---------------------------------------------------------------------------
// Tags. Each tag carries its own diagnostic name so error messages identify the
// exact identity domain that was violated.
// ---------------------------------------------------------------------------

struct RequestIdTag { static constexpr std::string_view kName = "request_id"; };
struct DecisionIdTag { static constexpr std::string_view kName = "decision_id"; };
struct IntentIdTag { static constexpr std::string_view kName = "intent_id"; };

struct ClientIdTag { static constexpr std::string_view kName = "client_id"; };
struct SourceIdTag { static constexpr std::string_view kName = "source_id"; };
struct TariffIdTag { static constexpr std::string_view kName = "tariff_id"; };
struct PolicyIdTag { static constexpr std::string_view kName = "policy_id"; };
struct ServiceClassIdTag { static constexpr std::string_view kName = "service_class_id"; };
struct DemandWindowIdTag { static constexpr std::string_view kName = "demand_window_id"; };
struct ZoneIdTag { static constexpr std::string_view kName = "zone_id"; };

struct PolicyGenerationTag { static constexpr std::string_view kName = "policy_generation"; };
struct EvidenceGenerationTag { static constexpr std::string_view kName = "evidence_generation"; };
struct EpochTag { static constexpr std::string_view kName = "epoch"; };
struct TariffRevisionTag { static constexpr std::string_view kName = "tariff_revision"; };
struct JournalSequenceTag { static constexpr std::string_view kName = "journal_sequence"; };

// ---------------------------------------------------------------------------
// Runtime identities
// ---------------------------------------------------------------------------

/// A 64-bit identity. Zero is reserved for "unset" and is never assigned to a
/// decision, request, or intent that has been produced by this runtime.
template <class Tag>
class Id {
 public:
  using rep = std::uint64_t;
  using tag = Tag;

  constexpr Id() noexcept = default;

  [[nodiscard]] static constexpr Id FromRaw(rep raw) noexcept { return Id(raw); }

  [[nodiscard]] constexpr rep raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return raw_ != 0; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return raw_ == 0; }

  /// Lower-case hexadecimal rendering; this is the form used in JSON and CLI
  /// output because it is fixed width and sorts stably.
  [[nodiscard]] std::string ToString() const;

  /// Parses a hexadecimal literal of exactly 16 digits, or the decimal literal
  /// "0" for the unset identity.
  [[nodiscard]] static Result<Id> Parse(std::string_view text);

  [[nodiscard]] constexpr std::size_t hash_value() const noexcept {
    return static_cast<std::size_t>(raw_ ^ (raw_ >> 32));
  }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(Id a, Id b) noexcept {
    return a.raw_ <=> b.raw_;
  }

 private:
  explicit constexpr Id(rep raw) noexcept : raw_(raw) {}

  rep raw_{0};
};

/// A monotonic counter with its own identity domain. Every increment is
/// explicit and checked, so a generation can never wrap silently.
template <class Tag>
class Counter {
 public:
  using rep = std::uint64_t;
  using tag = Tag;

  constexpr Counter() noexcept = default;

  [[nodiscard]] static constexpr Counter FromRaw(rep raw) noexcept { return Counter(raw); }

  [[nodiscard]] constexpr rep raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }

  /// The next counter value. Fails when the counter is exhausted rather than
  /// wrapping to a value that would look like a fresh generation.
  [[nodiscard]] Result<Counter> Next() const {
    if (raw_ == (std::numeric_limits<rep>::max)()) {
      return MakeError(ErrorCode::kNumericOverflow, std::string(Tag::kName),
                       "counter exhausted at " + std::to_string(raw_));
    }
    return Counter(raw_ + 1);
  }

  [[nodiscard]] std::string ToString() const { return std::to_string(raw_); }

  [[nodiscard]] static Result<Counter> Parse(std::string_view text);

  [[nodiscard]] constexpr std::size_t hash_value() const noexcept {
    return static_cast<std::size_t>(raw_ ^ (raw_ >> 32));
  }

  friend constexpr bool operator==(Counter a, Counter b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(Counter a, Counter b) noexcept {
    return a.raw_ <=> b.raw_;
  }

 private:
  explicit constexpr Counter(rep raw) noexcept : raw_(raw) {}

  rep raw_{0};
};

// ---------------------------------------------------------------------------
// External identities
// ---------------------------------------------------------------------------

/// A bounded, validated identity minted by an adjacent authority.
///
/// The accepted alphabet is ASCII alphanumerics plus '.', '_', '-', ':', '/',
/// and '@'; length is 1..64 characters. Anything else is rejected at the
/// boundary instead of being normalised into something that looks trustworthy.
template <class Tag>
class NameId {
 public:
  using tag = Tag;

  static constexpr std::size_t kMaxLength = 64;

  NameId() = default;

  /// Validates and constructs. This is the only path for untrusted text.
  [[nodiscard]] static Result<NameId> Parse(std::string_view text);

  /// Constructs from a literal or an already validated value. Violations are a
  /// programming defect and are trapped in debug builds.
  [[nodiscard]] static NameId FromTrusted(std::string text);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const char* c_str() const noexcept { return value_.c_str(); }

  [[nodiscard]] std::string ToString() const { return value_; }

  [[nodiscard]] std::size_t hash_value() const noexcept {
    return static_cast<std::size_t>(std::hash<std::string>{}(value_));
  }

  friend bool operator==(const NameId& a, const NameId& b) noexcept { return a.value_ == b.value_; }
  friend std::strong_ordering operator<=>(const NameId& a, const NameId& b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::string value_;
};

/// Returns true when text is a legal NameId body for any tag.
[[nodiscard]] bool IsValidNameId(std::string_view text) noexcept;

/// Hash functor for the strong types above, usable with unordered containers.
template <class T>
struct StrongHash {
  [[nodiscard]] std::size_t operator()(const T& value) const noexcept { return value.hash_value(); }
};

// ---------------------------------------------------------------------------
// Named aliases used across the domain
// ---------------------------------------------------------------------------

using RequestId = Id<RequestIdTag>;
using DecisionId = Id<DecisionIdTag>;
using IntentId = Id<IntentIdTag>;

using ClientId = NameId<ClientIdTag>;
using SourceId = NameId<SourceIdTag>;
using TariffId = NameId<TariffIdTag>;
using PolicyId = NameId<PolicyIdTag>;
using ServiceClassId = NameId<ServiceClassIdTag>;
using DemandWindowId = NameId<DemandWindowIdTag>;
using ZoneId = NameId<ZoneIdTag>;

using PolicyGeneration = Counter<PolicyGenerationTag>;
using EvidenceGeneration = Counter<EvidenceGenerationTag>;
using Epoch = Counter<EpochTag>;
using TariffRevision = Counter<TariffRevisionTag>;
using JournalSequence = Counter<JournalSequenceTag>;

}  // namespace ecg

#endif  // ECG_STRONG_HPP
