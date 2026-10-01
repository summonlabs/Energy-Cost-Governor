#ifndef ECG_VERSION_HPP
#define ECG_VERSION_HPP

// Energy-Cost-Governor -- Summon Software Labs Data Center Control Plane.
//
// Single source of truth for the runtime version. The CMake project version,
// the runtime constants below, and the persisted journal format version are
// deliberately distinct: the library version changes far more often than the
// on-disk format, and a reader must be able to reject a format it cannot
// interpret without guessing from the library version alone.

#include <cstdint>
#include <string_view>

#define ECG_VERSION_MAJOR 1
#define ECG_VERSION_MINOR 0
#define ECG_VERSION_PATCH 0

namespace ecg {

/// Library semantic version.
inline constexpr std::uint32_t kVersionMajor = ECG_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = ECG_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = ECG_VERSION_PATCH;

/// Canonical version string, for example "1.0.0".
[[nodiscard]] const char* VersionString() noexcept;

/// Version string including the build configuration, for example
/// "1.0.0 (Windows x64, MSVC 19.44)".
[[nodiscard]] std::string_view BuildIdentity() noexcept;

/// On-disk journal format version understood by this build. A journal written
/// with a different format version is rejected rather than migrated by guess.
inline constexpr std::uint32_t kJournalFormatVersion = 1;

/// Canonical binary encoding version for persisted records.
inline constexpr std::uint32_t kEncodingVersion = 1;

}  // namespace ecg

#endif  // ECG_VERSION_HPP
