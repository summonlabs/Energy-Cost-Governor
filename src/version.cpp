#include "ecg/version.hpp"

#include <string>

namespace ecg {
namespace {

std::string MakeBuildIdentity() {
  std::string s;
  s.reserve(96);
  s.append(VersionString());
  s.append(" (");
#if defined(_MSC_VER)
  s.append("MSVC ");
  s.append(std::to_string(_MSC_VER / 100));
  s.push_back('.');
  s.append(std::to_string(_MSC_VER % 100));
#elif defined(__clang__)
  s.append("Clang ");
  s.append(__clang_version__);
#elif defined(__GNUC__)
  s.append("GCC ");
  s.append(std::to_string(__GNUC__));
  s.push_back('.');
  s.append(std::to_string(__GNUC_MINOR__));
#else
  s.append("unknown compiler");
#endif
  s.append(", ");
#if defined(_WIN64)
  s.append("Windows x64");
#elif defined(_WIN32)
  s.append("Windows x86");
#elif defined(__linux__)
  s.append("Linux");
#else
  s.append("unknown platform");
#endif
  s.push_back(')');
  return s;
}

}  // namespace

const char* VersionString() noexcept { return "1.0.0"; }

std::string_view BuildIdentity() noexcept {
  static const std::string identity = MakeBuildIdentity();
  return identity;
}

}  // namespace ecg
