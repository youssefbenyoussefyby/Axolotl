# AxolotlDB — Code Audit

> **Scope:** full repository review — storage layer, core utils, build system, CI/CD, tests.
> **Date:** 2026-08-20 · **Branch:** `Axolot` · **Commit at audit time:** `2b2cc76` (buffer Pool/LRU)
>
> Status key: **OPEN** — still needs your attention · **FIXED** — already resolved

---

## Summary

The storage layer is the real work in this repo, and the code is decent: sane
slotted-page design, RAII, a proper exception hierarchy, thorough comments. But
it carries **seven correctness bugs** and **eight design flaws** that every layer
above it would inherit, and the build/CI layer had two defects severe enough that
**the test suite had never executed a single test.**

| Area | Findings | Severity |
|---|---|---|
| [A. Storage correctness](#a-correctness-bugs-in-the-storage-layer) | 8 (all fixed) | Data corruption, OOB reads, resource leaks |
| [B. Design flaws](#b-design-flaws) | 8 (7 resolved) | Silent data loss, blocks Phase 5 |
| [C. Build & CI/CD](#c-build--cicd) | 14 | Test suite never ran |
| [D. Test coverage](#d-test-coverage-gaps) | 9 gaps (all fixed, gate test added) | No persistence test at all |
| [E. Process](#e-the-meta-observation) | 1 | Roadmap tracks files, not behavior |

---

## A. Correctness bugs in the storage layer

### A1 — `Tuple::Deserialize` corrupts data when a NULL var-length column precedes another **FIXED**

Originally [`src/storage/tuple.cpp:221-223`](../src/storage/tuple.cpp#L221) (offsets-plus-sentinel format).

`Serialize` wrote the sentinel `0xFFFF` into the offset slot of a NULL
VARCHAR/VECTOR. `Deserialize` computed column *k*'s end as `offsets[k+1]` — and
when that slot held the sentinel, it fell back to the **total** var-data length
instead of the next real offset.

```
Schema: [ VARCHAR a, VARCHAR b, VARCHAR c ]
Values: [ "ab",      NULL,      "cd"      ]

on disk:  offset table = [0, 0xFFFF, 2]      var data = "abcd"

Deserialize(a): start = 0
                offsets[1] == 0xFFFF  ->  end = offsets[3] = 4
                a = "abcd"                      <-- expected "ab"
```

Triggered whenever a NULL variable-length column was followed by at least one
more variable-length column. Silent data corruption — no exception, no warning.
The original test [`storage_test.cpp:54`](../tests/unit/storage_test.cpp#L54) had
no NULL var column, so it passed despite the bug.

**Fix applied:** the wire format changed from offsets-plus-sentinel to a plain
per-column **length** table (still 2 bytes/column). A NULL column's slot is
written as `0` — its true length, not a sentinel — and contributes zero bytes to
`var_data`. `Deserialize` reads each column's own length directly and advances a
running cursor (`var_written`) by it; no lookahead into a neighboring slot, no
terminator, no special case for the last column. Since a value's extent no
longer depends on any other column's slot being valid, the bug class is closed
structurally, not patched around.

Verified in [`tests/unit/tuple_test.cpp`](../tests/unit/tuple_test.cpp):
`TupleVarLength.*` (12 tests — NULL in every position, consecutive NULLs,
empty-string-vs-NULL, mixed fixed/variable, VECTOR adjacent to NULL) and
`TupleSerializeRawBytes.*` (3 tests verifying `Serialize`'s exact byte output
independently of `Deserialize`, so a bug in one couldn't hide behind a matching
bug in the other). All pass.

---

### A2 — `Tuple::Deserialize` reads out of bounds on any truncated buffer **FIXED**

Originally [`src/storage/tuple.cpp:172-206`](../src/storage/tuple.cpp#L172)
(pre-fix line numbers).

Only the null bitmap length was validated. After that: the fixed-column loop
`memcpy`'d 4 or 8 bytes per column with no check those bytes existed, and the
offset table was read with no bounds check at all. The header at
[`tuple.h:99`](../include/storage/tuple.h#L99) documents `@throws
AxolotlDB::utils::StorageException on corrupt data` — that contract wasn't
implemented. Since tuple bytes come off disk, any partial write or bit-flip
became a heap over-read.

**Fix applied:** the A1 rewrite to a per-column length table made this
tractable — each region's expected byte count is now knowable purely from the
schema (bitmap, fixed columns, offset table) or from the offset table's own
values (var data), so `Deserialize` validates each region's size against what's
left in `raw` immediately before reading it, instead of trusting the buffer
past the initial bitmap check:

1. [`tuple.cpp:137-145`](../src/storage/tuple.cpp#L137) — sums `fixed_bytes`
   the same way `Serialize` does, checks `raw.size()` before the fixed-column
   loop.
2. [`tuple.cpp:171-174`](../src/storage/tuple.cpp#L171) — checks `raw.size()`
   for the offset table before reading it.
3. [`tuple.cpp:183-189`](../src/storage/tuple.cpp#L183) — sums the offset
   table's own declared lengths into `total_var_bytes` and checks `raw.size()`
   before reading any var-data byte.

Each check throws `AxolotlDB::utils::StorageException` on shortfall, matching
the documented contract. The old per-column `end >= start && var_start + end
<= raw.data() + raw.size()` guard in the read loop is now provably always true
once the three checks above pass, so it was removed.

Verified in [`tests/unit/tuple_test.cpp`](../tests/unit/tuple_test.cpp):
`TupleCorruption.TruncatedBufferThrows` (every proper prefix of a valid tuple,
12 lengths) and `TupleCorruption.EmptyBufferThrows` now pass. Full re-run:
`tuple_test` 18/19 (the one remaining failure is B6, unrelated) and
`storage_test` 4/4, no regressions.

**Leftover cleanup (not a correctness issue):** the old sentinel computation —
`const std::ptrdiff_t var_data_bytes = (raw.data() + raw.size()) - var_start;`
/ `offsets[var_col_count] = ...` — is now dead code; nothing reads
`offsets[var_col_count]` anymore since each column's length is self-declared.
Safe to delete along with sizing `offsets` to `var_col_count + 1` instead of
`var_col_count`.

---

### A3 — `BufferPool` permanently leaks a frame when `FetchPage`/`NewPage` throws **FIXED**

`EvictFrame()` *removed* the victim from both `lru_list_` and `lru_map_` and
returned its index, leaving the caller to reinsert it only after the load
succeeded. Between those two points `FetchPage` called `ReadPage()` and
`NewPage` called `AllocatePage()`; either can throw. On that path the frame was
**never reinserted** while `page_table_.erase()` had already run and
`frame.page_id` still held the stale id — the frame became permanently
unevictable and unreachable. A pool of N frames degraded to N-1, N-2, … 0,
ending in a spurious `BufferPoolExhaustedException` nowhere near the real cause.
`FetchPage` did not validate `page_id` before evicting, so a single
out-of-range fetch — a very ordinary caller bug — triggered it.

**Fix applied:** the pool no longer unlinks anything speculatively. Every
fallible call now happens before any structural mutation, giving the strong
exception guarantee without a try/catch or scope guard.

- `EvictFrame()` deleted; replaced by `SelectVictim()`, which runs the same
  backward LRU scan but **only returns an index** — it never touches
  `lru_list_` or `lru_map_`.
- Because the victim is never unlinked, the tail of both functions collapses
  from `EvictFrame()` + `lru_list_.push_front()` + `lru_map_[id] = begin()`
  into a single `TouchFrame(frame_id)` splice. This also removed a second,
  redundant backward scan that re-derived the victim independently of
  `SelectVictim()` — correct only by coincidence, since nothing between the two
  calls happened to change a `pin_count`.
- `FetchPage` validates `page_id` against `FileManager::PageCount()` **before**
  the cache lookup, so a bad id costs nothing instead of first evicting a good
  page.
- `FetchPage` parks the frame in a consistent *empty* state
  (`page_id = INVALID_PAGE_ID`, `ClearDirty()`) before calling `ReadPage()`. A
  throw there now leaves an empty, unpinned, still-linked frame rather than one
  that falsely claims to hold its previous page. Without this, a failed load
  orphaned the frame: `page_table_` had already dropped the old id while
  `frame.page_id` and the dirty flag still pointed at it, so a later eviction
  would write stale (or half-overwritten) bytes back over a newer version of
  that page — a lost update. The `ClearDirty()` also closed a related gap where
  the eviction path wrote a dirty victim but never marked it clean.

Verified in [`tests/unit/buffer_pool_test.cpp`](../tests/unit/buffer_pool_test.cpp)
(15 tests, all pass, warning-free).
`BufferPoolDirtyTest.FailedFetchDoesNotConsumeFrames` is the regression itself:
10 consecutive failing fetches against a 3-frame pool, then all three frames
must still pin simultaneously and `OccupiedFrames() == Capacity()`. Under the
old code the pool was dead after three. Also covered:
`OutOfRangeFetchThrows` (positive and negative ids) and
`FailedFetchLeavesCachedPageIntact`.

**Residual gap:** the I/O-failure paths (`ReadPage`/`WritePage` throwing
`IOException` mid-operation) are still untested — `FileManager` has no virtual
methods, so a test double needs an interface extraction first. The
out-of-range path, which is A3's named trigger, is covered. Also unchanged:
`NewPage` calls `AllocatePage()` before the victim write-back, so a throw in
the write-back leaks a *disk* page. Cleaning that up needs a free-list that
does not exist yet, and a leaked disk page is far cheaper than a leaked frame.

---

### A4 — `FileManager::Sync()` does not sync **FIXED**

Originally [`src/storage/file_manager.cpp:139-142`](../src/storage/file_manager.cpp#L139).

```cpp
void FileManager::Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();          // <-- stream buffer -> OS page cache. Not to disk.
}
```

`std::fstream::flush()` moves bytes from the stream's userspace buffer into the
**OS page cache** and returns. That survives `abort()`; it does not survive
power loss. `fsync(2)` is the call that moves the kernel's dirty pages to the
device. The declaration claimed `/// Flush OS buffers to disk (fsync).`

Two halves, and the second was the worse one:

1. `Sync()` did not sync.
2. **`Sync()` had zero call sites in the repo.** `BufferPool::FlushAllPages()`
   wrote every dirty frame and returned, so even a correct `fsync` would have
   changed nothing. The durability path never reached the primitive.

**Fix applied:** `std::fstream` cannot express `fsync` portably — the standard
gives no access to the underlying descriptor. Rather than rewrite the I/O layer
onto raw handles, `FileManager` keeps the `fstream` for reads and writes and
opens a **side descriptor** on the same path used only for syncing. On POSIX
`fsync` acts on the inode, so a second descriptor flushes the first one's
writes; on Windows `_commit` reaches the same per-file cache.

- `Sync()` is now: reject if a previous sync failed → reject if the stream is
  already poisoned → `file_.flush()` → **re-check `good()`** → platform sync.
  The re-check is the load-bearing step. `fsync` knows nothing about the
  userspace stream buffer, so syncing without a *successful* flush first would
  return success having synced whatever happened to already be in the page
  cache — a `Sync()` that reports durability it never delivered.
- Platform branch: `_commit` on Windows, `fcntl(F_FULLFSYNC)` on macOS (plain
  `fsync` there returns without flushing the drive's own write cache), `fsync`
  elsewhere.
- The side descriptor is opened with `_O_RDWR` / `O_RDWR`. This is not optional
  on Windows: `FlushFileBuffers`, which `_commit` calls, requires
  `GENERIC_WRITE` on the handle. A read-only descriptor works under POSIX and
  fails only on Windows — a platform-divergent bug invisible to Linux CI.
- The `_open` is the **last** statement in the constructor that can be reached
  before a throw. A throwing constructor never runs `~FileManager()`, and
  `int sync_fd_` has no destructor, so opening it earlier would leak a
  descriptor on every failed construction — silently, since fd leaks are
  invisible to ASan and Valgrind.
- A failed sync sets a `sync_failed_` latch and every later `Sync()` refuses.
  On Linux a failing `fsync` can *clear* the kernel error flag, so a retry
  returns success while the data is gone
  ([PostgreSQL fsyncgate](https://wiki.postgresql.org/wiki/Fsync_Errors)).
  There is no correct recovery; the only safe response is to stop pretending.
- `BufferPool::FlushAllPages()` now ends with `file_manager_->Sync()`. Without
  this the fix is unreachable.
- `_open`/`strerror` replaced with `_sopen_s(..., _SH_DENYNO, ...)` and
  `std::error_code(errno, std::generic_category()).message()`, which are
  warning-free under MSVC and make the file-sharing mode explicit.

**Contract, since it is surprising:** `WritePage` is buffered and is **not**
durable when it returns. Durability begins when `Sync()` returns. `FlushPage`
(single page) writes without syncing; `FlushAllPages` syncs.

Verified in [`tests/unit/file_manager_test.cpp`](../tests/unit/file_manager_test.cpp)
(11 tests, all pass, warning-free). Two are the regression itself and both fail
against the pre-fix code:
`FlushAllPagesCallsSync` flushes once while healthy so no frame is left dirty,
then poisons the stream — with nothing to write, `Sync()` is the only statement
left that can fail, so a clean return proves it was never called.
`SyncThrowsOnPoisonedStream` pins the "no success it cannot deliver" rule.
Also covered: the reopen round trip (`PagesSurviveReopen`,
`TuplesSurviveFlushAllPagesAndReopen`, `PageCountRecoveredOnReopen` — the
persistence test section D says the suite never had), visibility to an
independent reader, idempotence, and
`RepeatedFailedConstructionDoesNotExhaustDescriptors` for the fd-leak ordering.

**Residual gap:** no test proves `fsync` reached the platter, and none can —
that needs real power loss or a fault-injecting block device. Killing a
subprocess proves only that data left the userspace buffer, which the *old*
broken code also did, so such a test would pass against the bug it claims to
catch. Also unaddressed: on POSIX, a newly created file's **directory entry**
is itself unflushed metadata, so after a crash the file can be absent even
though its contents reached the device. Fixing that means `fsync`-ing the
parent directory once, on the creation branch only.

---

### A5 — `BufferPool` can be constructed with a null `FileManager` **FIXED**

Originally [`include/storage/buffer_pool.h:59`](../include/storage/buffer_pool.h#L59),
[`src/storage/buffer_pool.cpp:8`](../src/storage/buffer_pool.cpp#L8).

`file_manager = nullptr` was the constructor's **default argument** and the
constructor never rejected it. `BufferPool(128)` compiled fine and segfaulted
on the first fetch, inside `file_manager_->PageCount()`.

**Fix applied:** the constructor now rejects `file_manager == nullptr` with
`std::invalid_argument`, the same exception type and same synchronous-throw
pattern already used for `pool_size == 0` right next to it — fail before any
frame is touched, not three calls later inside a null-pointer dereference.

Verified in [`tests/unit/buffer_pool_test.cpp`](../tests/unit/buffer_pool_test.cpp)
(`BufferPoolConstruction.NullFileManagerThrows`,
`BufferPoolConstruction.DefaultFileManagerArgumentThrows` — the latter calls
`BufferPool(128)` with no second argument at all, pinning the default-argument
path specifically).

---

### A6 — `FileManager` accepts a `page_size` it cannot honour **FIXED**

Originally [`src/storage/file_manager.cpp:10`](../src/storage/file_manager.cpp#L10),
[`:112`](../src/storage/file_manager.cpp#L112),
[`:132`](../src/storage/file_manager.cpp#L132).

`page_size_` was a runtime parameter, but `Page::data_` is
`std::array<std::byte, PAGE_SIZE>` — fixed at compile time.

```cpp
FileManager fm("db", 8192);   // compiled
fm.ReadPage(0, page);         // wrote 8192 bytes into a 4096-byte buffer
```

`StorageConfig::page_size` ([`include/utils/config.h:17`](../include/utils/config.h#L17))
is a runtime knob wired to nothing, which made this mistake easy to make.

**Fix applied:** the constructor now rejects any `page_size != PAGE_SIZE` with
`std::invalid_argument`. An earlier pass only rejected `page_size > PAGE_SIZE`,
which blocked the overflow direction but left `page_size < PAGE_SIZE` silently
accepted — that doesn't overflow `Page::data_`, but it still changes the file's
page-boundary math (`AllocatePage`'s offsets, `PageCount()`'s
`file_size / page_size_`) away from what `Page`'s own layout assumes
(`free_ptr = PAGE_SIZE`, etc.), which the header comment ("Must match
PAGE_SIZE") rules out either way. `!=` closes both directions.

Verified in [`tests/unit/file_manager_test.cpp`](../tests/unit/file_manager_test.cpp)
(`FileManagerSyncTest.OversizedPageSizeThrows`,
`FileManagerSyncTest.UndersizedPageSizeThrows`).

---

### A7 — `std::fstream` error state is never cleared **FIXED**

Originally [`src/storage/file_manager.cpp:113`](../src/storage/file_manager.cpp#L113):

```cpp
if (!file_.good() && !file_.eof()) { throw ... }
```

The condition deliberately tolerated `eofbit`. But once `eofbit` or `failbit`
was set, **every subsequent `seekg`/`read` on that stream silently failed**
until `clear()` was called — and there was no `clear()` anywhere in the file.
One short read poisoned the `FileManager` for the rest of the process. Made
more urgent by A4: `Sync()` now throws on a poisoned stream, so the same short
read that used to just get annoying now took down the durability path.

**First pass overcorrected:** an intermediate fix cleared the stream on *any*
`eof()`, without checking whether the read had actually gotten everything it
asked for. Since `AllocatePage` always zero-fills a full `page_size_` block,
the file is always an exact multiple of the page size in the healthy case —
meaning a full, successful `ReadPage` should never set `eofbit` at all, and the
*only* time it fires is a genuinely short read caused by external truncation
or corruption. Clearing indiscriminately erased that signal: `Sync()`'s A4
guard (`if (!file_.good()) throw`) then saw a "healthy" stream and reported
success on a page that had just come back corrupted. Two A4 regression tests
(`FlushAllPagesCallsSync`, `SyncThrowsOnPoisonedStream`) caught this — both
require `Sync()` to keep refusing after a short read, and both failed against
that intermediate version. A second inverted pass then briefly did the reverse
(threw on every *successful* full read, cf. `git blame` on this section for the
one-line history) before landing on the correct condition below.

**Fix applied:** the check is now driven by `file_.gcount()` — the number of
bytes actually extracted by the last read — instead of by which `fstream`
flags happen to be set:

```cpp
file_.read(..., page_size_);
if (file_.gcount() < page_size_) {
    throw AxolotlDB::utils::IOException(...);
}
```

`gcount() < page_size_` is real corruption or truncation and stays poisoned —
the stream is not touched, so `Sync()`'s guard still sees the broken state and
still refuses. `gcount() == page_size_` is a complete, successful read; nothing
needs clearing because nothing gets left set. This sidesteps reasoning about
`good()`/`eof()`/`fail()` combinations entirely — `gcount()` is a plain byte
count and answers the only question that matters directly.

Verified in [`tests/unit/file_manager_test.cpp`](../tests/unit/file_manager_test.cpp):
the two A4 regression tests above pass again, alongside the ordinary read path
(`PagesSurviveReopen`, `SyncedDataIsVisibleToAnIndependentReader`,
`TuplesSurviveFlushAllPagesAndReopen`) — full suite is green except B6.

---

### A8 — `Tuple::Serialize` silently truncates variable-length values over 65535 bytes **FIXED**

Found during the Phase 1 reliability audit (not in the original review).

The tuple wire format records each variable column's byte count in a `uint16_t`
length table. `Serialize` cast the real size down to `uint16_t` at the write
site with no range check, so any value over 65535 bytes wrapped:

```
VARCHAR of 70000 bytes  ->  length table records 70000 & 0xFFFF = 4464
Deserialize             ->  returns a 4464-byte string

VECTOR of 20000 floats  ->  80000 bytes recorded as 14464
Deserialize             ->  returns 3616 floats
```

Measured, not theorised — a probe confirmed both round trips before the fix.
No exception, no warning, no truncation flag: the same silent-wrong-data shape
as **A1** and **B6**.

**Reachability, honestly:** a tuple that large cannot fit in a 4096-byte page,
so `Page::InsertTuple` would have rejected it — the corruption was unreachable
*through the page path*. But `Serialize`/`Deserialize` are public static API,
the corruption happened before any size check could intervene, and Phase 3's
executor calls them directly. `Page` itself is safe here: `FreeSpace()` bounds
`len` to at most `PAGE_SIZE`, so its own `uint16_t` slot length cannot wrap.

**Fix applied:** the sizing loop now checks each column's byte count against
`MAX_VAR_COLUMN_BYTES` (65535, declared in `tuple.h` as the format constraint
it is) and throws `ValueTooLargeException` naming the offending column. The
check runs before the buffer is allocated, so nothing is written on the failure
path.

Verified in [`tests/unit/tuple_test.cpp`](../tests/unit/tuple_test.cpp): an
oversized VARCHAR throws; a value of *exactly* 65535 bytes still round-trips
(an off-by-one in the guard would reject a value the format handles fine); and
the message names the column.

**Update after B5:** the VECTOR half of this is now gone by construction, not
by guard. A vector column is eight fixed bytes regardless of dimension, so it
has no length-table entry left to overflow — a 20000-dim embedding serialises
fine and the size question belongs to `VectorHeap`, where a record simply spans
as many pages as it needs. `MAX_VAR_COLUMN_BYTES` constrains VARCHAR only.

---

## B. Design flaws

### B1 — Two competing sources of truth for "dirty" **FIXED**

`PageHeader::is_dirty` lived *inside the page bytes* (originally
`page.h:56`) **and** `FrameDescriptor::is_dirty` lived in the buffer pool
(originally `buffer_pool.h:34`).

Two consequences:

1. `Page::InsertTuple` set the header bit but **could not** set the frame bit.
   If a caller forgot `UnpinPage(id, /*is_dirty=*/true)`, the frame bit stayed
   false and the modification was **silently discarded on eviction**. Grepping
   confirmed the header bit was write-only: `Page::IsDirty()` had exactly one
   caller in the whole repo (a test assertion), and nothing in `BufferPool`
   ever consulted it. The flag that knew the truth was never read; the flag
   that decided whether data reached disk was set only by hand.
2. Dirtiness is an in-memory concept. Persisting it meant the byte written to
   disk was always `1`, and two logically identical pages had different
   on-disk bytes — breaking checksums and page comparison later. The design
   was self-contradictory at the concept level: `ClearDirty()` *modified the
   page bytes*, so marking a page clean made it differ from disk.

**Fix applied:** the flag moved out of the serialised bytes entirely and became
a single source of truth owned by the `Page` object.

- `PageHeader::is_dirty` deleted; `_pad` grown from `[7]` to `[8]` so the
  `sizeof(PageHeader) == 16` static assert still holds. This frees 8
  contiguous bytes for the Phase 7 `lsn` + `checksum`.
- `Page` gained a private `bool isDirty_` — part of the C++ object, **not** the
  4096 serialised bytes. `IsDirty`/`MarkDirty`/`ClearDirty` now read and write
  it, and `InsertTuple`/`DeleteTuple` set it automatically.
- `FrameDescriptor::is_dirty` deleted. Every eviction and flush check in
  `BufferPool` now asks `frame.page.IsDirty()`.
- `UnpinPage(id, is_dirty)` keeps its parameter but is now purely additive
  (`if (is_dirty) frame.page.MarkDirty();`) — a belt-and-suspenders escape
  hatch for callers that mutate through `RawData()`, rather than the only
  thing standing between an insert and oblivion.

Consequence 1 is now structurally impossible: a caller cannot fail to record a
modification, because the page records it itself.

Verified in [`tests/unit/buffer_pool_test.cpp`](../tests/unit/buffer_pool_test.cpp)
(12 tests, all pass). The headline case is
`BufferPoolDirtyTest.ModifiedPageSurvivesEvictionWhenUnpinnedClean`: modify a
page, unpin it as *clean* (the exact caller mistake), force eviction, refetch —
the write survives. `PageDirtyFlag.DirtyFlagIsNotStoredInPageBytes` pins
consequence 2 by asserting two identical pages stay byte-identical after one is
marked dirty. Also covered: new-page-is-dirty, disk-read-page-is-clean, header
survives eviction untouched, and flush-clears-dirty. `storage_test` 4/4 and
`tuple_test` 18/19 (the one failure is B6) — no regressions.

> Note: the fix as first written transposed `MarkDirty()`/`ClearDirty()`
> between `FetchPage` and `NewPage` — marking freshly-read pages dirty
> (pointless write-back on every eviction) and freshly-allocated pages clean
> (the new page's header never reached disk, so it read back with
> `free_ptr == 0` and zero free space). Caught by three of the tests above and
> corrected. Polarity: a page read from disk is **clean**; a newly allocated
> page is **dirty**.

---

### B2 — 4 KB pages cannot store the vectors this database exists to store **DECIDED + IMPLEMENTED (standalone)**

A 768-dim `float32` embedding is 3072 bytes. A 1536-dim one is **6144 bytes —
larger than an entire page.** `Page::InsertTuple` throws `PageFullException` and
there is no overflow-page or TOAST mechanism.

**Strategy chosen: a separate vector heap** ([`include/storage/vector_heap.h`](../include/storage/vector_heap.h),
[`src/storage/vector_heap.cpp`](../src/storage/vector_heap.cpp)).

Three options were on the table:

| Option | Why not / why yes |
|---|---|
| Overflow pages (PostgreSQL TOAST) | Keeps one uniform page size and leaves the row store untouched, but adds a pointer format, chain split/reassembly/cleanup — and for a *vector* database it puts an indirection hop in front of the primary workload, since vector search reads lots of vectors. |
| **Separate vector heap** ✅ | What essentially every purpose-built vector DB does (Milvus, Qdrant, Weaviate, LanceDB): vectors in their own storage segment, metadata alongside. Vectors can be laid out exactly how a vector index wants them, and the row-store engine hardened in A1–A7 is not reopened at all. |
| Bigger pages for vector tables | Rejected. It reopens A6 (`Page::data_` is compile-time fixed for a reason), and it only raises the ceiling — 3072-dim models hit the same wall later. No serious database answers unbounded value size this way. |

**Design, and why each choice was made:**

- **Scope is one heap per (table, vector column)**, never shared across columns
  even when two columns happen to share a dimension. A shared heap makes
  `VectorId` ambiguous — id 42 could mean two different records — and that
  failure is *silent*, since a dimension check can't catch it. It also keeps
  drop-column to "delete the file" instead of reopening B7-style compaction.
- **Reuses `FileManager` and `BufferPool` unmodified.** `VectorHeap` is a new
  consumer that interprets a page's bytes as packed floats instead of a slotted
  tuple directory. Zero new risk to A1–A7 or B1.
- **No per-record pointer is ever stored.** Records are fixed-stride
  (`dimension * sizeof(float)`, guaranteed uniform by the B6 fix), so a
  `VectorId`'s location is *computed*: `LocateVector` returns
  `{first_page, offset_in_page, page_span}`. Two regimes — packed
  several-per-page when `stride <= PAGE_SIZE`, spanning whole page-aligned
  runs when it doesn't.
- **Page-aligned large records**, accepting wasted tail space, rather than
  byte-packing across page boundaries. Byte-packing is denser but makes a
  single `Get` straddle a partial page mid-record — exactly the subtle index
  arithmetic that produced A2/A6.
- **Self-describing header on page 0** (`dimension`, `vector_count`).
  Reopening with a mismatched dimension throws at construction, the same
  fail-fast contract as A6's `page_size` check. Recovering `vector_count` is
  load-bearing: if it came back 0, the next `Put()` would hand out a
  `VectorId` that aliases a stored record and overwrite it while reporting
  success.

Verified in [`tests/unit/vector_heap_test.cpp`](../tests/unit/vector_heap_test.cpp)
— 19 tests, all pass. Both addressing regimes, the page-boundary rollover, a
stride that does not divide `PAGE_SIZE` evenly, the exact-one-page-wide seam,
multi-page records spanning 2 and 3 pages, persistence across full
destroy-and-reopen for both regimes, append-after-restart, mid-page resumption
across restarts, and correctness under a pool far smaller than the page count
(which also catches leaked pins). The suite was mutation-checked: an
off-by-one in `records_per_page` fails 4 tests including the boundary test by
name; a wrong per-record page stride fails the two large-vector tests.

**Commit / rollback — the orphaned-vector leak, FIXED.** `Put()` originally
committed immediately: it wrote the data pages *and* the header, then returned.
If the row referencing the new vector then failed to insert, the vector was
durably stored, counted, and unreferenced forever. A retry allocated a *new*
id, so retrying was correct but never reclaimed the abandoned record — every
failed insert grew the file permanently.

The commit point moved to `Sync()`:

- `Put()` writes the data pages and advances an in-memory counter. It does not
  touch the header, so nothing is durable and no id is permanent yet.
- `Sync()` writes the header with the current count and fsyncs. **This is the
  commit.** Writing the header last is what makes a crash mid-`Put` harmless:
  the data pages may have reached disk, but an unchanged count leaves them
  unreferenced and their ids reusable.
- `Rollback()` rewinds the in-memory counter to the durable one, handing the
  ids straight back. The pages stay allocated on purpose — they get overwritten
  when those ids are reissued, so no free list is needed.

The subtle part was **page acquisition**. `Put()` used to choose `NewPage` vs
`FetchPage` from `info.offsets == 0`, which silently assumed `NewPage` always
returns exactly the page `LocateVector` predicted. That holds only while every
`Put` commits — after a rollback the pages exist while the counter has rewound,
so the next `Put` would have appended a *different* page than the id resolves
to. `AcquirePage()` now decides from `FileManager::PageCount()` and throws if a
fresh allocation ever lands somewhere other than predicted, rather than
corrupting silently.

Cost is now bounded rather than unbounded: the first abandoned `Put` leaves its
pages allocated, and every subsequent failure reuses them. Verified on disk —
20 abandoned `Put`s grow a heap by at most one record.

This also removed the three `FlushAllPages()` + `fsync` pairs `Put()` was doing
*per call* (one per data page, plus one for the header), which were both the
durability bug and a serious performance problem.

Nine tests cover it, and the suite is mutation-checked: making `Rollback()` a
no-op fails exactly the five tests written for it.

**Now wired into the row store.** A `VECTOR` column is a fixed-width
`VectorRef` in the row and the floats live in the heap — see
[B5](#b5--value-is-a-fat-struct-not-a-variant-fixed), which turned out to be
the same change. The Phase 1 gate test now spans both files: 500 rows with
1536-dim embeddings (6144 bytes each, larger than an entire page — rows that
were *physically impossible to insert* before this) written, closed, reopened,
and every reference resolved back to the right embedding.

Still deferred, both matching existing accepted debt: deletion/compaction
(append-only, same as B7) and per-record locking (inherits `BufferPool`'s
posture, same as B8).

Reference: PostgreSQL [TOAST](https://www.postgresql.org/docs/current/storage-toast.html)
is the overflow-page approach solved.

---

### B3 — `LogLevel::ERROR` collides with the Windows `ERROR` macro **FIXED**

Originally [`include/utils/logger.h:16`](../include/utils/logger.h#L16).

`wingdi.h` does `#define ERROR 0`, turning `LogLevel::ERROR` into `LogLevel::0`.
This produced `error C2589: 'constant': illegal token on right side of '::'`.
The workaround was a local `#undef ERROR` in `cli/main.cpp`, which rescued
exactly one translation unit; Phase 8's TCP server would have hit it again.

**Fix applied:** all seven enumerators renamed to the `k` prefix already used
elsewhere in the codebase (`kHeadGuard`/`kTailGuard` in `memory.cpp`):
`kTrace`, `kDebug`, `kInfo`, `kWarn`, `kError`, `kCritical`, `kOff`.

Renaming only `ERROR` would have left the enum half-converted and, worse, left
`DEBUG` exposed — it is also a commonly-defined macro, so the same class of bug
was one `#define` away from recurring under a different name. The `#undef ERROR`
in `cli/main.cpp` is gone, replaced by a comment explaining why it is no longer
needed; `windows.h` itself stays, since that file genuinely uses
`GetStdHandle`/`SetConsoleMode`.

---

### B4 — `Allocator` silently ignores alignment **FIXED**

Originally [`src/utils/memory.cpp:44`](../src/utils/memory.cpp#L44) —
`(void)alignment;`.

It always called plain `::operator new`, which guarantees only
`alignof(std::max_align_t)` (typically 16). `IAllocator::New<T>()` for any
over-aligned `T` — the SIMD/Eigen types Phase 5 needs — returned misaligned
memory: UB, and an immediate fault on targets where aligned vector loads trap.

**Fix applied:** both paths now honour the requested alignment.

- **Raw path:** `::operator new(bytes, std::align_val_t{...})`, with the
  matching aligned `::operator delete`. Mixing the aligned and unaligned
  overloads is itself UB, so both sides had to change together.
- **Tracked/guarded path** was the subtler half, and the suggested one-line fix
  is *not* sufficient for it. The `AllocationHeader` sits immediately in front
  of the user pointer, so aligning the base allocation alone still leaves
  `base + sizeof(header)` off-boundary whenever `alignment >
  alignof(std::max_align_t)`. The gap in front of the user pointer is now
  rounded up to a multiple of the requested alignment (`UserOffset()`), the
  header is written at `user_ptr - sizeof(header)` rather than at `base`, and
  `DeallocateBytes` recomputes the same padding from its own `alignment`
  argument to recover the base pointer — so no extra per-allocation bookkeeping
  is stored.

Verified in [`tests/unit/memory_test.cpp`](../tests/unit/memory_test.cpp): raw,
tracked, and guarded modes each checked at 32/64/128/256-byte alignment, plus
`New<T>` on an `alignas(64)` type. The guarded case matters most — the footer
is placed at `user_ptr + bytes`, so wrong padding maths corrupts the tail guard
rather than merely misaligning. Mutation-checked: forcing `BaseAlignment` back
to ignore its argument fails all four new tests.

---

### B5 — `Value` is a fat struct, not a variant **FIXED — and it was the B2 wiring**

[`include/storage/tuple.h`](../include/storage/tuple.h)

Every `Value` carried `i32`, `i64`, `f32`, `f64`, `b`, a `std::string`, **and** a
`std::vector<float>` simultaneously. Measured on this toolchain: **112 bytes**
per column value. Query execution will create millions of these.

Size was not the worst of it. Both heap-owning members were constructed, copied
and destroyed regardless of the type actually held, so copying a `Value`
holding an `int32_t` still ran `std::string`'s and `std::vector`'s copy
constructors — and the tag and payload could disagree
(`Value{.type = VECTOR, .str = "oops"}` was representable). Reading the wrong
member returned a default-constructed **zero** rather than complaining, which is
how a wiring bug got to look like legitimate data.

**This was the same change as the B2 wiring.** The vector write path was:

```
caller's floats ─► Value.vec (owns)      ─► memcpy into buf ─► memcpy into page
                   1 heap alloc              copy #1            copy #2
```

A 6144-byte embedding was copied **twice** after it already existed, and
`Deserialize` allocated a fresh `std::vector<float>` and copied into it on
*every* row read — even for `SELECT id, title` that never looks at the vector.
`VectorHeap::Put(std::span<const float>)` is one copy and zero allocations, so
wiring the two together while `Value` still owned the floats would have kept
every one of those copies and added indirection for nothing.

**Fix applied — `VECTOR` is now a fixed-length column holding a reference:**

| | Before | Now |
|---|---|---|
| `IsVariable(VECTOR)` | `true` | `false` |
| `FixedSize(VECTOR)` | `0` | `8` (`sizeof(VectorRef)`) |
| Stored in the row | raw float bytes | an 8-byte reference |
| Offset table covers | VARCHAR + VECTOR | VARCHAR only |
| Row cost of a 1536-dim column | 6144 bytes (unstorable) | 8 bytes |

The floats never enter the tuple path: the caller `Put`s them into the heap and
hands `Serialize` the reference. **`Tuple` therefore has no `VectorHeap`
dependency at all** — which is what removed the objection that this looked
blocked on the catalog. The catalog is still needed to know *which* heap, but
that is the insert path's problem, one layer up.

`Value` is now a variant, with **`std::vector<float>` gone from it entirely**:

```cpp
std::variant<NullValue,       // SQL NULL, carrying the type it is null *for*
             int32_t, int64_t, float, double, bool,
             std::string,     // VARCHAR
             VectorRef>       // VECTOR -- 8 bytes, trivially copyable
```

**112 bytes → 48**, measured, and pinned by `TupleValueLayout.ValueStaysSmall`
so a future "just one more field" regression trips a test.

Note `NullValue` rather than `std::monostate`: the type tag lives *inside* the
null alternative instead of beside the variant. A separate `ColumnType` member
would have cost a full eight bytes after alignment (56, not 48) and would only
ever have been consulted for nulls — and keeping it out means a tag that
disagrees with the payload is now unrepresentable rather than merely unlikely.

Reads go through checked accessors (`AsInt32()`, `AsVarchar()`, `AsVector()`…)
that throw `TypeMismatchException` naming both types, so the old
silently-return-zero behaviour is gone. `Serialize` additionally checks each
value's type against its column up front, because only there is the column
*name* available for the message.

**The three consequences, handled:**

1. **B6's dimension check moved.** `Tuple` no longer sees floats, so it cannot
   compare against `ColumnDef::dimension`. `VectorHeap::Put` already performed
   exactly that check and owns the dimension; it is now the sole enforcement
   point. See [B6](#b6--columndefdimension-is-never-enforced-fixed--enforcement-moved-to-vectorheap).
2. **Reference provenance.** A bare `uint64_t` id carries no proof of which
   heap it came from, and using one against the wrong heap reads a
   valid-looking vector belonging to another column — silently. `VectorRef` is
   therefore self-identifying:

   ```cpp
   struct VectorRef { HeapId heap_id; uint32_t index; };  // still 8 bytes
   ```

   The heap stamps its id into its own on-disk header (in what used to be
   padding, so `VectorHeapHeader` is still 16 bytes), validates it on reopen the
   way it already validates `dimension` (the A6 pattern), issues references
   through `Ref(VectorId)`, and rejects a foreign one in `Get(VectorRef)` with
   `ForeignVectorRefException`. `INVALID_HEAP_ID = 0` is reserved, so a
   default-constructed reference — one no heap ever issued — is rejected too,
   and `Serialize` refuses to write one into a row (NULL is how an absent
   vector is spelled). **Heap ids must never be reused after a column is
   dropped**, or a stale reference could validate against a new heap; that is
   the catalog's obligation when it becomes the assigner.
3. **The on-disk tuple format changed.** No migration concern pre-release. The
   gate test regenerates rather than passing byte-identical.

**Verification.** 144 tests pass, the tree is warning-free, and the guards are
mutation-checked:

| Mutation | Tests that fail |
|---|---|
| `IsVariable(VECTOR)` back to `true` | 6 tuple tests + the gate test |
| Drop the `heap_id` check in `Get(VectorRef)` | `ForeignRefIsRejected…`, `DefaultConstructedRefIsRejected` |
| Drop the header `heap_id` check on reopen | `ReopenWithWrongHeapIdThrows` |
| Drop `Serialize`'s `ref.IsValid()` guard | `UnassignedVectorRefIsRejected` |

The one that matters most is `ForeignRefIsRejectedRatherThanSilentlyResolved`:
two heaps, same dimension, index 0 valid in both. Without `heap_id` the wrong
heap answers with a real, plausible vector and nothing downstream can tell.

---

### B6 — `ColumnDef::dimension` is never enforced **FIXED — enforcement moved to `VectorHeap`**

Originally: `Serialize` accepted a 3-dim value in a 768-dim column;
`Deserialize` inferred the dimension from the byte count. Vector index building
would assume a fixed dimension and break.

**First fix (superseded):** `Serialize` compared `values[i].vec.size()` against
`schema.columns[i].dimension` and threw before copying the vector's bytes.

**Where it lives now:** `Tuple` no longer sees floats at all — B5 made `VECTOR`
an 8-byte reference — so it cannot perform this check, and it would be the
wrong place for it anyway. `VectorHeap::Put` throws
`VectorDimensionMismatchException` on any vector whose length is not the heap's
dimension, and it is the only way floats reach disk. That is strictly stronger
than the old check: the heap owns the dimension, and its fixed-stride
addressing would be corrupted by a wrong-sized record, so the constraint is
enforced where it is actually load-bearing rather than one layer away from it.

Verified in [`tests/unit/vector_heap_test.cpp`](../tests/unit/vector_heap_test.cpp)
(`VectorHeapTest.WrongSizedVectorThrows`, which also asserts that a rejected
`Put` consumes no id).

**Residual gap, now smaller.** The old note here worried that `Deserialize`
inferred vector length from the stored byte count and never cross-checked it
against the schema. That inference is gone: a row's vector column is exactly
`sizeof(VectorRef)` bytes and the length is not derived from anything. What
remains is that a *corrupt* `VectorRef` — a valid heap id with a plausible
index — resolves to the wrong vector rather than being detected. Checksums are
the real answer to that class of problem, and they belong with the WAL work in
Phase 6, not here.

---

### B7 — `Page` never reclaims deleted space **CLOSED** (Phase 2)

`DeleteTuple` tombstones with `length = 0`
([`page.cpp:167`](../src/storage/page.cpp#L167)) and slots remain stable so existing
RIDs stay valid/tombstoned.

**Applied:** Added [`Page::Compact()`](../src/storage/page.cpp) to defragment the
payload region towards `PAGE_SIZE`, zeroing out reclaimed bytes and adjusting
`hdr.free_ptr`. Integrated compaction into [`TableHeap::Delete`](../src/storage/table_heap.cpp)
along with free-space map tracking so that deleted space is reused by subsequent
inserts without file growth. Verified by `TableHeapTest.DeletedSpaceIsReusedBySubsequentInserts`
and `PageSlotDirectory.DeleteTupleAloneDoesNotReclaimUntilCompact_B7Closed`.

---

### B8 — No page-level latch **DOCUMENTED** (implementation still deferred to Phase 7)

`BufferPool::FetchPage` returns a raw `Page*` and then releases the pool mutex.
Two threads holding the same `Page*` can corrupt the slot directory.

Acceptable for a single-threaded MVP. The audit asked for the doc comment to be
corrected in the meantime — and the situation was slightly worse than described:
the header carried *no* thread-safety documentation at all, so the guarantee was
implied purely by the presence of a `mutex_` member, which is the most
misleading form the claim could take.

**Applied:** [`buffer_pool.h`](../include/storage/buffer_pool.h) now carries an
explicit class-level contract — the pool's *metadata* (page table, LRU list,
pin counts) is mutex-guarded and safe to share; page *contents* are not, and
callers must serialise access to a `Page` themselves until per-page latching
lands in Phase 7. The underlying latch is still **OPEN**.

---

### Formal-UB note (low priority)

[`page.cpp:14-28`](../src/storage/page.cpp#L14) uses
`reinterpret_cast<PageHeader*>(data_.data())` on a `std::byte` array. No
`PageHeader` object was ever created there, so this is formally UB — it works on
every real compiler, but it's worth understanding before relying on it further.
See [object lifetime](https://en.cppreference.com/w/cpp/language/lifetime) and
C++23's `std::start_lifetime_as`.

---

## C. Build & CI/CD

### C1 — Eigen hijacked the entire CTest suite **FIXED**

`FetchContent_MakeAvailable(eigen)` pulls in Eigen's full CMake project,
including ~900 of its own test registrations, into **this** project's CTest.

```
$ ctest -N
...
Could not find executable special_packetmath_3
Total Tests: 915          <-- all Eigen's. Zero AxolotlDB tests.

$ ctest --print-labels
  Official / Unsupported / smoketest    <-- Eigen's labels
```

Every registered test belonged to Eigen, and their executables were never built.
`ctest -L integration` therefore matched nothing and exited 8.

Fixed by setting `BUILD_TESTING`/`EIGEN_BUILD_*` off around the
`FetchContent_MakeAvailable(eigen)` call. CMake's generate step dropped from
**142s to 4s** as a side effect.

---

### C2 — vcpkg silently swapped GoogleTest, so no test ever ran **FIXED**

The worst finding in this audit, and completely invisible.

`vcpkg integrate install` injects `C:\dev\vcpkg\installed\x64-windows\include`
into **every** MSBuild project on the machine, ahead of the include directories
CMake sets. Confirmed in the generated command line:

```
/I"C:\...\AXOLOTL\BUILD\_DEPS\..."
/I"C:\DEV\VCPKG\INSTALLED\X64-WINDOWS\INCLUDE"     <-- injected
```

The tests compiled against **vcpkg's** GoogleTest but linked FetchContent's
**v1.14.0**. The two versions have different signatures:

| Source | `MakeAndRegisterTestInfo` first parameter |
|---|---|
| vcpkg gtest | `std::string test_suite_name` |
| FetchContent v1.14.0 | `const char* test_suite_name` |

So `TEST()` macros registered into vcpkg's gtest singleton, while
`gtest_main`'s `RUN_ALL_TESTS()` queried v1.14's. Result:

```
$ storage_test.exe
Running main() from gtest_main.cc
[==========] Running 0 tests from 0 test suites.
[  PASSED  ] 0 tests.
$ echo $?
0
```

**It links cleanly and exits 0.** Nothing warns you. This affected every test
binary in both Debug and Release, and survived a full clean rebuild.

Fixed with `set(CMAKE_VS_GLOBALS "VcpkgEnabled=false")` under MSVC — repo-level,
so it protects anyone who clones rather than requiring a machine change.

> The `DISCOVERY_MODE PRE_TEST` comment in `tests/CMakeLists.txt` about "access
> violations during build" was papering over this same problem.

---

### C3 — `tests/CMakelists.txt` was misnamed **FIXED**

Lowercase `l`. The root `CMakeLists.txt` checked for `CMakeLists.txt`, which
passes on Windows (case-insensitive) and **fails on Linux and in Docker**. CI
silently fell into a fallback branch that registered tests without labels, so
`ctest -L integration` matched nothing.

Renamed, and the fallback branch replaced with a `FATAL_ERROR` so it cannot fail
quietly again.

---

### C4 — `gtest_discover_tests` used `PRE_TEST`, so labels never existed **FIXED**

With `DISCOVERY_MODE PRE_TEST` the test list — and therefore `LABELS` — does not
exist until ctest runs its discovery pass, so `ctest -L unit` matched nothing and
exited 8. CI depends on those labels. Switched to `POST_BUILD`.

---

### C5–C14 — Remaining build and CI defects

| # | Issue | Status |
|---|---|---|
| C5 | CI compiler matrix `[gcc-11, clang-14]` never referenced by any step — 4 identical jobs on the default compiler | **FIXED** — wired to `CC`/`CXX` |
| C6 | `ubuntu-20.04` is a retired runner; those jobs could not start | **FIXED** — 22.04 + 24.04 |
| C7 | Coverage collected from an `-O3` Release build, making line coverage meaningless | **FIXED** — Debug |
| C8 | `.clang-format` said `IndentWidth: 2` while the whole codebase uses 4 — the gate could never pass | **FIXED** — set to 4 |
| C9 | `.clang-tidy` had `WarningsAsErrors: '*'` with all of `modernize-*`/`readability-*`, and `HeaderFilterRegex: '.*'` matched every FetchContent dependency | **FIXED** — scoped; advisory until the tree is clean |
| C10 | Dead `cli/CMakeLists.txt` + `server/CMakeLists.txt` referencing an undefined `${AxolotlDB_LIB_TARGET}`, with target names conflicting with the root's | **FIXED** — deleted |
| C11 | `docker-compose.yml` ran `./build/server/vector-db-server` — a path and binary name that never existed (real target: `AxolotlDB-server` in `build/`) | **FIXED** |
| C12 | `file(GLOB_RECURSE)` without `CONFIGURE_DEPENDS` — new `.cpp` files don't trigger a CMake re-run | **FIXED** |
| C13 | Committed build artifacts (`build_*.txt`, 1048 lines) and unrelated competitive-programming files (1344 lines) at repo root | **FIXED** — deleted |
| C14 | `install(TARGETS AxolotlDB EXPORT AxolotlDBTargets)` has no matching `install(EXPORT)`; `cmake/AxolotlDBConfig.cmake.in` references `onnxruntime` and `Boost` that don't exist | **OPEN** — harmless today; completing it means exporting FetchContent deps too, which is a real design decision |

Naming is still inconsistent across the project: `Axolotl` (repo) / `AxolotlDB`
(namespace) / `AxolotlDB` (CMake project) / `AxolotException` (exception base).
**OPEN** — pick one.

---

## D. Test coverage gaps

[`tests/unit/storage_test.cpp`](../tests/unit/storage_test.cpp) has 4 tests.
Status of each original gap:

1. ~~**Any FileManager round-trip.**~~ **FIXED** — covered by
   `FileManagerSyncTest.SyncedDataIsVisibleToAnIndependentReader` (A4 work) and,
   at scale, by the gate test below.
2. ~~**Any persistence-across-restart test.**~~ **FIXED** — `PagesSurviveReopen`,
   `TuplesSurviveFlushAllPagesAndReopen` (A4 work), and the gate test below.
3. ~~**Any assertion that eviction actually happened.**~~ **FIXED** — in two
   places. [`tests/integration/persistence_test.cpp`](../tests/integration/persistence_test.cpp)
   uses a pool deliberately smaller than the page count 500 rows produces,
   forcing real eviction mid-insert, and asserts every evicted page's content
   is still byte-identical after a full reopen. `storage_test.cpp`'s own
   `BufferPoolNewAndFetch` has since been rewritten too: each of the four
   pages now carries a distinct marker that is read back and compared after
   eviction, instead of asserting only `GetPageId()`.
4. ~~NULL values in tuples~~ **FIXED** — `tuple_test.cpp` (A1 work).
5. ~~Truncated/corrupt buffers~~ **FIXED** — `TupleCorruption.*` (A2 work).
6. ~~`PageFullException` on overflow~~ **FIXED** — `PageSlotDirectory.*` in
   [`buffer_pool_test.cpp`](../tests/unit/buffer_pool_test.cpp) now fills a page
   until it genuinely refuses, asserts the tuple count is physically plausible
   rather than just non-zero, and — the part that matters — checks that a
   *failed* insert leaves earlier tuples readable and does not consume a slot,
   the same throw-before-mutation discipline as A3. Zero-length inserts covered
   too.
7. ~~`DeleteTuple`, and `GetTuple` on a deleted slot~~ **FIXED** — `GetTuple` on
   a tombstoned slot throws rather than handing back the old bytes; double
   delete throws; out-of-range slot indices throw on both paths; and deleting a
   middle slot leaves its neighbours readable *and* does not renumber them,
   which is what makes previously handed-out `SlotIdx` values safe to hold.
8. ~~`FreeSpace` accounting, LRU eviction ordering~~ **FIXED** — `FreeSpace` is
   now asserted to shrink by exactly payload + `sizeof(SlotEntry)` (the loose
   `EXPECT_GT(..., 0u)` never would have caught slot overhead being forgotten),
   to be monotonic across many inserts, and to leave room for the page header.
   `EvictionFollowsLeastRecentlyUsedOrder` touches a page to change the LRU
   order and then forces an eviction, asserting every page still holds *its own*
   content afterwards.

   B7 is also now pinned by `DeletedSpaceIsNotReclaimed_B7`, which asserts the
   *current* (non-reclaiming) behaviour so that adding compaction becomes a
   visible, deliberate test change rather than a silent one.
9. ~~Dirty-page flush semantics~~ **FIXED** — `BufferPoolDirtyTest.*` (B1 work).

~~Also: `page0`/`page1`/`page2`/`page3` in `storage_test.cpp` are set and never
used, and `:121` discards a `[[nodiscard]]` return — 6 warnings against the
project's own "warning-free" goal.~~ **FIXED** — those locals now carry real
assertions rather than being deleted, so the warning and the weak-coverage
gap (item 3) closed together.

**The whole project now compiles warning-free** (MSVC `/W4`-level, excluding
FetchContent dependencies). Also fixed in the same pass: 6 `C4267` narrowing
warnings in [`tuple.cpp`](../src/storage/tuple.cpp), 4 `C4267`/`C4018` in
[`vector_heap.cpp`](../src/storage/vector_heap.cpp), and a `C4996` on
`std::getenv` in [`config.cpp`](../src/utils/config.cpp) — the last resolved
with a `_dupenv_s` platform branch matching the `_commit`/`fsync` split
already in `FileManager`, rather than defining `_CRT_SECURE_NO_WARNINGS`
project-wide and losing the whole warning class.

**The Phase 1 gate — FIXED.** Original text: create DB → insert 500
multi-column tuples with NULLs across many pages → `FlushAllPages()` +
`Sync()` → destroy every object → reopen from the same file → read all 500
back byte-identical. Implemented verbatim in
[`tests/integration/persistence_test.cpp`](../tests/integration/persistence_test.cpp)
(`PersistenceGateTest.FiveHundredMultiColumnTuplesWithNullsSurviveReopenByteIdentical`):
schema interleaves a fixed-length column between two nullable variable-length
columns (VARCHAR, VECTOR) so adjacent-NULL variable columns — the exact shape
of A1 — occur at scale; an 8-frame pool against the ~17 pages 500 rows
produces forces real eviction during the write phase, not just at the end;
verification compares every tuple's raw bytes after a full destroy-and-reopen,
plus confirms `Tuple::Deserialize` still succeeds on the round-tripped bytes.
This is the first test in the suite that exercises `FileManager`, `BufferPool`,
`Page`, and `Tuple` together rather than each in isolation.

**Also found while adding it:** `tests/CMakeLists.txt`'s `file(GLOB ...)` calls
for `unit/*.cpp` and `integration/*.cpp` were missing `CONFIGURE_DEPENDS` — C12
fixed this for the root `CMakeLists.txt`'s library sources but missed the test
globs, so a new test file silently required a manual `cmake` reconfigure to be
picked up at all (a `cmake --build` alone would not add it to the test binary
list). **FIXED** alongside this test.

---

## E. The meta-observation

The repo was scaffolded top-down: 44 empty `.cpp` files, an 8-phase roadmap,
`docs/`, benchmarks, and a progress report all created before the first line of
storage code.

That is why the roadmap tracks *files created* rather than *behavior working*,
and why `axolotl_progress_report.md` reported **"Phase 1 — 60% Complete"** for a
system that could not yet store and retrieve a row — and whose test suite, as
C1/C2 showed, had never executed a single assertion.

The 8-phase structure is a sound curriculum and worth keeping. The change to make
is in the definition of done:

> **A phase is complete when a test proves the behavior, not when the file exists.**

Rewrite each phase's checklist as behaviors. `- [ ] Implement AST` becomes
`- [ ] SELECT id FROM t WHERE id > 5 parses into a plan tree a test can assert on`.

---

## Priority order

1. ~~**A1**~~ **FIXED** — var-length wire format changed to a length table; see [A1](#a1--tupledeserialize-corrupts-data-when-a-null-var-length-column-precedes-another-fixed).
2. ~~**A2**~~ **FIXED** — per-region size validation before every read; see [A2](#a2--tupledeserialize-reads-out-of-bounds-on-any-truncated-buffer-fixed).
3. ~~**B1**~~ **FIXED** — dirty flag moved out of the page bytes into a single
   page-owned in-memory flag; see [B1](#b1--two-competing-sources-of-truth-for-dirty-fixed).
4. ~~**A3**~~ **FIXED** — `SelectVictim()` never unlinks; all fallible I/O now
   precedes any mutation; see [A3](#a3--bufferpool-permanently-leaks-a-frame-when-fetchpagenewpage-throws-fixed).
5. ~~**A4**~~ **FIXED** — real `fsync` via a side descriptor, and
   `FlushAllPages()` now calls it; see [A4](#a4--filemanagersync-does-not-sync-fixed).
6. ~~**A5**~~ **FIXED** — null `FileManager` now rejected at construction with
   `std::invalid_argument`; see [A5](#a5--bufferpool-can-be-constructed-with-a-null-filemanager-fixed).
7. ~~**A6**~~ **FIXED** — constructor rejects any `page_size != PAGE_SIZE`; see
   [A6](#a6--filemanager-accepts-a-page_size-it-cannot-honour-fixed).
8. ~~**A7**~~ **FIXED** — `ReadPage` now judges success by `gcount()` against
   `page_size_` instead of by `fstream` flag state; see
   [A7](#a7--stdfstream-error-state-is-never-cleared-fixed).
9. ~~**D — the persistence test**~~ **FIXED** — the 500-row gate test now
   proves A1–A7 hold together end to end, not just individually; see
   [D](#d-test-coverage-gaps). Three narrower coverage gaps remain open within
   D itself: `PageFullException` on overflow, `GetTuple` on a deleted slot,
   and real `FreeSpace`/LRU-order assertions.
10. ~~**B2**~~ **DECIDED + IMPLEMENTED** — separate vector heap
    (`VectorHeap`), 19 tests, mutation-checked; see
    [B2](#b2--4-kb-pages-cannot-store-the-vectors-this-database-exists-to-store-decided--implemented-standalone).
    Wired into the row store as of item 20.
11. ~~**`VectorHeap` follow-ups**~~ **FIXED** — `Sync()` now has a definition
    (and a test that calls it, which is why the missing one went unnoticed);
    `Get()` bounds-checks against `Count()`; two new exception types
    (`InvalidVectorIdException`, `VectorDimensionMismatchException`) replace
    the `std::invalid_argument`/generic-`StorageException` placeholders, both
    carrying the operands in their message and both asserted on by tests.
    24 `VectorHeapTest` cases, all passing.
12. ~~**Project-wide warnings**~~ **FIXED** — 11 warnings across `tuple.cpp`,
    `vector_heap.cpp`, `config.cpp`, and `storage_test.cpp`; the tree is now
    warning-free. See the note at the end of [D](#d-test-coverage-gaps).
13. ~~**Wiring `VectorHeap` into the row store**~~ **FIXED** — and it was never
    blocked on the catalog after all. Because the *caller* does the `Put` and
    hands `Serialize` the reference, `Tuple` needs no `VectorHeap` dependency;
    the catalog only decides *which* heap, one layer up. Done as item 20.
14. ~~**B3, B4**~~ **FIXED** — `LogLevel` enumerators k-prefixed (kills the
    Windows `ERROR`/`DEBUG` macro collision at the source);
    `Allocator` honours over-alignment in all three modes.
15. ~~**D6, D7, D8**~~ **FIXED** — `PageSlotDirectory.*` and
    `EvictionFollowsLeastRecentlyUsedOrder`; see [D](#d-test-coverage-gaps).
16. ~~**B8 documentation**~~ **APPLIED** — `buffer_pool.h` now states what is
    and is not thread-safe. The latch itself remains deferred to Phase 7.
18. ~~**A8**~~ **FIXED** — `Serialize` silently truncated variable-length values
    over 65535 bytes; now throws `ValueTooLargeException`. Found by this audit,
    not the original one. See [A8](#a8--tupleserialize-silently-truncates-variable-length-values-over-65535-bytes-fixed).
19. ~~**B2 orphaned-vector leak**~~ **FIXED** — commit point moved from `Put()`
    to `Sync()`, with `Rollback()` reclaiming abandoned ids and `AcquirePage()`
    keeping id→page addressing correct across a rollback.
20. ~~**B5 + the B2 wiring**~~ **FIXED** — one change, as predicted.
    `VECTOR` is now a fixed-width `VectorRef` column, `Value` is a variant
    (112 bytes → 48, `std::vector<float>` gone from it), B6's dimension check
    moved to `VectorHeap::Put`, and references are self-identifying so a
    reference used against the wrong heap throws instead of resolving to
    another column's vector. The gate test now spans both files and inserts
    1536-dim embeddings — rows that were physically impossible before. See
    [B5](#b5--value-is-a-fat-struct-not-a-variant-fixed--and-it-was-the-b2-wiring).
21. Still genuinely open:
    - **Phase 2 catalog** — the natural next piece, and now unblocked in both
      directions. It maps (table, column) → heap file, owns `VectorHeap`
      lifetimes, and becomes the assigner of `HeapId`s — with one hard rule
      the storage layer cannot enforce for it: **never reuse a heap id after a
      column is dropped.** A reused id makes a stale `VectorRef` validate
      against a heap that knows nothing about it, which is exactly the failure
      `heap_id` was added to prevent.
    - **B7** — page compaction. Behaviour now pinned by a test, so implementing
      it is a visible change.
    - **C14** — `install(EXPORT)`; needs a real decision about exporting
      FetchContent dependencies.
    - **Naming** — `Axolotl` / `AxolotlDB` / `AxolotlDB` / `AxolotException`.
      Pick one; nobody but you can make this call.
    - **Formal-UB note** — `reinterpret_cast<PageHeader*>` over a `std::byte`
      array. Works everywhere real, worth understanding before leaning harder
      on it.
    - **Third-party warning noise** — a full rebuild emits a wall of C4996
      (`STL4043`, `stdext::checked_array_iterator`) from spdlog's bundled `fmt`
      against the current MSVC STL. None of it is our code — an incremental
      build hides it, which is why it went unnoticed. Fix by marking the
      FetchContent include directories `SYSTEM`, not by defining
      `_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS`, which would also silence our
      own translation units.

Verify fixes under sanitizers — **A2** and **A3** are exactly what these catch:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure --no-tests=error
```

On Windows, MSVC supports `/fsanitize=address`; UBSan needs WSL or the Docker
`dev` image.
