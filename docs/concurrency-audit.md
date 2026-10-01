# Concurrency and ownership audit

This document is a code-level audit of every lock, thread, and shared mutable
value in Energy-Cost-Governor. It is written from reading the call paths, not
from the fact that the tests pass: a test can only observe an interleaving it
happens to hit.

## What is shared, and what is not

| Component | Shared mutable state | Synchronisation |
|---|---|---|
| `GovernorEngine` | none | none required |
| `Evaluation` (per call) | none | stack-local |
| `DecisionLedger` | journal handle, hash chain, sequence, epoch, dedupe map | one `std::mutex` + kernel file lock |
| `ecg` CLI | process-local only | none required |

`GovernorEngine::Evaluate` is a pure function of policy, request, evidence, and
the evaluation instant. It takes no lock, performs no I/O, mutates nothing, and
allocates only bounded storage. There is therefore no lock for it to hold
incorrectly, and N threads calling it concurrently produce results bit-identical
to one thread calling it N times. That property is asserted directly by
`concurrency.engine_evaluation_is_shareable_across_threads`.

## Lock inventory

There are exactly two synchronisation mechanisms in the runtime.

1. **`DecisionLedger::mutex_`** (process-local, non-recursive). Guards the
   journal write handle, the hash chain, the last sequence, the authority epoch,
   the last recorded instant, and the in-memory idempotency map. It is the only
   mutex in the library.
2. **The kernel writer lock** (`writer.lock`, `LockFileEx` on Windows,
   `flock` on POSIX). Held for the lifetime of an open ledger and released by
   the kernel when the owning process exits, however it exits.

## Call-path audit

### Self-deadlock and recursive acquisition

`mutex_` is never acquired twice on one path. The public entry points
`Submit`, `Commit`, `AdvanceEpoch`, `PublishState`,
`RecordAcknowledgement`, `RecoveredDecisions`, `Verify`, and `Close` each
take it exactly once at their outermost level, and every internal helper they
call (`AppendRecord`, `Flush`, `EncodeFrame`, `Remember`,
`FindCommitted`, `Replay`) assumes the lock is **already held or not needed**
and never acquires it. The narrowed accessors `recovery()`,
`current_epoch()`, and `last_sequence()` do take the lock, and no
lock-holding path calls them: this is why they are accessors on the ledger rather
than direct field reads, and it is checked by inspection of every call site.

The mutex is non-recursive, so a future recursive acquisition would deadlock
immediately and deterministically rather than silently corrupting state. No code
path currently relies on reentrancy.

### Read to write upgrade

There is no read/write lock anywhere in the runtime, so the classic
read-guard-to-write-guard upgrade deadlock cannot occur. Shared-read access is
achieved by immutability (the engine) rather than by a reader lock, which is a
stronger guarantee: there is no guard that could be held while a write is
attempted.

### Write locks held across re-entry

No callback, virtual function, or user-supplied code is invoked while
`mutex_` is held. `AppendRecord` calls only the encoder, the hash, and
`std::fwrite`/`FlushFileBuffers`. `PublishState` performs the atomic rename
while holding the lock, which is deliberate: it keeps the published snapshot
position and the journal position consistent. The cost is that the rename is
serialised with submissions, and that is documented rather than hidden.

### Inconsistent lock ordering

There is one process-local lock and one kernel lock, and the kernel lock is
acquired first (during `Open`) and released last (during `Close`). There is
no path that takes them in the opposite order, so no ordering inversion is
possible. A second process never blocks on `mutex_`; it is refused immediately
by `LockFileEx`/`flock` before any in-process state exists.

### Shutdown while holding worker-required state

The runtime starts no background threads of its own; there are no worker pools,
no asynchronous callbacks, and nothing to join. `Close()` takes `mutex_`,
flushes and closes the journal, releases the kernel lock, and is idempotent. A
caller that owns worker threads must join them before closing, which is what the
tests do and what the CLI's single-threaded flow makes trivial. Because the
runtime holds no thread, a caller can never be left waiting on a worker that is
itself waiting on a ledger lock.

### Cancellation races

The library exposes no cancellation token; `ErrorCode::kCancelled` exists for
callers that wrap the API. A cancelled submission is simply a submission that was
never made: the only durable effect of `Submit` is the append, and the append is
a single `fwrite` followed by a flush. There is no partially applied operation
to cancel.

### Stale-authority races

Epoch admission is checked **twice**: once before evaluation, so an obviously
stale submission costs nothing, and again inside `Commit` while the lock is
held, so a concurrent `AdvanceEpoch` cannot slip between the check and the
append. A decision can therefore never be appended under an epoch that was not in
force at append time. This is asserted by
`concurrency.epoch_advance_never_admits_a_stale_decision`, which runs
submitters and an epoch advancer at the same time and then requires the journal
to verify.

### Concurrent publication

Two publications exist. The journal append is serialised by `mutex_` and
committed by a flush, so it is a single-writer operation by construction. The
state snapshot is published by writing a temporary file, flushing it, and
renaming it over the target, so a concurrent reader sees either the previous
complete snapshot or the new complete snapshot and never a partial file. The
snapshot records the journal position it corresponds to, and a snapshot claiming
a position the journal has not reached is deleted on open and reported rather
than trusted.

## Defect found by this audit

Reading the accessors rather than the tests exposed a real data race: the
previous `recovery()`, `current_epoch()`, and `last_sequence()` returned
references to, or copies of, fields written by concurrent appends without taking
the lock, and `LoadState()` read the last sequence without the lock. Under
concurrent submission these were unsynchronised reads of non-atomic values:
undefined behaviour that a functional test would not reliably catch. The
accessors now take the writer lock and return by value, and `LoadState` copies
the journal position under the lock before comparing it with the snapshot.
