#include "ecg/hash.hpp"

#include <array>

namespace ecg {
namespace {

/// CRC-32C lookup table, reflected form. Built once at first use; the table is
/// a pure function of the polynomial, so the digest is identical everywhere.
const std::array<std::uint32_t, 256>& Crc32cTable() {
  static const std::array<std::uint32_t, 256> table = [] {
    constexpr std::uint32_t kPoly = 0x82F63B78u;  // reflected 0x1EDC6F41
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPoly : (crc >> 1);
      }
      t[i] = crc;
    }
    return t;
  }();
  return table;
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed) noexcept {
  constexpr std::uint64_t kPrime = 0x100000001b3ULL;
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t hash = seed;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kPrime;
  }
  return hash;
}

std::uint32_t Crc32cUpdate(std::uint32_t running, const void* data, std::size_t size) noexcept {
  const auto& table = Crc32cTable();
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = running ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32c(const void* data, std::size_t size) noexcept {
  return Crc32cUpdate(0, data, size);
}

std::uint32_t Crc32c(std::string_view bytes) noexcept { return Crc32c(bytes.data(), bytes.size()); }

std::string ToHex(std::uint64_t value) {
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

std::string ToHex(std::uint32_t value) {
  std::string out(8, '0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

bool ParseHex64(std::string_view text, std::uint64_t* out) noexcept {
  if (text.size() != 16 || out == nullptr) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A') + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  *out = value;
  return true;
}

void DigestBuilder::UpdateU32(std::uint32_t value) noexcept {
  unsigned char bytes[4];
  for (int i = 0; i < 4; ++i) {
    bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  }
  Update(bytes, sizeof(bytes));
}

void DigestBuilder::UpdateU64(std::uint64_t value) noexcept {
  unsigned char bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  }
  Update(bytes, sizeof(bytes));
}

void DigestBuilder::UpdateI64(std::int64_t value) noexcept {
  UpdateU64(static_cast<std::uint64_t>(value));
}

}  // namespace ecg
