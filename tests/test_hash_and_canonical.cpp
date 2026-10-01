// Tests for the deterministic hashing primitives and the canonical binary
// encoding.
//
// The known-answer cases below are the published FNV-1a 64 and CRC-32C vectors,
// so they catch a wrong constants table or a wrong byte order rather than just
// re-deriving whatever the implementation happens to do.

#include "test.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ecg/canonical.hpp"
#include "ecg/hash.hpp"

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;

/// A byte string built from explicit values, so no source-encoding question can
/// change what a test feeds the hash.
[[nodiscard]] std::string Bytes(std::initializer_list<unsigned> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned value : values) {
    out.push_back(static_cast<char>(value & 0xFFu));
  }
  return out;
}

/// Feeds a byte range one byte at a time through the public incremental API.
[[nodiscard]] std::uint64_t FnvByteAtATime(std::string_view text) {
  std::uint64_t running = kFnvOffsetBasis;
  for (const char byte : text) {
    running = ecg::Fnv1a64(&byte, 1, running);
  }
  return running;
}

}  // namespace

// ---------------------------------------------------------------------------
// FNV-1a 64
// ---------------------------------------------------------------------------

ECG_TEST("hash.fnv1a64_matches_published_vectors") {
  ECG_CHECK_EQ(ecg::Fnv1a64(std::string_view("")), std::uint64_t{0xcbf29ce484222325ULL});
  ECG_CHECK_EQ(ecg::Fnv1a64(std::string_view("a")), std::uint64_t{0xaf63dc4c8601ec8cULL});
  ECG_CHECK_EQ(ecg::Fnv1a64(std::string_view("b")), std::uint64_t{0xaf63df4c8601f1a5ULL});
  ECG_CHECK_EQ(ecg::Fnv1a64(std::string_view("foobar")), std::uint64_t{0x85944171f73967e8ULL});

  // The default seed is the offset basis, and the pointer/length overload agrees
  // with the string_view overload.
  const char text[] = "foobar";
  ECG_CHECK_EQ(ecg::Fnv1a64(text, 6), std::uint64_t{0x85944171f73967e8ULL});
  ECG_CHECK_EQ(ecg::Fnv1a64(text, 6, kFnvOffsetBasis), ecg::Fnv1a64(std::string_view(text)));

  // A different seed gives a different digest for the same input.
  ECG_CHECK(ecg::Fnv1a64(std::string_view("foobar"), 1u) !=
            ecg::Fnv1a64(std::string_view("foobar")));
}

ECG_TEST("hash.fnv1a64_incremental_feeding_matches_one_shot") {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  const std::uint64_t one_shot = ecg::Fnv1a64(text);

  ECG_CHECK_EQ(FnvByteAtATime(text), one_shot);

  const std::uint64_t first_half = ecg::Fnv1a64(text.data(), 20);
  const std::uint64_t second_half = ecg::Fnv1a64(text.data() + 20, text.size() - 20, first_half);
  ECG_CHECK_EQ(second_half, one_shot);

  // The length is respected: an embedded NUL is a byte like any other.
  const std::string with_nul = Bytes({'a', 0x00, 'b'});
  ECG_CHECK(ecg::Fnv1a64(with_nul) != ecg::Fnv1a64(std::string_view("a")));
  ECG_CHECK_EQ(ecg::Fnv1a64(with_nul.data(), 3), ecg::Fnv1a64(with_nul));
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

ECG_TEST("hash.crc32c_matches_published_vectors") {
  ECG_CHECK_EQ(ecg::ToHex(ecg::Crc32c(std::string_view(""))), std::string("00000000"));
  ECG_CHECK_EQ(ecg::ToHex(ecg::Crc32c(std::string_view("123456789"))), std::string("e3069283"));

  const char digits[] = "123456789";
  ECG_CHECK_EQ(ecg::ToHex(ecg::Crc32c(digits, 9)), ecg::ToHex(ecg::Crc32c(std::string_view(digits))));

  // The empty range through the pointer overload matches the empty string_view.
  ECG_CHECK_EQ(ecg::ToHex(ecg::Crc32c(digits, 0)), std::string("00000000"));
}

ECG_TEST("hash.crc32c_incremental_update_matches_one_shot") {
  const std::string text = "123456789";
  const std::uint32_t one_shot = ecg::Crc32c(text);

  for (std::size_t split = 0; split <= text.size(); ++split) {
    const std::uint32_t first = ecg::Crc32cUpdate(0, text.data(), split);
    const std::uint32_t second =
        ecg::Crc32cUpdate(first, text.data() + split, text.size() - split);
    ECG_CHECK_EQ(ecg::ToHex(second), ecg::ToHex(one_shot));
  }

  // Chunk boundaries in the middle of a longer buffer, and byte-at-a-time.
  std::string longer;
  for (int i = 0; i < 300; ++i) {
    longer.push_back(static_cast<char>((i * 7) & 0xFF));
  }
  std::uint32_t running = 0;
  for (std::size_t offset = 0; offset < longer.size();) {
    const std::size_t chunk = (offset % 13) + 1;
    const std::size_t size = chunk < longer.size() - offset ? chunk : longer.size() - offset;
    running = ecg::Crc32cUpdate(running, longer.data() + offset, size);
    offset += size;
  }
  ECG_CHECK_EQ(ecg::ToHex(running), ecg::ToHex(ecg::Crc32c(longer)));
}

// ---------------------------------------------------------------------------
// Hex rendering and parsing
// ---------------------------------------------------------------------------

ECG_TEST("hash.to_hex_is_fixed_width_lower_case") {
  ECG_CHECK_EQ(ecg::ToHex(std::uint64_t{0}), std::string("0000000000000000"));
  ECG_CHECK_EQ(ecg::ToHex(std::uint64_t{0xdeadbeefcafebabeULL}),
               std::string("deadbeefcafebabe"));
  ECG_CHECK_EQ(ecg::ToHex((std::numeric_limits<std::uint64_t>::max)()),
               std::string("ffffffffffffffff"));
  ECG_CHECK_EQ(ecg::ToHex(std::uint32_t{0}), std::string("00000000"));
  ECG_CHECK_EQ(ecg::ToHex(std::uint32_t{0xE3069283u}), std::string("e3069283"));

  const ecg::Digest64 digest = ecg::Digest64::FromRaw(0x0123456789abcdefULL);
  ECG_CHECK_EQ(digest.Hex(), std::string("0123456789abcdef"));
  ECG_CHECK_EQ(digest.raw(), std::uint64_t{0x0123456789abcdefULL});
  ECG_CHECK(ecg::Digest64::FromRaw(1) == ecg::Digest64::FromRaw(1));
  ECG_CHECK(ecg::Digest64::FromRaw(1) != ecg::Digest64::FromRaw(2));
  ECG_CHECK(ecg::Digest64::FromRaw(1) < ecg::Digest64::FromRaw(2));
}

ECG_TEST("hash.parse_hex64_round_trips_and_rejects_bad_input") {
  const std::uint64_t values[] = {0u, 1u, 0x0123456789abcdefULL,
                                  (std::numeric_limits<std::uint64_t>::max)()};
  for (const std::uint64_t value : values) {
    std::uint64_t parsed = 0;
    ECG_CHECK(ecg::ParseHex64(ecg::ToHex(value), &parsed));
    ECG_CHECK_EQ(parsed, value);
  }

  std::uint64_t parsed = 0;
  ECG_CHECK(ecg::ParseHex64("ABCDEF0123456789", &parsed));
  ECG_CHECK_EQ(parsed, std::uint64_t{0xABCDEF0123456789ULL});

  const char* const rejected[] = {
      "",                   "0",                  "0123456789abcde",
      "0123456789abcdef0",  "0123456789abcdeg",   "0123456789abcdef ",
      " 123456789abcdef",   "0x123456789abcd",    "-123456789abcdef",
  };
  for (const char* const text : rejected) {
    std::uint64_t ignored = 0;
    ECG_CHECK_MSG(!ecg::ParseHex64(text, &ignored), std::string("must be rejected: ") + text);
  }

  // A rejected literal must not publish a value through the out parameter.
  std::uint64_t untouched = 0x5A5A5A5A5A5A5A5AULL;
  ECG_CHECK(!ecg::ParseHex64("not-a-digest", &untouched));
  ECG_CHECK_EQ(untouched, std::uint64_t{0x5A5A5A5A5A5A5A5AULL});
  ECG_CHECK(!ecg::ParseHex64("0123456789abcdef", nullptr));
}

// ---------------------------------------------------------------------------
// DigestBuilder
// ---------------------------------------------------------------------------

ECG_TEST("canonical.digest_builder_is_order_sensitive") {
  ecg::DigestBuilder forward;
  forward.Update(std::string_view("ab"));
  ecg::DigestBuilder backward;
  backward.Update(std::string_view("ba"));
  ECG_CHECK(forward.digest() != backward.digest());

  // Feeding byte by byte equals feeding the whole range at once.
  ecg::DigestBuilder whole;
  whole.Update(std::string_view("ab"));
  ecg::DigestBuilder pieces;
  pieces.UpdateByte(static_cast<std::uint8_t>('a'));
  pieces.UpdateByte(static_cast<std::uint8_t>('b'));
  ECG_CHECK_EQ(pieces.raw(), whole.raw());

  // An untouched builder is the FNV offset basis, i.e. the empty digest.
  const ecg::DigestBuilder fresh;
  ECG_CHECK_EQ(fresh.raw(), kFnvOffsetBasis);
  ECG_CHECK_EQ(fresh.digest().raw(), kFnvOffsetBasis);
  ECG_CHECK_EQ(whole.raw(), ecg::Fnv1a64(std::string_view("ab")));

  // Update(pointer, size) and Update(string_view) agree, NUL bytes included.
  const std::string with_nul = Bytes({'x', 0x00, 'y'});
  ecg::DigestBuilder as_view;
  as_view.Update(std::string_view(with_nul));
  ecg::DigestBuilder as_pointer;
  as_pointer.Update(with_nul.data(), with_nul.size());
  ECG_CHECK_EQ(as_view.raw(), as_pointer.raw());
}

ECG_TEST("canonical.digest_builder_writes_integers_little_endian") {
  ecg::DigestBuilder via_methods;
  via_methods.UpdateU32(0x11223344u);
  via_methods.UpdateU64(0x1122334455667788ULL);
  via_methods.UpdateI64(-2);

  ecg::DigestBuilder via_bytes;
  via_bytes.Update(Bytes({0x44, 0x33, 0x22, 0x11,                          // u32
                          0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // u64
                          0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));  // i64 -2
  ECG_CHECK_EQ(via_methods.raw(), via_bytes.raw());

  // A negative int64 is its two's-complement unsigned image, nothing else.
  ecg::DigestBuilder signed_value;
  signed_value.UpdateI64(-1);
  ecg::DigestBuilder unsigned_value;
  unsigned_value.UpdateU64((std::numeric_limits<std::uint64_t>::max)());
  ECG_CHECK_EQ(signed_value.raw(), unsigned_value.raw());

  // A single byte is written unsigned, not sign extended.
  ecg::DigestBuilder high_byte;
  high_byte.UpdateByte(0xFFu);
  ecg::DigestBuilder high_byte_builder;
  high_byte_builder.Update(Bytes({0xFF}));
  ECG_CHECK_EQ(high_byte.raw(), high_byte_builder.raw());

  // Field order matters: the same fields in the other order hash differently.
  ecg::DigestBuilder first_then_second;
  first_then_second.UpdateU32(1u);
  first_then_second.UpdateU32(2u);
  ecg::DigestBuilder second_then_first;
  second_then_first.UpdateU32(2u);
  second_then_first.UpdateU32(1u);
  ECG_CHECK(first_then_second.raw() != second_then_first.raw());
}

// ---------------------------------------------------------------------------
// Encoder / Decoder
// ---------------------------------------------------------------------------

ECG_TEST("canonical.encoder_decoder_round_trip_every_field_type") {
  const std::string payload = Bytes({0x00, 0x01, 0x02});

  ecg::Encoder encoder;
  encoder.U8(0xABu);
  encoder.U16(0xBEEFu);
  encoder.U32(0xDEADBEEFu);
  encoder.U64(0x0123456789ABCDEFULL);
  encoder.I64(-123456789012345LL);
  encoder.Bool(true);
  encoder.Bool(false);
  ECG_CHECK_OK(encoder.String(std::string("hello")));
  ECG_CHECK_OK(encoder.String(std::string()));
  ECG_CHECK_OK(encoder.Bytes(payload));

  ecg::Decoder decoder(encoder.bytes());
  const auto u8 = decoder.U8();
  ECG_CHECK_OK(u8);
  if (u8.ok()) {
    ECG_CHECK_EQ(static_cast<unsigned>(u8.value()), 0xABu);
  }
  const auto u16 = decoder.U16();
  ECG_CHECK_OK(u16);
  if (u16.ok()) {
    ECG_CHECK_EQ(static_cast<unsigned>(u16.value()), 0xBEEFu);
  }
  const auto u32 = decoder.U32();
  ECG_CHECK_OK(u32);
  if (u32.ok()) {
    ECG_CHECK_EQ(static_cast<unsigned>(u32.value()), 0xDEADBEEFu);
  }
  const auto u64 = decoder.U64();
  ECG_CHECK_OK(u64);
  if (u64.ok()) {
    ECG_CHECK_EQ(u64.value(), std::uint64_t{0x0123456789ABCDEFULL});
  }
  const auto i64 = decoder.I64();
  ECG_CHECK_OK(i64);
  if (i64.ok()) {
    ECG_CHECK_EQ(i64.value(), std::int64_t{-123456789012345LL});
  }
  const auto flag_true = decoder.Bool();
  ECG_CHECK_OK(flag_true);
  if (flag_true.ok()) {
    ECG_CHECK_EQ(flag_true.value(), true);
  }
  const auto flag_false = decoder.Bool();
  ECG_CHECK_OK(flag_false);
  if (flag_false.ok()) {
    ECG_CHECK_EQ(flag_false.value(), false);
  }
  const auto text = decoder.String();
  ECG_CHECK_OK(text);
  if (text.ok()) {
    ECG_CHECK_EQ(text.value(), std::string("hello"));
  }
  const auto empty_text = decoder.String();
  ECG_CHECK_OK(empty_text);
  if (empty_text.ok()) {
    ECG_CHECK_EQ(empty_text.value(), std::string());
  }
  const auto bytes = decoder.Bytes();
  ECG_CHECK_OK(bytes);
  if (bytes.ok()) {
    ECG_CHECK_EQ(std::string(bytes.value()), payload);
  }
  ECG_CHECK(decoder.at_end());
  ECG_CHECK_EQ(decoder.remaining(), static_cast<std::size_t>(0));
  ECG_CHECK_OK(decoder.RequireEnd());
}

ECG_TEST("canonical.encoder_writes_little_endian_fields_without_padding") {
  ecg::Encoder encoder;
  encoder.U16(0x0102u);
  encoder.U32(0x11223344u);
  encoder.U64(0x1122334455667788ULL);
  ECG_CHECK_EQ(encoder.size(), static_cast<std::size_t>(14));
  ECG_CHECK_EQ(encoder.bytes(),
               Bytes({0x02, 0x01,                                        // u16
                      0x44, 0x33, 0x22, 0x11,                            // u32
                      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}));  // u64
}

ECG_TEST("canonical.decoder_rejects_truncated_input") {
  ecg::Encoder encoder;
  encoder.U64(0x0123456789ABCDEFULL);
  const std::string bytes = encoder.bytes();

  for (std::size_t size = 0; size < bytes.size(); ++size) {
    ecg::Decoder decoder(std::string_view(bytes).substr(0, size));
    ECG_CHECK_ERR(decoder.U64(), ecg::ErrorCode::kIntegrityFailure);
  }

  ecg::Decoder empty{std::string_view()};
  ECG_CHECK_ERR(empty.U8(), ecg::ErrorCode::kIntegrityFailure);
  ECG_CHECK_ERR(empty.U16(), ecg::ErrorCode::kIntegrityFailure);
  ECG_CHECK_ERR(empty.U32(), ecg::ErrorCode::kIntegrityFailure);
  ECG_CHECK_ERR(empty.String(), ecg::ErrorCode::kIntegrityFailure);
  ECG_CHECK_ERR(empty.Bool(), ecg::ErrorCode::kIntegrityFailure);

  // A length prefix that promises more bytes than the buffer holds.
  ecg::Encoder promising;
  promising.U32(5u);
  promising.U8(static_cast<std::uint8_t>('a'));
  promising.U8(static_cast<std::uint8_t>('b'));
  ecg::Decoder short_payload(promising.bytes());
  ECG_CHECK_ERR(short_payload.String(), ecg::ErrorCode::kIntegrityFailure);
  ECG_CHECK_ERR(short_payload.Bytes(), ecg::ErrorCode::kIntegrityFailure);

  // A truncated multi-byte field must not consume the bytes it did read.
  ecg::Decoder partial(std::string_view(bytes).substr(0, 6));
  ECG_CHECK_ERR(partial.U64(), ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("canonical.decoder_rejects_overlong_declared_lengths") {
  ecg::Encoder declared_string;
  declared_string.U32(static_cast<std::uint32_t>(ecg::kMaxEncodedStringLength + 1u));
  ecg::Decoder string_decoder(declared_string.bytes());
  ECG_CHECK_ERR(string_decoder.String(), ecg::ErrorCode::kResourceLimitExceeded);

  ecg::Encoder declared_payload;
  declared_payload.U32(static_cast<std::uint32_t>(ecg::kMaxEncodedPayload + 1u));
  ecg::Decoder payload_decoder(declared_payload.bytes());
  ECG_CHECK_ERR(payload_decoder.Bytes(), ecg::ErrorCode::kResourceLimitExceeded);

  // The boundary itself is not over-long; it fails only because the bytes are
  // absent, which is an integrity failure rather than a resource failure.
  ecg::Encoder at_limit;
  at_limit.U32(static_cast<std::uint32_t>(ecg::kMaxEncodedStringLength));
  ecg::Decoder at_limit_decoder(at_limit.bytes());
  ECG_CHECK_ERR(at_limit_decoder.String(), ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("canonical.decoder_rejects_a_boolean_encoded_as_two") {
  ecg::Encoder encoder;
  encoder.U8(2u);
  ecg::Decoder decoder(encoder.bytes());
  ECG_CHECK_ERR(decoder.Bool(), ecg::ErrorCode::kIntegrityFailure);

  ecg::Encoder high;
  high.U8(0xFFu);
  ecg::Decoder high_decoder(high.bytes());
  ECG_CHECK_ERR(high_decoder.Bool(), ecg::ErrorCode::kIntegrityFailure);
}

ECG_TEST("canonical.decoder_require_end_rejects_trailing_bytes") {
  ecg::Encoder encoder;
  encoder.U8(1u);
  encoder.U8(2u);
  ecg::Decoder decoder(encoder.bytes());

  const auto first = decoder.U8();
  ECG_CHECK_OK(first);
  ECG_CHECK(!decoder.at_end());
  ECG_CHECK_EQ(decoder.remaining(), static_cast<std::size_t>(1));
  ECG_CHECK_ERR(decoder.RequireEnd(), ecg::ErrorCode::kIntegrityFailure);

  const auto second = decoder.U8();
  ECG_CHECK_OK(second);
  if (first.ok() && second.ok()) {
    ECG_CHECK_EQ(static_cast<unsigned>(first.value()), 1u);
    ECG_CHECK_EQ(static_cast<unsigned>(second.value()), 2u);
  }
  ECG_CHECK(decoder.at_end());
  ECG_CHECK_OK(decoder.RequireEnd());
}
