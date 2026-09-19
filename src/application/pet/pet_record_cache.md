# Raw records, memory versions and bounded retention

`PetRecordCache` is Core-owned. Its default raw payload ceiling is 64 MiB,
with a 32 MiB per-record admission ceiling, 16,384 resident entries, at most
200,000 JSON nodes per record and depth 64. Its meter walks JSON values without
serializing them: node/container/key/string charges are conservative logical
payload charges, not process Private Bytes. Packet decoding buffers, compact
inventory/identity metadata, and consumer-owned display values have their own
lifetimes/budgets; this counter does not claim to measure the entire process.

Each accepted original is stored in one immutable RawPetRecordPayload. Its
lease remains charged until the last RawPetRecordHandle releases it, including
after LRU eviction, replacement and account switching. Digest/brief metadata
updates create small immutable wrappers over that same payload. The cache
cannot reclaim charged bytes simply by removing an entry still held by GUI,
Compute or IO. Admission is refused if remaining leases occupy the budget.

Unpersisted originals and records whose derivation consumer has not called
`markRecordDerived(key)` are protected from LRU. Compact facts alone do not
justify discarding an unsaved original: a later algorithm/metadata version may
need fields the facts omitted. On account reset the cache releases its own
scope, while outstanding IO/Compute/UI handles retain their original account,
epoch, memory version and charge. No file is deleted by raw-cache eviction.

## Repository contracts

- `PetRecordKey` freezes account, epoch, instanceId and detailMemoryRevision.
  The last field is independent of ordered Storage write revision. It changes
  for accepted new raw facts, source changes and calculation-relevant brief
  overrides; identical detail facts reuse the memory key.
- `briefFor(id)` / `recordBrief(id)` are O(1) compact current observations;
  `backpackBriefs` / `warehouseBriefs` preserve sorted legacy display order.
  `currentInstanceIds()` deduplicates both maps in O(P), without name sorting.
  The two inventory maps contain scalar summaries and normalized move flags,
  never the full cultivation/relationship tree.
- `rawRecordHandle(id)` returns a leased original when resident. A member
  which has never had complete detail can return a versioned summary handle
  with complete=false and payload=null. A formerly complete, evicted record
  returns no raw handle; consumers must use valid compact facts or restore the
  original, never overwrite existing knowledge with a newly invented Unknown.
- `recordVersion(id).complete` is the analysis coverage flag. A backpack list
  needs czdlv and mzdlv objects to establish full detail; individual missing
  cultivation values still remain Unknown in Domain. The legacy bool
  `hasCachedDetail` also recognizes a persisted partial original, preserving
  its former use as a cache/preservation check. Analysis must use complete,
  not that legacy bool. Residency and durability are separate flags.
- `rawRecordAvailable(handle)` includes later IO digest completion using a new
  small wrapper. `rawRecordEvicted(account,epoch,id,memRev)` reports residency
  changes without erasing known/saved state. `rawCachePressure` is explicit;
  network originals that cannot be retained are rejected and invalidate the
  session instead of being reported Saved.
- `requestCachedDetail(id)` tests residency, so known-but-evicted records can
  be restored with selected-read priority. `rawRecordLoadFinished(requested,
  loaded,error)` terminates the exact frozen request key. Old epoch/memory
  reads cannot cover a later network observation. A formerly known original
  requires the expected file digest; missing/corrupt/different files report
  failure without changing complete to false. Temporary capacity pressure
  retries on the Core timer and resumes when derivation releases protection.

Content digests come from Storage's actual Loaded/Saved byte stream; Core
never serializes a large JSON object to hash it. Saved without a 32-byte
digest is rejected as unconfirmed persistence, and does not release the raw
durability protection. PendingWrite holds the original handle until the IO
completion is consumed, so a Storage JSON value cannot outlive its raw charge.

The current schema 3 inventory writer stores summaries. Verified backpack
originals are independently submitted to details/<id>.json, including partial
originals with explicit complete=false. This is local IO, not an added game
query. Loading such a file preserves false coverage. A partial backpack list
never overwrites an old complete original: only losslessly representable
summary changes are accepted. An additional/changed field which cannot be
represented or checked against the old original rejects the list and leaves
the prior original/file intact. Preservation writes the actual original,
not a synthetic mixture of a newer partial list with older cultivation.
`observedAt` is stored separately from the physical savedAt timestamp.

## Derived facts and persistent indices

Raw handles carry the frozen compact `brief` used for calculation. Relevant
brief changes advance detailMemoryRevision even if the physical raw payload
and file digest are unchanged. The derivation key also needs metadata identity
and analysisVersion. A persistent index bound only to the raw source digest
may be reused only when rawProjectionOnly is true. That check compares effective
race aliases and numbers; `r` versus equal `ri`, display name and position alone
do not fabricate a changed cultivation input. Actual race/level/power/visual
mismatch overrides prevent raw-only index reuse. A future IO-generated overlay
dependency digest can support those cases without Core hashing large JSON.

## Required handle ownership in consumers

Legacy `detailFor/backpackPet/warehousePet/backpackPets/warehousePets` still
return naked QJsonObject copies for existing integrations/tests. Every resident
raw export through them increments untrackedExports. Qt cannot tell the cache
when those external copies die; **a passing raw-cache byte counter cannot prove
those legacy copies are bounded**. Production must use compact briefs and
retain a raw handle alongside any materialized subset that shares its JSON.

The integration checklist is:

1. InventoryPublisher/Projection hold only selected/watched raw handles, never
   one handle for every LRU resident. All inventory rows use summaries/facts.
2. Pet/Shop detail widgets and lazy RawDataTree keep the matching handle for
   as long as their JSON is retained, including hidden pages and until replacing
   or clearing their previous JSON on an account/selection change.
3. DerivationCache owns one frozen handle for each accepted pending/executing
   derivation and releases it after accepting compact facts. The whole-account
   analysis freezes compact facts, not a second full raw array.
4. Snapshot/diagnostic/export helpers must not retain naked full-detail arrays.
   Snapshot facts and rendered strings are separate values; raw inspection uses
   a scoped handle. Core source-frame temporaries are not a long-lived cache.
5. Storage pending writes already carry the frozen raw lease in Repository;
   callers adding another raw IO path must do the same.

Tests: `pet_record_cache_smoke` verifies shared leases, two protection states,
LRU pressure, immutable digest wrappers, cross-cache refusal and old-account
lifetimes. `pet_record_repository_smoke` exercises real Repository/Storage
restore, exact read keys, missing-known-file behavior, compact access, brief
version changes and partial-source preservation. The original Repository,
Refresh, Move and sanitized protocol fixture suites remain required.
