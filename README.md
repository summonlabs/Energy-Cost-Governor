# Energy-Cost-Governor

Energy-Cost-Governor (ECG) is a portable C++20 runtime that answers one narrow,
high-consequence question inside a Summon Software Labs Data Center Control Plane:
**given current price, demand-charge, reserve, efficiency, service-class, risk,
capacity, and incident evidence, which energy-cost-sensitive operating decisions
are allowed, refused, deferred, or indeterminate, and why?**

It is a decision engine, not a controller. It produces deterministic outcomes with
complete reason traces and, when it authorises something, a bounded advisory intent
for the authority that actually owns the effect. It never actuates, never
schedules, never moves a workload, and never reports an effect it did not observe.

The repository is independently useful: it builds, tests, installs, and exports a
CMake package with no third-party dependencies. It composes with adjacent DCCP,
ASI, and DFI authorities through explicit typed contracts and nothing else.

---

## Boundary and non-ownership

ECG owns deterministic policy decisions and bounded intents about
energy-cost-sensitive operation. Everything it reasons about belongs to somebody
else, and the code says so:

| ECG owns | ECG does **not** own |
|---|---|
| Policy thresholds, bounds, and rule enablement | Tariffs and price schedules (tariff authority) |
| Decision outcomes and their reason traces | Metering and demand readings (metering authority) |
| Bounded advisory intents | Capacity and flexible-load inventories (capacity authority) |
| The durable decision journal | Reserve requirements (reserve authority) |
| Freshness, epoch, and generation admission rules | Efficiency measurements (efficiency authority) |
| | Service-class definitions and terms (service catalog authority) |
| | Operational risk posture (risk authority) |
| | Incident state and severity (incident authority) |
| | Power actuation, workload scheduling, placement, admission |

Three boundary rules are enforced in the type system and in the tests rather than
in prose:

* **Observation is not ownership.** Evidence carries the authority that produced
  it. ECG validates the shape of what it consumes and never invents a term it was
  not given. An unknown service class is *refused*, not defaulted.
* **An acknowledgement is not an effect.** `intent acknowledge` records that
  another authority received an intent. No decision, cached state, or recovery
  path derives an effect from it.
* **Configured state is not observed state.** Policy is ECG's own; evidence is
  always somebody else's observation, with a source, a generation, and a time.

---

## Outcomes and precedence

Every request is answered with exactly one of four outcomes, computed from the
reason trace by a total, documented precedence: **Refused > Indeterminate >
Deferred > Allowed**.

| Outcome | Meaning |
|---|---|
| **Allowed** | Every required condition held on fresh evidence. A bounded, advisory intent is emitted. This is an authorisation, not an effect: nothing has changed in the facility. |
| **Refused** | Fresh evidence shows a hard constraint is violated. A definitive no that does not depend on any missing input. |
| **Deferred** | Nothing forbids it, but now is not the moment. Always carries `reconsider_at`, the instant at which the question is worth asking again. |
| **Indeterminate** | A required input is missing, stale, recovered, conflicting, or from another epoch. "Cannot decide" is never a permissive answer. |

Refusal outranks indeterminacy because a definitive violation does not become less
definitive when an unrelated input is stale. Indeterminacy outranks deferral
because "cannot say" is a weaker claim than "not now".

**Economic desirability is reported separately from authorisation.** Every decision
carries an `EconomicAssessment` (baseline cost, candidate cost, projected saving,
avoided demand charge, and the shift target, all in exact integers). Only an
explicit policy switch (`require_positive_economic_benefit`) turns an unfavourable
assessment into a refusal, and reserve restoration is never refused on economic
grounds at all.

---

## Architecture

```
include/ecg/            public headers (the whole API surface)
  version.hpp           library, encoding, and journal-format versions
  result.hpp            Error taxonomy and Result<T>; no exceptions on the decision path
  strong.hpp            identities, generations, epochs, revisions, validated names
  quantity.hpp          exact fixed-point quantities with checked arithmetic
  hash.hpp              FNV-1a 64 (chain) and CRC-32C (frame integrity)
  canonical.hpp         canonical binary encoding: no padding, layout, or locale
  time.hpp              UTC instants, half-open intervals, explicit zone rules
  json.hpp              strict JSON dialect with exact decimal numbers
  json_io.hpp           domain <-> JSON, with self-verifying decisions
  tariff.hpp            price schedules and demand-charge rules
  evidence.hpp          evidence envelopes, provenance, freshness
  policy.hpp            thresholds, bounds, ordered rule pipeline
  request.hpp           the questions ECG may be asked
  decision.hpp          outcomes, reason codes, intents, economics
  engine.hpp            GovernorEngine::Evaluate (pure, thread-safe, no I/O)
  ledger.hpp            durable journal, recovery, idempotency, writer lock
src/                    implementation, plus src/cli for the ecg executable
tests/                  14 suites, 223 tests, one executable per suite
benchmarks/             labelled measurement of completed work
examples/               independent out-of-tree find_package consumer
scripts/                install-and-consume validation
docs/                   concurrency and ownership audit
```

The decision path is a pure function:

```
GovernorEngine::Evaluate(policy, request, evidence, now) -> Decision
```

It holds no mutable state, takes no lock, performs no I/O, contacts no other
runtime, and allocates only bounded storage. That is what makes it safe to call
from many threads and what makes a recorded decision replayable.

Inside, an ordered rule pipeline runs to completion rather than short-circuiting,
so a trace explains every constraint that mattered:

```
request contract -> policy generation -> epoch consistency -> evidence presence
-> evidence freshness -> evidence validity -> window coverage -> service terms
-> reserve floor -> capacity -> risk -> incident -> demand charge -> economics
-> intent bounding -> outcome
```

---

## State, evidence, authority, and freshness

Every observation is an envelope:

```json
{
  "meta": {
    "authority": "metering_authority",
    "source": "meter.main",
    "generation": 9,
    "epoch": 1,
    "observed_at": "2026-03-02T11:59:00.000000Z",
    "published_at": "2026-03-02T11:59:00.000000Z",
    "payload_version": 1,
    "provenance": "live_observation"
  },
  "value": { "current_demand_kw": 9200, "rolling_peak_kw": 9500, "interval_length_s": 900, "...": "..." }
}
```

* **Generations** are monotonic counters with their own identity domains. A
  request carries the policy generation it was formed against; a stale one is
  `Indeterminate`.
* **Epochs** are the authority's own timeline. A submission from an older epoch is
  refused at the ledger, not evaluated.
* **Provenance** distinguishes a live observation, state recovered from
  persistence, and a synthetic model. **Recovered dynamic evidence is never
  promoted to current**: the engine refuses to authorise anything on it, however
  recently it was written.
* **Freshness** is classified per category against policy age limits, at full
  microsecond precision. Missing, stale, future-dated, recovered, epoch-mismatched,
  unsupported-version, and internally contradictory payloads are all explicit
  states. None of them is permissive.
* **Absence is explicit.** Missing evidence is `null`/empty optional, never a
  zero that happens to look safe.
* **Numeric exactness.** Prices, energy, money, ratios, and durations are scaled
  integers with checked arithmetic throughout. Binary floating point is not used
  anywhere in the decision path, so the same inputs produce the same bytes on
  every host.
* **Time-zone safety.** All arithmetic is in UTC. Wall-clock recurrences such as a
  weekday evening demand window are resolved through an explicit, evidence-supplied
  zone transition table, never the host time-zone database. Nonexistent local
  times (spring-forward gaps) and ambiguous ones (fall-back overlaps) have
  documented, deterministic resolutions.

A decision is content-addressed: its identity is the digest of its own content.
`ParseDecision` recomputes that digest and rejects a decision whose bytes do not
match, so a journal entry altered in storage cannot be read back as valid.

---

## Build and test

Requirements: CMake 3.28 or newer, a C++20 compiler, and (for the presets) Ninja.
No third-party libraries are used or fetched.

```sh
cmake --preset debug            # Debug, strict warnings, warnings-as-errors
cmake --build --preset debug
ctest --preset debug

cmake --preset release          # Release, strict warnings
cmake --build --preset release
ctest --preset release

cmake --preset release-bench  # same as release, plus the benchmark target
```

Options: `ECG_BUILD_CLI` (default ON), `ECG_BUILD_TESTS` (default ON at top
level), `ECG_BUILD_BENCHMARKS` (default OFF), `ECG_WARNINGS_AS_ERRORS`
(default ON).

Compilers are held to `/W4 /permissive- /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast -Werror` elsewhere. First-party code is warning-free in Debug
and Release.

---

## Command line

```console
$ ecg version
energy-cost-governor 1.0.0
1.0.0 (MSVC 19.44, Windows x64)
journal format 1, encoding 1
```

Ask for a decision. The input document carries the request, the evidence the
adjacent authorities are currently publishing, and the evaluation instant:

```console
$ ecg decide --state ./state --input decide.json --now 2026-03-02T12:00:00.000000Z
outcome: deferred
decision_id: 8ee1f0f5e9db0f31
request_id: 00000000000001f5
client_id: ops.duty
kind: shift_flexible_load
policy: ecg.default@1 epoch=1
decided_at: 2026-03-02T12:00:00.000000Z
reasons:
  [notice] window_beyond_horizon window: the desired window begins at 2026-03-03T18:30:00.000000Z, beyond the decision horizon ending at 2026-03-03T00:00:00.000000Z
  ...
reconsider_at: 2026-03-03T06:30:00.000000Z
```

Other commands:

```
ecg decide                 --state <dir> --input <file|-> [--now <rfc3339>] [--policy <file>] [--json]
ecg state import           --state <dir> --input <file|-> [--epoch <n>] [--now <rfc3339>]
ecg state show             --state <dir> [--json]
ecg journal verify         --state <dir> [--json]
ecg journal show           --state <dir> [--limit <n>] [--json]
ecg epoch advance          --state <dir> --epoch <n> [--now <rfc3339>]
ecg intent acknowledge     --state <dir> --decision <hex> --intent <hex> --by <client>
ecg selftest               [--json]
```

Exit codes are part of the automation contract, so a script never has to parse
output:

| Code | Meaning |
|---|---|
| 0 | Allowed, or the command succeeded |
| 3 | Refused |
| 4 | Deferred |
| 5 | Indeterminate |
| 2 | Usage, malformed input, or an inadmissible (stale-epoch) submission |
| 6 | Persistence failure or the writer lock is held by another process |
| 7 | Integrity failure: corrupt, inconsistent, or unsupported journal content |
| 1 | Internal failure |

---

## Library use

```cpp
#include "ecg/engine.hpp"
#include "ecg/json_io.hpp"

ecg::PolicySet policy = ecg::DefaultPolicy();

auto engine = ecg::GovernorEngine::Make(policy);          // validates the policy
if (!engine.ok()) { /* policy refused */ }

ecg::EvidenceSet evidence = /* from the adjacent authorities */;
ecg::DecisionRequest request = /* kind, magnitude, duration, window */;

auto decision = engine.value().Evaluate(request, evidence, now);
if (!decision.ok()) {
  // The request or the evidence bundle violates its own contract.
} else if (decision.value().outcome == ecg::Outcome::kAllowed) {
  const ecg::BoundedIntent& intent = *decision.value().intent;  // advisory_only == true
}
```

Durability and idempotency go through the ledger:

```cpp
auto ledger = ecg::DecisionLedger::Open("./state", policy);   // takes the writer lock
auto committed = ledger.value()->Submit(request, evidence, now);
// committed.value().duplicate == true means this (client, request) was already
// committed and the stored decision was returned instead of a second one.
```

---

## Persistence, recovery, and concurrency

The journal is a versioned, integrity-checked, append-only file.

* **Commit point.** A record is committed when its frame is fully written and
  flushed to stable storage. Nothing before that is durable; everything after it
  is.
* **Frame integrity and chaining.** Each frame is length-prefixed, CRC-32C
  protected, and chained to its predecessor by a hash of the previous frame. A
  frame whose binary envelope and JSON body disagree is rejected.
* **Torn tail versus interior corruption.** Truncated or zero-filled trailing
  bytes are recovered by discarding the incomplete tail. A *complete* frame that
  fails its integrity check is corruption wherever it sits, and the ledger refuses
  to open rather than silently dropping committed history.
* **Atomic publication.** The state snapshot is written to a temporary file,
  flushed, and renamed over the previous snapshot, so a reader sees either the old
  complete file or the new one. A snapshot claiming a journal position the journal
  has not reached is deleted and reported, never trusted.
* **Idempotency in the same commit.** The `(client, request)` identity lives
  inside the decision frame, so a retry after a crash either finds the original
  decision or re-evaluates it; it can never append the same decision twice.
* **Replay rejection.** The authority epoch only moves forward, is checked before
  evaluation and again while holding the writer lock, and a submission from an
  older epoch is refused.
* **One writer per machine.** A kernel-enforced exclusive lock
  (`LockFileEx`/`flock`) is held for the ledger's lifetime and released by the
  kernel however the process exits.

The concurrency and lock-ownership audit, including the defect it found, is in
[docs/concurrency-audit.md](docs/concurrency-audit.md).

---

## Validation: what was actually run

Everything in this section was executed on the development machine
(Windows 11, MSVC 19.44.35207, CMake 4.3.2, Ninja 1.13.2) against the sources in
this repository unless it is explicitly labelled otherwise.

**Test suite — REAL.** `ctest --preset debug` and `ctest --preset release`:
**14 suites, 226 tests, 0 failures**, including

| Suite | Tests | What it proves |
|---|---|---|
| `test_strong_and_quantity` | 24 | Typed identities, exact decimal parsing, checked arithmetic including 128-bit `(a*b)/d` against an independent route |
| `test_hash_and_canonical` | 14 | Known-answer hashes, canonical encoding round-trips and rejections |
| `test_json_dialect` | 18 | The strict dialect: exact numbers, canonical dumps, and every documented rejection |
| `test_time_and_zones` | 28 | Civil-time round-trips, interval algebra, DST gap/overlap resolution, recurrence expansion |
| `test_tariff_coverage` | 15 | Covered/gap/ambiguous pricing, schedule self-contradiction |
| `test_evidence_freshness` | 13 | Age boundaries, future dating, recovered and epoch-mismatched evidence |
| `test_engine_decisions` | 45 | Every outcome, every rule, deterministic ordering, replay identity |
| `test_engine_property` | 10 | 2 000 seeded randomized decisions plus adversarial inputs and numeric extremes |
| `test_json_roundtrip` | 12 | Exact round-trips and digest-based tamper detection |
| `test_ledger_persistence` | 14 | Commit visibility after reopen, idempotency, epoch admission, snapshots |
| `test_ledger_recovery` | 11 | Torn tails, zero-filled tails, interior corruption, header damage, read-only verification |
| `test_concurrency` | 6 | Shared evaluation, racing identical submissions, epoch races, verification during writes |
| `test_multiprocess` | 7 | Real second processes: lock exclusion, cross-process durability, abrupt process death |
| `test_cli_end_to_end` | 9 | The real executable: exit codes, journalling, duplicate detection, acknowledgements |

**Determinism — REAL.** Identical inputs produce byte-identical decision JSON and
identical decision identities, including across a real process restart.

**Synthetic inputs — SYNTHETIC.** All evidence used by the tests, the benchmarks,
and the examples is a synthetic model: internally consistent, hand-written, and
**not** a reading from any facility, meter, BMS, or DCIM. No real telemetry,
electrical, cooling, or multi-node data was available or is claimed.

**AddressSanitizer — UNSUPPORTED.** The MSVC installation on the development
machine does not provide the ASan runtime libraries
(`clang_rt.asan_static_runtime_thunk-x86_64.lib` is absent), so an instrumented
build could not be linked. This is a toolchain gap, not a claim of cleanliness;
the Debug builds use the checked debug runtime, and the concurrency audit covers
the ownership questions a sanitizer would otherwise have to catch.

**POSIX — UNSUPPORTED.** The portable code paths (including the `flock` writer
lock and `posix_spawn` helpers) are written and compile-conditional, but nothing
in this repository has been built or run on a POSIX host. Only the Windows path is
validated.

---

## Benchmarks

Methodology: `benchmarks/bench_engine.cpp` measures **completed work**, not loop
iterations. Each timed iteration evaluates a request and consumes the resulting
outcome, reason trace, evidence dependencies, intent, and digest into a running
value, so nothing can be optimised away. Wall-clock results are reported with mean,
median, and p99, after a separately reported warm-up. Every line is labelled
`REAL`, `SYNTHETIC`, or `UNSUPPORTED`.

Release build, MSVC 19.44, Windows 11:

```
decision path -- SYNTHETIC input, real engine work
  completed decisions : 200000
  mean                : 9.802 us per completed decision
  median / p99        : 8.500 us / 30.200 us
  throughput          : 97211 completed decisions/s

durable ledger -- REAL local filesystem, one flush to stable storage per commit
  commits completed   : 200, journal sequence 1..200
  per-commit mean     : 1.370 ms (median 1.109 ms)
  journal             : 889759 bytes, re-read and chain-verified: 200 frames
```

Repeated runs on the same machine gave a stable median of 8.5-10.7 us per
decision and a mean of 9.8-12.1 us; the mean is tail-dominated by scheduler
noise, which is why the median is reported alongside it. Per-commit latency for
the durable path varied between 1.16 ms and 4.15 ms depending on how much other
work the machine was doing, because each commit performs a real flush to stable
storage.

The decision figure is **SYNTHETIC evidence driving real engine work**: the
arithmetic, the rule pipeline, and the digesting are real; the inputs are a model.
The ledger figure is **REAL**: real files, real flushes, and every frame re-read
and chain-verified afterwards. Timings are indicative of this machine only; they
are not a capacity claim for any other host.

---

## Install and downstream use

```sh
cmake --preset release -DECG_BUILD_TESTS=OFF -DECG_BUILD_BENCHMARKS=OFF
cmake --build --preset release
cmake --install build/release --prefix /path/to/prefix
```

The install provides the library, the public headers, the `ecg` executable, and a
CMake package. An independent out-of-tree consumer needs only:

```cmake
find_package(ECG CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE ecg::core)
```

`examples/find_package_consumer` is exactly such a project: it is not part of the
parent build, it links the installed package, and it asserts both a successful
authorisation and the indeterminate result produced by stale evidence. The
full install-and-consume proof, including exact commands and a PASS/FAIL summary,
is `scripts/validate_install.ps1`.

---

## Limitations and non-goals

* ECG does not schedule, place, admit, actuate, or measure anything, and it holds
  no credentials or connections to any facility system.
* Demand-charge, reserve, and efficiency modelling is deliberately conservative
  and policy-driven; it is not a utility billing engine and does not compute a
  bill.
* The economics module optimises over published tariff intervals only. It does not
  model grid services, ancillary markets, carbon, or contractual penalties beyond
  the demand charge it is given.
* Recourse to a real facility's behaviour is out of scope: an intent is advisory,
  and whether anything happens is another authority's decision and another
  authority's evidence.
* Only Windows builds are validated in this repository; the POSIX paths are
  unexercised.
* No AddressSanitizer run was possible on the development toolchain.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
