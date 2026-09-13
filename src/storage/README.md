# Storage foundation and initial repository write integration

`StorageService` is the Core-side facade for one fixed, already resolved data
root. It starts exactly one I/O thread. The worker QObject and its QTimer are
constructed in that thread's `run()` and destroyed there after its event loop
has stopped. File writes, reads, hashing, account-lock acquisition/release and
shared-file locks run on the I/O thread. The facade's `completed` signal is
delivered on the facade's owning thread.

## Write integration

1. Create an account capability with
   `createAccountContext(accountKey, alreadySafeDirectoryName)`. The final scope
   is `<fixedDataRoot>/accounts/<alreadySafeDirectoryName>`; it is never looked
   up from a mutable current-account property when a job executes.
2. Keep the capability while that account is active. Each accepted `StoreWrite`
   copies the capability, absolute path, revision and implicitly shared byte
   content. Changing the caller's request afterwards cannot change the job.
3. Submit with `submitWrite`. Check admission immediately: queue-full and other
   rejection leave the unsaved bytes with the caller. Admission is `Queued`,
   never a claim that a record is already saved.
4. Only `completed(... Saved ...)` means QSaveFile wrote the complete byte array
   and committed it. Failed or superseded tasks also produce one completion.
   A failed write may be retried with the same revision and identical content;
   different bytes require a new revision. A committed duplicate is skipped.
5. On account change, release the caller's old capability. Queued and executing
   jobs keep the old scope alive. The old account's `.storage-writer.lock` is
   not released while another live or queued capability for the same directory
   remains. A new process cannot acquire it before those tasks have drained.

Account locks are acquired lazily before the first write. Acquisition failure
produces `LockUnavailable` and does not change the record. A context in another
StorageService cannot be reused in this one. Shared files use a separate
`createSharedContext()` capability and a short `<target>.lock` around each
atomic write; that capability cannot address `accounts/`.

All writers of an account must migrate to the same service/lock contract before
claiming process-wide account ownership. The initial integration connects
PetRepository's inventory/detail/last-account and legacy migration writes.
Its bootstrap/account/detail reads now use the same I/O service with bounded
paging and selection priority. The remaining controllers and operations journal
must also use the shared service before claiming complete account ownership.

PetRepository accepts an injected service or creates its own default facade.
`storageService()` and `storageContext()` let Application share the same owner.
`hasCachedDetail()` means a complete detail exists in memory;
`isDetailPersisted()` separately checks whether that memory revision is saved.
Network detail admission emits `detailPersistenceQueued(id, generation, taskId)`
so the request controller can stop its network timeout without claiming disk
success. A zero taskId announces valid observation with failed admission, before
the corresponding persistence rejection; it must not cause a network retry.
`detailResponseAccepted` is emitted only after a real `Saved` result.
Movement preservation uses `preserveBackpackDetailAsync` and waits for
`detailPreservationFinished`; its legacy bool query never pretends admission is
synchronous persistence. Old-session writes retain their old target and lease,
but their completions do not notify the new repository view.

## Reads and bounded work

`StoreRead.selected` gives a user-selected record priority over background
reads. Required writes precede selected reads; normal writes follow selected
reads. An executing file operation is never preempted by a second I/O thread.
Core supplies `expectedMemoryRevision`; a `Loaded` completion echoes that tag.
Core must still validate its pending task, account/session and current record
revision before applying returned bytes. This tag is not a disk timestamp or a
claim that the file is newer than live data.

Defaults: 256 outstanding jobs, 32 MiB of outstanding write/read-buffer
reservations, 16 MiB per record, and batches of at most 8 jobs, 1 MiB of reserved
bytes or 8 ms between file operations. Reservations include results queued back
to Core, so a blocked Core event loop cannot create an unbounded result queue.
These limits do not include the caller's own retained inputs/results. A single
filesystem call can exceed the batch time budget and cannot be forcibly
interrupted safely.

Paths reject traversal, absolute paths, alternate streams, device names,
reserved lock filenames and known symlink/junction/reparse components. Existing
parents are checked before directory creation and again before writing. This is
not a handle-relative defence against a hostile process racing directory
replacement between filesystem calls; such an adversarial threat model would
require a different Windows file-opening implementation.


Directory scans return at most 128 names per page and retain an opaque cursor
owned by the I/O thread. `cancelReads(context)` closes that consumer's cursors
and cancels queued reads without cancelling accepted writes. Explicit legacy
read contexts grant no write capability. KQPET_DATA_ROOT overrides default to
`<thatRoot>/legacy-import`, so previews/tests do not inspect personal legacy
folders; callers can supply a different explicit legacy root when needed.

Legacy migration advances only after each real write result and writes its
completion digest last. Conditional writes never replace an existing target or
supersede a newer accepted normal write. A failed newer write with no target does
not count as completed migration. Queue-full retries keep the current item;
actual failures leave the source and omit the completion marker for a future
attempt. No old detail file is deleted.

## Shutdown

`shutdown(2000)` closes admission, cancels queued reads and drains accepted
writes. It returns true only after the worker stops. If a filesystem call is
still blocked, it returns false while the thread, runtime, byte payloads and
account leases remain alive. Destroying the facade after an explicit shutdown
does not spend another two-second budget. A thread that eventually finishes
uses `deleteLater` on its owning thread; if that event loop has already stopped,
its small thread wrapper can remain until process exit. No running QThread is
destroyed and no thread is forcibly terminated.

The optional Writer callback is solely an explicit test/fault-injection seam.
Its captures must outlive any timed-out I/O, preferably through shared owners;
production callers should use the default strict QSaveFile implementation.

## Build and tests

Library sources: `storage_service.cpp`, `storage_write_context.cpp`; public
headers: `storage_service.h`, `storage_write_context.h`, `storage_types.h`.
Enable AUTOMOC, include `src/storage`, and link Qt 6.6.3 Core. The standalone
`tests/storage_queue_smoke.cpp` needs only this library and Qt Core.

The smoke test exercises frozen caller data, out-of-order revisions and equal
revision conflicts, bounded outstanding jobs/bytes, selected-read priority,
returned revision tags, cross-process account-lock contention, account-context
replacement, real Windows junction rejection, real QSaveFile replacement
failure preserving old bytes, shared-file lock duration, and a blocked I/O
thread surviving the complete two-second shutdown budget.
