cmake_minimum_required(VERSION 3.28)

project(EnergyCostGovernor
  VERSION 1.0.0
  DESCRIPTION "Summon Software Labs Data Center Control Plane energy-cost policy governor"
  HOMEPAGE_URL "https://github.com/summonlabs/Energy-Cost-Governor"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(ECG_IS_TOP_LEVEL ON)
else()
  set(ECG_IS_TOP_LEVEL OFF)
endif()

option(ECG_BUILD_CLI "Build the ecg command line tool" ON)
option(ECG_BUILD_TESTS "Build the Energy-Cost-Governor test suite" ${ECG_IS_TOP_LEVEL})
option(ECG_BUILD_BENCHMARKS "Build the Energy-Cost-Governor benchmarks" OFF)
option(ECG_WARNINGS_AS_ERRORS "Promote first-party warnings to errors" ON)

# ---------------------------------------------------------------------------
# Warnings. First-party targets only; the test suite inherits the same policy.
# ---------------------------------------------------------------------------
add_library(ecg_warnings INTERFACE)
if(MSVC)
  target_compile_options(ecg_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
  if(ECG_WARNINGS_AS_ERRORS)
    target_compile_options(ecg_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(ecg_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(ecg_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion)
  if(ECG_WARNINGS_AS_ERRORS)
    target_compile_options(ecg_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(ecg_core
  src/decision.cpp
  src/engine.cpp
  src/evidence.cpp
  src/hash.cpp
  src/json.cpp
  src/json_io.cpp
  src/ledger.cpp
  src/policy.cpp
  src/quantity.cpp
  src/request.cpp
  src/result.cpp
  src/strong.cpp
  src/tariff.cpp
  src/time.cpp
  src/version.cpp)
add_library(ecg::core ALIAS ecg_core)
target_include_directories(ecg_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ecg_core PUBLIC cxx_std_20)
target_link_libraries(ecg_core PRIVATE $<BUILD_INTERFACE:ecg_warnings>)
set_target_properties(ecg_core PROPERTIES
  # CMake does not export ALIAS targets, so the installed package would expose
  # the target as ecg::ecg_core without this. Downstream consumers link ecg::core.
  EXPORT_NAME core
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_definitions(ecg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ecg_core PRIVATE Threads::Threads)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(ecg_core PUBLIC ECG_SHARED_BUILD)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(ECG_BUILD_CLI)
  add_executable(ecg_cli src/cli/main.cpp src/cli/commands.cpp)
  set_target_properties(ecg_cli PROPERTIES OUTPUT_NAME ecg)
  target_link_libraries(ecg_cli PRIVATE ecg_core ecg_warnings)
  target_compile_definitions(ecg_cli PRIVATE ECG_VERSION_STRING="${PROJECT_VERSION}")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ECG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ECG_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------
install(TARGETS ecg_core
  EXPORT ECGTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(ECG_BUILD_CLI)
  install(TARGETS ecg_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ecg
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

install(EXPORT ECGTargets
  FILE ECGTargets.cmake
  NAMESPACE ecg::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ECG)

configure_package_config_file(
  cmake/ECGConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ECGConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ECG)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ECGConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ECGConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ECGConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ECG)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
