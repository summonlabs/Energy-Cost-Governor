#ifndef ECG_CANONICAL_HPP
#define ECG_CANONICAL_HPP

// Canonical binary encoding.
//
// Digests, journal frames, and idempotency keys must be byte-identical for the
// same logical value on every platform, at every optimisation level, forever.
// This encoding is therefore explicit: little-endian field order, no padding, no
// implicit lengths, no compiler struct layout, no locale, and no floating point.
// Changing it is a format break, which is why kEncodingVersion exists.

#include <cstdint>
#include <string>
#include <string_view>

#include "ecg/result.hpp"

namespace ecg {

/// Bounds applied by the encoder so a malformed structure cannot produce an
/// unbounded buffer.
inline constexpr std::size_t kMaxEncodedStringLength = 1u << 20;
inline constexpr std::size_t kMaxEncodedPayload = 64u << 20;

class Encoder {
 public:
  Encoder() { bytes_.reserve(256); }

  void U8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

  void U16(std::uint16_t value) {
    U8(static_cast<std::uint8_t>(value & 0xFFu));
    U8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }

  void U32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      U8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }

  void U64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      U8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }

  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }

  void Bool(bool value) { U8(value ? 1u : 0u); }

  /// Length-prefixed byte string with a 32-bit length.
  [[nodiscard]] Status String(std::string_view text) {
    if (text.size() > kMaxEncodedStringLength) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "canonical",
                       "string longer than the encoding limit");
    }
    U32(static_cast<std::uint32_t>(text.size()));
    bytes_.append(text);
    return OkStatus();
  }

  /// Length-prefixed raw bytes, for nested encodings.
  [[nodiscard]] Status Bytes(std::string_view raw) {
    if (raw.size() > kMaxEncodedPayload) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "canonical",
                       "nested payload longer than the encoding limit");
    }
    U32(static_cast<std::uint32_t>(raw.size()));
    bytes_.append(raw);
    return OkStatus();
  }

  [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string TakeBytes() && { return std::move(bytes_); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

 private:
  std::string bytes_;
};

class Decoder {
 public:
  explicit Decoder(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] Result<std::uint8_t> U8() {
    if (index_ + 1 > bytes_.size()) {
      return Short();
    }
    return static_cast<std::uint8_t>(bytes_[index_++]);
  }

  [[nodiscard]] Result<std::uint16_t> U16() {
    ECG_TRY(lo, U8());
    ECG_TRY(hi, U8());
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(lo) |
                                      (static_cast<std::uint16_t>(hi) << 8));
  }

  [[nodiscard]] Result<std::uint32_t> U32() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      ECG_TRY(byte, U8());
      value |= static_cast<std::uint32_t>(byte) << (8 * i);
    }
    return value;
  }

  [[nodiscard]] Result<std::uint64_t> U64() {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      ECG_TRY(byte, U8());
      value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    return value;
  }

  [[nodiscard]] Result<std::int64_t> I64() {
    ECG_TRY(value, U64());
    return static_cast<std::int64_t>(value);
  }

  [[nodiscard]] Result<bool> Bool() {
    ECG_TRY(value, U8());
    if (value > 1u) {
      return MakeError(ErrorCode::kIntegrityFailure, "canonical",
                       "boolean field encoded with value " + std::to_string(value));
    }
    return value == 1u;
  }

  [[nodiscard]] Result<std::string> String() {
    ECG_TRY(length, U32());
    if (length > kMaxEncodedStringLength) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "canonical",
                       "declared string length exceeds the encoding limit");
    }
    if (index_ + length > bytes_.size()) {
      return Short();
    }
    std::string out(bytes_.substr(index_, length));
    index_ += length;
    return out;
  }

  [[nodiscard]] Result<std::string_view> Bytes() {
    ECG_TRY(length, U32());
    if (length > kMaxEncodedPayload) {
      return MakeError(ErrorCode::kResourceLimitExceeded, "canonical",
                       "declared payload length exceeds the encoding limit");
    }
    if (index_ + length > bytes_.size()) {
      return Short();
    }
    const std::string_view out = bytes_.substr(index_, length);
    index_ += length;
    return out;
  }

  [[nodiscard]] bool at_end() const noexcept { return index_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - index_; }

  /// Fails when unread bytes remain. Used to reject trailing garbage, which is
  /// how interior journal corruption is detected.
  [[nodiscard]] Status RequireEnd() const {
    if (!at_end()) {
      return MakeError(ErrorCode::kIntegrityFailure, "canonical",
                       std::to_string(remaining()) + " trailing bytes after the encoded value");
    }
    return OkStatus();
  }

 private:
  [[nodiscard]] Error Short() const {
    return MakeError(ErrorCode::kIntegrityFailure, "canonical", "encoded value ends prematurely");
  }

  std::string_view bytes_;
  std::size_t index_{0};
};

}  // namespace ecg

#endif  // ECG_CANONICAL_HPP
