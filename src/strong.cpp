#include "ecg/strong.hpp"

#include <limits>

#include "ecg/hash.hpp"

namespace ecg {
namespace {

/// True when the character is allowed anywhere inside a NameId.
[[nodiscard]] bool IsAllowedNameChar(char c) noexcept {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  switch (c) {
    case '.': case '_': case '-': case ':': case '/': case '@':
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsValidNameId(std::string_view text) noexcept {
  if (text.empty() || text.size() > 64) {
    return false;
  }
  for (char c : text) {
    if (!IsAllowedNameChar(c)) {
      return false;
    }
  }
  return true;
}

template <class Tag>
std::string Id<Tag>::ToString() const {
  return ToHex(raw_);
}

template <class Tag>
Result<Id<Tag>> Id<Tag>::Parse(std::string_view text) {
  if (text == "0") {
    return Id(0);
  }
  std::uint64_t raw = 0;
  if (!ParseHex64(text, &raw)) {
    return MakeError(ErrorCode::kMalformedInput, std::string(Tag::kName),
                     "identity must be 16 hexadecimal digits, got '" + std::string(text) + "'");
  }
  return Id(raw);
}

template <class Tag>
Result<Counter<Tag>> Counter<Tag>::Parse(std::string_view text) {
  if (text.empty() || text.size() > 20) {
    return MakeError(ErrorCode::kMalformedInput, std::string(Tag::kName),
                     "counter literal must be 1..20 decimal digits");
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return MakeError(ErrorCode::kMalformedInput, std::string(Tag::kName),
                       "counter literal contains a non-digit character");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10u) {
      return MakeError(ErrorCode::kNumericOverflow, std::string(Tag::kName),
                       "counter literal does not fit in 64 bits");
    }
    value = value * 10u + digit;
  }
  return Counter(value);
}

template <class Tag>
Result<NameId<Tag>> NameId<Tag>::Parse(std::string_view text) {
  if (text.empty()) {
    return MakeError(ErrorCode::kMissingRequiredField, std::string(Tag::kName), "identifier is empty");
  }
  if (text.size() > kMaxLength) {
    return MakeError(ErrorCode::kOutOfRange, std::string(Tag::kName),
                     "identifier longer than " + std::to_string(kMaxLength) + " characters");
  }
  for (char c : text) {
    if (!IsAllowedNameChar(c)) {
      return MakeError(ErrorCode::kMalformedInput, std::string(Tag::kName),
                       std::string("identifier contains disallowed character '") + c + "'");
    }
  }
  NameId result;
  result.value_.assign(text);
  return result;
}

template <class Tag>
NameId<Tag> NameId<Tag>::FromTrusted(std::string text) {
  NameId result;
  result.value_ = std::move(text);
  return result;
}

// Explicit instantiations for every identity domain in the public API. Keeping
// them here means the templates stay out of every translation unit that only
// needs the types, while the definitions remain in one reviewable place.
template class Id<RequestIdTag>;
template class Id<DecisionIdTag>;
template class Id<IntentIdTag>;

template class Counter<PolicyGenerationTag>;
template class Counter<EvidenceGenerationTag>;
template class Counter<EpochTag>;
template class Counter<TariffRevisionTag>;
template class Counter<JournalSequenceTag>;

template class NameId<ClientIdTag>;
template class NameId<SourceIdTag>;
template class NameId<TariffIdTag>;
template class NameId<PolicyIdTag>;
template class NameId<ServiceClassIdTag>;
template class NameId<DemandWindowIdTag>;
template class NameId<ZoneIdTag>;

}  // namespace ecg
