#ifndef ECG_HASH_HPP
#define ECG_HASH_HPP

// Deterministic hashing primitives used for canonical digests, journal frame
// integrity, and the journal hash chain.
//
// Two distinct algorithms are used on purpose:
//   * FNV-1a 64   -- the decision/journal chain value. Cheap, order sensitive,
//                    and stable across processes and platforms.
//   * CRC-32C     -- per-frame integrity, with a hardware-independent
//                    software table so a persisted journal verifies identically
//                    on every supported toolchain.
// Neither is a cryptographic construction, and neither is used as one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ecg {

/// FNV-1a 64-bit hash of a byte range.
[[nodiscard]] std::uint64_t Fnv1a64(const void* data, std::size_t size,
                                    std::uint64_t seed = 0xcbf29ce484222325ULL) noexcept;

[[nodiscard]] inline std::uint64_t Fnv1a64(std::string_view bytes,
                                           std::uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
  return Fnv1a64(bytes.data(), bytes.size(), seed);
}

/// CRC-32C (Castagnoli, reflected, polynomial 0x1EDC6F41) of a byte range.
[[nodiscard]] std::uint32_t Crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] std::uint32_t Crc32c(std::string_view bytes) noexcept;
/// Incremental form; the running value starts at 0 and is fed to the next call.
[[nodiscard]] std::uint32_t Crc32cUpdate(std::uint32_t running, const void* data, std::size_t size) noexcept;

/// A 64-bit digest rendered as 16 lower-case hex digits.
[[nodiscard]] std::string ToHex(std::uint64_t value);
/// A 32-bit digest rendered as 8 lower-case hex digits.
[[nodiscard]] std::string ToHex(std::uint32_t value);
/// Parses 16 hex digits; rejects any other length or non-hex character.
[[nodiscard]] bool ParseHex64(std::string_view text, std::uint64_t* out) noexcept;

/// A first-class digest value with canonical formatting, so digests can be
/// compared, logged, and persisted without ever round-tripping through text.
class Digest64 {
 public:
  constexpr Digest64() noexcept = default;
  explicit constexpr Digest64(std::uint64_t raw) noexcept : raw_(raw) {}

  [[nodiscard]] static constexpr Digest64 FromRaw(std::uint64_t raw) noexcept { return Digest64(raw); }
  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
  [[nodiscard]] std::string Hex() const { return ToHex(raw_); }

  friend constexpr bool operator==(Digest64 a, Digest64 b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(Digest64 a, Digest64 b) noexcept { return a.raw_ != b.raw_; }
  friend constexpr bool operator<(Digest64 a, Digest64 b) noexcept { return a.raw_ < b.raw_; }

 private:
  std::uint64_t raw_{0};
};

/// Accumulates a canonical byte stream into an FNV-1a digest. All persisted
/// structures are hashed through this type so the chain never depends on
/// padding, endianness, or locale.
class DigestBuilder {
 public:
  DigestBuilder() noexcept = default;

  void Update(const void* data, std::size_t size) noexcept { value_ = Fnv1a64(data, size, value_); }
  void Update(std::string_view bytes) noexcept { Update(bytes.data(), bytes.size()); }
  void UpdateByte(std::uint8_t byte) noexcept { Update(&byte, 1); }
  void UpdateU32(std::uint32_t value) noexcept;
  void UpdateU64(std::uint64_t value) noexcept;
  void UpdateI64(std::int64_t value) noexcept;

  [[nodiscard]] Digest64 digest() const noexcept { return Digest64(value_); }
  [[nodiscard]] std::uint64_t raw() const noexcept { return value_; }

 private:
  std::uint64_t value_{0xcbf29ce484222325ULL};
};

}  // namespace ecg

#endif  // ECG_HASH_HPP
