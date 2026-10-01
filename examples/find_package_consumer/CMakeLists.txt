# Out-of-tree consumer of the installed Energy-Cost-Governor package.
#
# This project is standalone on purpose. It is NOT added to the parent build --
# no add_subdirectory, no CTest registration, no shared cache entry -- because
# the thing it has to prove is precisely that an independent project can consume
# an INSTALLED package through find_package(ECG CONFIG) and link ecg::core.
#
# Build it from its own build directory against an install prefix:
#
#   cmake -S examples/find_package_consumer -B <build-dir> -G Ninja
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build <build-dir>
#   <build-dir>/ecg_find_package_consumer
#
# scripts/validate_install.ps1 does exactly that, end to end, against a real
# cmake --install prefix.

cmake_minimum_required(VERSION 3.28)

project(ECGFindPackageConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent out-of-tree consumer of the installed Energy-Cost-Governor runtime"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The whole point: the package is found through its installed config file, and
# the imported target carries the include directories, the C++20 requirement,
# and the library itself.
find_package(ECG CONFIG REQUIRED)

# The installed package must export the namespaced target ecg::core. Say so in
# one place, with the fix, instead of letting the linker fail with a message
# that does not name the cause.
if(NOT TARGET ecg::core)
  message(FATAL_ERROR
    "The installed ECG package did not export ecg::core. An ALIAS target is never "
    "exported by CMake, so the package must set EXPORT_NAME on the real target "
    "(set_target_properties(ecg_core PROPERTIES EXPORT_NAME core)) or install it "
    "under that name.")
endif()

add_executable(ecg_find_package_consumer main.cpp)
target_link_libraries(ecg_find_package_consumer PRIVATE ecg::core)

# A consumer's own warning policy. Deliberately not /WX: a downstream project
# owns its warning budget, and the installed package must not dictate it.
if(MSVC)
  target_compile_options(ecg_find_package_consumer PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(ecg_find_package_consumer PRIVATE -Wall -Wextra)
endif()
