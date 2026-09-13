# Diagnostic storage and lifecycle

`DiagnosticLogger::initialize` and the wchar_t logging overloads use only
Win32/standard-library memory before compatibility is accepted. They create no
directory and call no Qt function. Failed compatibility therefore does not
promise a disk log. The retained compatibility report and recent events are
bounded; UI diagnostic copying is an explicit local user action.

Runtime calls `attachStorage(storage, coreRoot)` on Core after constructing the
shared StorageService. The pump's QObject and QTimer are created there. Every
100 ms it admits at most one normal `postAuxiliary` batch to the existing IO
thread. JSON serialization, salt loading/creation, directory scans, rotation and
file writes occur only in that callback. There is no logger thread or worker
pool. The IO-owned StoreOwner is a child of Storage's IO root, so a timeout does
not destroy it or its open run lease from Core.

The pending queue is capped at 512 events and 256 KiB of charged event/string
capacity, including records currently being written. Recent diagnostic memory
is separately capped at 64 events and 64 KiB. Messages are limited to 1,024
characters, code/module/stage to 80, and suggested action to 256. Batch input is
at most 32 KiB, apart from one bounded event. Serialized buffers are separately
flushed at the batch limit; one escaped event can exceed that limit but never
the per-file limit. These are payload budgets, not a claim about exact process
Private Bytes; standard-library container/allocator metadata has an independent
bound from the maximum event count. Source caller-owned QStrings are not owned
by the log queue. Qt strings are truncated before conversion into log records.

Every process run has a cryptographically random name under `logs/run-*` and
holds an exclusive OS file handle on its `.active.lock`. A crashed process
releases that lease through the OS. The logger never reopens an existing run.
`latest.json` is an atomic, small index to the last successful writer, not a
shared append file. No PID liveness guesses or stale-lock deletion are needed.

Normal ceilings are 10 MiB per JSONL volume, five volumes per run, and 100 MiB
for the complete logs directory. Older *closed volumes in the same run* rotate
before a new volume opens. The cross-run directory lease serializes accounting,
reclamation, append and index replacement. Only recognized, ended runs whose
exclusive run lease can actually be acquired are reclaimed, oldest first.
Active runs and unrecognized files are preserved. If active or unrecognized
content consumes the budget, new log writes fail visibly instead of exceeding
the budget or deleting somebody else's live log. The atomic index's temporary
copy is included in admission reservation. Enumeration stops at 2,048 entries;
an oversized/unrecognized directory layout refuses logging with a diagnostic
code. A filesystem call may itself block; the count limit is not a hard disk
latency guarantee. Paths reject traversal and known Windows reparse components;
as with StorageService, this is not a handle-relative defense against a hostile
process racing filesystem reparse replacement between checks.

Events encode level, code, module, taskId, stage, UTC observedAt, message and
suggestedAction. Callers should use `event` for task-specific errors. The legacy
string overloads explicitly use taskId 0 and stage `observe`, since a logger
cannot invent a request identity. Messages are short explanations, never a
channel for protocol/detail JSON. As defense in depth, the logger removes
object/array payloads, sensitive key/value fields, long numeric identifiers and
personal paths before keeping either memory or disk events. Export replaces
the log path with `<data-root>` and retains the redacted compatibility report.
Callers must not put unlabelled secrets in human-readable explanations.

The installation salt is 32 random bytes, created atomically while holding the
directory lease, and reused by later runs. Missing/corrupt/unreadable salt means
`maskedAccount` returns `account-correlation-unavailable`; it never silently
falls back to an unsalted hash. Valid account correlations have a 96-bit digest
under a versioned domain separator. They allow local correlation, not anonymity
of low-entropy identities. Salt and personal paths are excluded from diagnostic
copying; no upload/export network code exists in this module.

Runtime calls `closeStorage` before its existing `storage.shutdown(remaining)`.
It stops the timer, closes log admission and attempts one bounded final IO
callback. It never sleeps or adds a second shutdown budget. If the shared
queue is full or already closing, it returns false, keeps bounded memory and
reports `log_close_not_queued`. Accepted callbacks may finish after the caller's
wait expires; Storage owns that lifetime. The last log line is best effort.
`status`/`diagnosticText` expose saved, pending, dropped, failed and admission
failure counts without pretending queued events have been saved.

`tests/diagnostic_smoke.cpp` uses smaller production-clamped limits. It verifies
rotation and byte budgets, real child-process run-lock contention, active-run
preservation and ended-run cleanup, stable installation salt, bounded scans,
path rejection, queue/backpressure accounting, memory/disk redaction, structured
identity, rejected final admission, and blocked IO surviving the caller timeout.
Its direct DiagnosticStore fixtures run on one test thread; production ownership
is separately exercised through the real shared StorageService callback path.
