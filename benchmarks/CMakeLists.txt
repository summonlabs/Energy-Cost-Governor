# Benchmarks.
#
# One executable, no third-party dependencies. It is never part of the default
# build: ECG_BUILD_BENCHMARKS is OFF unless a caller asks for it, and the target
# is never installed or exported.
#
# ecg_bench carries one deliberate obligation beyond measuring: its input is a
# hand-written SYNTHETIC model, and every run says so. It must never be read as
# evidence about a real facility, a real meter, or a real BMS/DCIM feed.

add_executable(ecg_bench bench_engine.cpp)

# The shared first-party warning policy (/W4 /WX on MSVC, -Wall -Wextra -Werror
# elsewhere). ecg_core links it privately, so the benchmark asks for it directly.
target_link_libraries(ecg_bench PRIVATE ecg_core ecg_warnings)

set_target_properties(ecg_bench PROPERTIES
  # The binary name is part of the automation contract used by the runbook.
  OUTPUT_NAME ecg_bench)
