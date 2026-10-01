# Contributing to Energy-Cost-Governor

Energy-Cost-Governor is a portable C++20 library and command line tool, built with CMake, that
evaluates energy-cost policy decisions for data center control planes. Bug reports, documentation
fixes, tests, and code contributions are welcome.

## Licensing

Energy-Cost-Governor is released under the Apache License, Version 2.0 (see [LICENSE](LICENSE)).
By submitting a contribution you agree that it is licensed under those same terms.

- No Contributor License Agreement (CLA) is required.
- No copyright assignment is required; you keep the copyright on your contribution.
- Contributions are inbound=outbound: the project accepts them under Apache-2.0 and distributes
  them under Apache-2.0, with no additional terms.

Do not edit LICENSE or NOTICE as part of a feature contribution. If you believe either file is
incorrect, raise it separately before proposing a change.

## Building

Requirements: CMake 3.28 or newer, a C++20 compiler (MSVC, GCC, or Clang), and Ninja.

```
cmake --preset debug
cmake --build --preset debug
```

Release builds use the matching preset:

```
cmake --preset release
cmake --build --preset release
```

## Testing

The test suite is registered with CTest and runs through the same presets:

```
ctest --preset debug
ctest --preset release
```

Run both configurations before opening a pull request. A change that passes in only one of them is
not ready to merge.

## Warnings policy

First-party targets are compiled with strict warnings, and first-party warnings are errors by
default (`ECG_WARNINGS_AS_ERRORS`):

- MSVC: `/W4 /permissive- /WX`
- GCC and Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion ... -Werror`

Contributions must compile warning-free in both Debug and Release. Do not silence a warning by
lowering the warning level, disabling a warning globally, or adding a blanket suppression. Fix the
underlying issue, or narrow the suppression to the smallest possible scope and explain in a comment
why it is correct.

## Code expectations

- Portable C++20. Avoid compiler-specific extensions and any reliance on a single standard library
  implementation or platform.
- No new third-party dependencies without a written justification in the pull request: what the
  dependency provides, why the standard library is insufficient, and its license.
- Prefer strong types over raw scalars. Distinguish units and identifiers at the type level rather
  than by naming convention.
- On the decision path, report failure through explicit values. Exceptions are not used for
  expected, recoverable outcomes.
- Deterministic behaviour. Results must not depend on wall-clock time, locale, pointer or object
  addresses, hash iteration order, or unordered container traversal order.
- Bounded resource use. Bound memory, recursion, and input sizes, and reject oversized or malformed
  input explicitly instead of allocating until failure.
- Match the surrounding code: existing naming, file layout, and error handling conventions.

## Tests

Every behaviour change needs a test that fails without it.

- Cover persistence and restart behaviour whenever serialized state is involved.
- Cover corrupted, truncated, and inconsistent input whenever state is parsed or loaded.
- Cover concurrency behaviour whenever a change touches shared state or threading.

## Commits and pull requests

- One logical change per pull request. Split unrelated fixes and refactors into separate requests.
- Commit messages are concise and neutral: a short imperative summary, plus a body only when the
  reason for the change is not obvious from the diff.
- Messages are public-facing. Write them for a reader who has no context beyond the repository.
- Commits must be authored solely by the human contributor. Do not add `Co-authored-by` trailers,
  attribution trailers, or generator and tool credits of any kind.
- Describe the observable behaviour change and how it was verified, including the presets you built
  and the tests you ran.
- Keep the diff focused: no drive-by reformatting, and no unrelated edits in the same pull request.

## Reporting problems

Open an issue with the observed behaviour, the expected behaviour, and the smallest input that
reproduces the problem. For build failures, include the compiler and version, the preset used, and
the full error output.
