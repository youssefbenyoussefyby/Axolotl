# AxolotlDB — Architecture, Audit & Implementation Roadmap

> **Supersedes** `axolotl_progress_report.md` as the project's source of truth.
> **Companion to** [`audit.md`](audit.md), which remains the record of findings A1–A8,
> B1–B8, C1–C14 and D1–D9. This document does not repeat those; it adds N1–N14,
> which that audit did not cover, and specifies everything after Phase 1.
>
> **Branch** `Axolot` · **Baseline commit** `d9d5a9f` · **C++20** · namespace `axolotldb`

---

## Contents

- [1. Status](#1-status)
- [2. Phase 1 audit — findings N1–N14](#2-phase-1-audit--findings-n1n14)
- [3. Architecture](#3-architecture)
- [4. On-disk format catalogue](#4-on-disk-format-catalogue)
- [5. Phases P0–P10](#5-phases-p0p10)
- [6. Cross-cutting](#6-cross-cutting)
- [7. Amendment log](#7-amendment-log)

---

## 1. Status

### 1.1 The one-page table

| Phase | Scope | State | Size | Guide |
|---|---|---|---|---|
| **P0** | Hardening — N1–N4, N10–N14; green CI baseline | **DONE** — 152/152, 0 warnings | S | [phases/P0.md](phases/P0.md) |
| **P1** | Storage core — Page, FileManager, BufferPool, Tuple, VectorHeap | **DONE** | — | [phases/P1.md](phases/P1.md) |
| **P2** | Storage services — StorageManager, PageGuard, TableHeap, Catalog, WAL contract | **DONE** — 217/217, gate green | L | [phases/P2.md](phases/P2.md) |
| **P3** | SQL front end — lexer, parser, binder, logical plan | **DONE** | M | [phases/P3.md](phases/P3.md) |
| **P4** | Execution engine — Volcano operators, rule-based rewrites | **IN PROGRESS** — 4.2 milestone reached (SeqScan + Filter + Projection end-to-end); 4.3 DML next | XL | [phases/P4.md](phases/P4.md) |
| **P5** | WAL + recovery + page latching | Not started | L | [phases/P5.md](phases/P5.md) |
| **P6** | Relational indexing — B+Tree, hash index, IndexScan | Not started | L | [phases/P6.md](phases/P6.md) |
| **P7** | Vector indexing / ANN — brute force, then HNSW | Not started | XL | [phases/P7.md](phases/P7.md) |
| **P8** | Optimizer + statistics — cost model, join ordering | Not started | M | [phases/P8.md](phases/P8.md) |
| **P9** | Client / server / CLI | Not started | M | [phases/P9.md](phases/P9.md) |
| **P10** | Embeddings — provider interface, ONNX optional | Not started | M | [phases/P10.md](phases/P10.md) |

The **Guide** column points at [`docs/phases/`](phases/README.md) — per-phase reading lists
(docs, books, lectures, code to read) and commit-sized implementation checklists. This
document stays the source of truth for signatures, formats and gates; the guides say what to
read before writing each phase and break it into units. Where the two disagree, this one
wins.

Sizes are **relative, for a solo part-time developer — not calendar commitments.**
They are the least reliable thing in this document. Read them as "P4 is roughly
twice P3" and nothing more. The original roadmap's week ranges are dropped
deliberately: they were derived from file counts, not from behaviour, which is the
same mistake section E of `audit.md` identified.

### 1.2 What actually works today

The storage engine stores and retrieves rows, including rows with embeddings larger
than a page, and survives a full close-and-reopen. That is a real database storage
layer, and it is well tested.

| Component | File | State |
|---|---|---|
| `Page` | `src/storage/page.cpp` | Slotted page, insert/get/delete, tombstones, dirty flag outside the bytes |
| `FileManager` | `src/storage/file_manager.cpp` | page_id ↔ offset, real `fsync` via a side descriptor, failure latch |
| `BufferPool` | `src/storage/buffer_pool.cpp` | LRU, pin/unpin, strong exception guarantee on eviction |
| `Tuple` | `src/storage/tuple.cpp` | `[bitmap ǀ fixed ǀ uint16 length table ǀ var data]`, region-validated |
| `VectorHeap` | `src/storage/vector_heap.cpp` | Fixed-stride computed addressing, `Sync()` as commit point, `Rollback()` |
| `ConfigManager` | `src/utils/config.cpp` | YAML + `AxolotlDB_*` env overrides, full validation |
| `Logger` | `src/utils/logger.cpp` | spdlog facade, console + rotating file |
| `Allocator` | `src/utils/memory.cpp` | Raw / tracking / guarded, honours over-alignment |

### 1.3 Test inventory — 144 cases

| Suite | Cases | What it pins |
|---|---|---|
| `vector_heap_test` | 43 | Both addressing regimes, page-boundary seam, 2- and 3-page records, reopen, commit/rollback, heap-id provenance |
| `tuple_test` | 33 | NULL var columns in every position, raw-byte assertions independent of `Deserialize`, truncation, type mismatch, 65535 boundary |
| `buffer_pool_test` | 29 | Dirty semantics, eviction survival when unpinned *clean*, LRU order, `FailedFetchDoesNotConsumeFrames`, slot directory |
| `file_manager_test` | 13 | Sync idempotence, `FlushAllPagesCallsSync`, reopen round trips, fd-leak ordering |
| `memory_test` | 8 | Lifecycle, guard failure, over-alignment in all three modes |
| `config_test` | 6 | Defaults, YAML, validation, env overrides |
| `storage_test` | 4 | The original four, rewritten to assert content rather than ids |
| `persistence_test` | 2 | **The Phase 1 gate** — 500 rows, 1536-dim embeddings, forced eviction, byte-identical after reopen |
| `logger_test` | 1 | Writes to a file |

Five further files exist that assert nothing but `SUCCEED()`: `btree_test`,
`hnsw_test`, `parser_test`, `query_test`, `transaction_test`. See [N14](#n14--minor).

### 1.4 What does not exist

Everything above the storage layer. 44 source files across `catalog/`, `executor/`,
`index/`, `network/`, `optimizer/`, `parser/`, `vector/`, plus `storage/wal.cpp` and
`storage/transaction.cpp`, are zero bytes. `include/` has no headers at all outside
`storage/` and `utils/`.

### 1.5 The honest scorecard

Phase 1's storage engine is substantially done and genuinely well tested. What is
missing is (a) four blocking defects below, and (b) any layer between `Page` and a
future executor. `README.md`'s "Implement storage foundation" box should be ticked;
a "storage services" line should be added, unticked.

---

## 2. Phase 1 audit — findings N1–N14

`audit.md`'s A1–A8, B1–B6, C1–C13 and D1–D9 were re-derived from source for this
document and are confirmed fixed. B7 (page compaction), B8 (page latch) and C14
(`install(EXPORT)`) remain open there and are correctly characterised.

Everything below is **new** — not covered by `audit.md`, which audited `Tuple`,
`FileManager`, `BufferPool` and `VectorHeap` but never audited `Page` itself, never
compiled against the Linux CI toolchain, and never asked what the layer above would
need.

All line references verified against the working tree at time of writing.

### Blocking — nothing else is verifiable until these are done

#### N1 — `Page` never validates a header or slot entry that came off disk

**Severity: out-of-bounds read, and an out-of-bounds write.**

[`file_manager.cpp:134`](../src/storage/file_manager.cpp#L134) memcpys 4096 raw disk
bytes straight into `Page::data_`:

```cpp
file_.read(reinterpret_cast<char*>(out.RawData().data()), page_size_);
```

`Page::Header()` then reinterprets the first 16 of those bytes as truth. Nothing
checks them. Three consequences:

**1. `GetTuple` over-reads the slot array.**
[`page.cpp:78`](../src/storage/page.cpp#L78) guards only `slot >= hdr.slot_count`,
and `slot_count` is disk data:

```
slot_count = 60000  (corrupt)
GetTuple(59999)  ->  slot < slot_count, passes
                 ->  Slots()[59999] reads at data_ + 16 + 239996
                 ->  ~236 KB past a 4096-byte array
```

**2. The returned span can point almost anywhere.**

```cpp
return std::span<const std::byte>(data_.data() + entry.offset, entry.length);
```

Neither `offset` nor `length` is validated, and both are `uint16_t` — so the span
can extend ~127 KB past the buffer. It goes straight into `Tuple::Deserialize`,
whose A2 region checks are all *relative to that already-bogus span* and therefore
pass. A2 hardened `Tuple` against a short buffer; it cannot help against a long one
that lies about where it starts.

**3. `InsertTuple` over-writes.** [`page.cpp:68`](../src/storage/page.cpp#L68):

```
free_ptr = 65535, slot_count = 0
  SlotArrayEnd() = 16
  FreeSpace()    = 65535 - 16 - 4 = 65515
InsertTuple(100 bytes)  ->  65515 >= 100, passes
                        ->  free_ptr becomes 65435
                        ->  memcpy(data_ + 65435, ..., 100)
                        ->  out-of-bounds WRITE, ~61 KB past the array
```

**Why this was missed.** It is the A2 bug class — trusting sizes that came off
disk — applied to `Page` rather than `Tuple`. A2 was found and fixed thoroughly;
the `Page` equivalent was never looked at, because the audit's scope was the
serialisation layer.

**Reachability.** Today only through disk corruption, external truncation, or a
torn write. That is exactly the failure mode P5's recovery has to survive, and
exactly why `_pad[8]` was reserved for a checksum. Untested code that runs only
after a crash is the code most worth hardening.

**Fix.** A validation pass over the header, called from `ReadPage` and from the
`Page(const std::array&)` constructor:

```cpp
/// Validate that this page's header and slot directory are self-consistent.
///
/// Must be called on every Page whose bytes came from outside this process --
/// FileManager::ReadPage and the raw-array constructor. A Page built by the
/// default constructor and only ever mutated through InsertTuple/DeleteTuple
/// maintains these invariants by construction.
///
/// @throws AxolotlDB::utils::CorruptPageException naming the violated invariant.
void ValidateOrThrow() const;
```

Invariants asserted, in this order — each depends on the previous holding:

| # | Invariant | Guards |
|---|---|---|
| 1 | `slot_count <= (PAGE_SIZE - sizeof(PageHeader)) / sizeof(SlotEntry)` | consequence 1 |
| 2 | `SlotArrayEnd() <= free_ptr` | slot array and payloads do not overlap |
| 3 | `free_ptr <= PAGE_SIZE` | consequence 3 |
| 4 | for each live slot: `offset >= free_ptr && offset + length <= PAGE_SIZE` | consequence 2 |

Invariant 1 must be checked before `SlotArrayEnd()` is trusted, and invariant 4
requires 2 and 3 to already hold — the check order is part of the fix, not
incidental.

> **Where these live, decided during P0.** The obvious home is
> `FileManager::ReadPage` — validate once, on the way in. It does not work yet,
> for two reasons that are both features:
>
> * `AllocatePage` zero-fills, so a freshly allocated page read back has
>   `free_ptr == 0` and fails invariant 2. Legitimate page, rejected.
> * `VectorHeap` data pages have no `PageHeader` at all — records start at byte
>   0 — so their first 16 bytes are float payload. Every `VectorHeap::Get`
>   would throw.
>
> Telling the two apart needs a page-type tag, which is [N9](#n9--vectorheap-data-pages-have-nowhere-to-put-a-page_lsn)'s
> work in P2. So the invariants are enforced by the slotted-page accessors
> (`GetTuple`, `InsertTuple`, `DeleteTuple`) in O(1) instead — which is where
> the out-of-bounds access actually happened, so nothing can read or write out
> of bounds either way. `ValidateOrThrow()` is the explicit whole-page pass for
> callers that would rather reject a bad page up front than on first use.
> `FreeSpace()` stays `noexcept` and unvalidated: on a corrupt page it returns
> a wrong number, but it only reads the header and dereferences nothing.

A new `CorruptPageException : StorageException` carrying the page id and the
violated invariant. `InvalidPageException` would be wrong here: it means "you asked
for a page that does not exist", and conflating "your argument is bad" with "the
disk is bad" costs you the diagnostic exactly when you need it.

#### N2 — `std::format` without `#include <format>`

**Severity: six of seven CI surfaces cannot compile. There is no green baseline.**

[`file_manager.cpp:32`](../src/storage/file_manager.cpp#L32):

```cpp
throw std::invalid_argument(std::format("Page Size should be less then {}", PAGE_SIZE));
```

libstdc++ shipped `<format>` in GCC 13. Nothing earlier has the header at all, so
this is not a missing-include warning — `std::format` does not exist. It compiles
today only on MSVC, where it arrives transitively.

> **Update, found during P0.** By the time P0 ran, this line had been changed to
> `std::invalid_argument("Page Size should be less then " + PAGE_SIZE)`. That is
> *worse*: `const char* + 4096` is pointer arithmetic that walks a 30-character
> string literal 4096 bytes past its end, and constructing a `std::string` from
> the result is a wild read. Undefined behaviour on every platform, MSVC
> included, rather than a compile error on six of seven. Fixed with
> `std::to_string`, which is what the rest of the file already uses.

| Surface | Toolchain | |
|---|---|---|
| `build-linux` ubuntu-22.04 × gcc | gcc-11 | ✗ |
| `build-linux` ubuntu-22.04 × clang | libstdc++-12 | ✗ |
| `sanitizers` | ubuntu-22.04 | ✗ |
| `coverage` | ubuntu-22.04 | ✗ |
| `code-quality` | ubuntu-22.04 | ✗ |
| `docker` | `FROM ubuntu:22.04` | ✗ |
| `build-windows` | MSVC | ✓ |

The message is also wrong twice over: it says *"should be less then"* while the
check is `page_size != PAGE_SIZE` (A6 deliberately closed both directions), and
"then" should be "than".

**Fix.** Plain string concatenation, matching every other message in the file:

```cpp
throw std::invalid_argument("FileManager: page_size must be exactly " +
                            std::to_string(PAGE_SIZE) + ", got " +
                            std::to_string(page_size));
```

This is one line and it is **the highest-leverage change in this document.** Every
phase gate below is stated as "these tests pass"; that is unverifiable until the
project compiles on the platforms the tests run on. This is what makes P0 a phase
rather than a chore.

#### N3 — `UnpinPage` silently discards the dirty flag on an unbalanced unpin

[`buffer_pool.cpp:101`](../src/storage/buffer_pool.cpp#L101):

```cpp
if (frame.pin_count <= 0) {
    return;                       // <-- returns before the dirty flag is applied
}
frame.pin_count--;
if (is_dirty) {
    frame.page.MarkDirty();
}
```

An over-unpin drops a modification with no error, in a layer where every other
misuse throws. B1 makes this survivable in the common case — the page marks itself
in `InsertTuple`/`DeleteTuple` — so the blast radius is callers that mutate through
`RawData()`. That is precisely what `VectorHeap::Put` does.

**Fix.** Two changes, and the order matters:

```cpp
if (is_dirty) {
    frame.page.MarkDirty();       // record the modification unconditionally
}
if (frame.pin_count <= 0) {
    throw AxolotlDB::utils::InvalidPageException(
        "UnpinPage: page " + std::to_string(page_id) + " is not pinned");
}
frame.pin_count--;
```

Applying `is_dirty` first means a buggy caller loses its *pin accounting*, not its
*data*. Throwing rather than returning matches A5's fail-fast posture: an unbalanced
unpin is a programming error, and a silent one costs you the stack trace at the
point where it is still cheap to find.

#### N4 — Pin leaks on the exception paths, and no RAII to prevent them [CLOSED in P2]

`FetchPage` returns a raw `Page*` under a *"caller MUST call UnpinPage"* comment.
Any throw between fetch and unpin leaks the pin permanently: the frame is never
evictable again, the pool degrades frame by frame, and it dies with a
`BufferPoolExhaustedException` nowhere near the cause. This is A3's failure shape,
relocated from inside the pool to its callers.

> **Corrected during P0 — the original claim here was too strong.** It said a
> throw on iteration *i* leaks *i* pins in both loops. Reading them again: each
> iteration acquires and releases within itself, so earlier iterations have
> already unpinned. `Put`'s loop holds a pin only across `memcpy` and
> `MarkDirty`, neither of which can throw, so **`Put` leaks nothing.**
>
> `Get` did leak, but exactly **one** pin, and for a subtler reason: it built a
> `std::vector<float>` temporary *between* `FetchPage` and `UnpinPage`, and that
> allocation can throw. One leaked pin costs a frame for the life of the pool,
> so it still matters — but the accurate statement is "one pin, in `Get`, on
> allocation failure", not "*i* pins in both".
>
> The fix is better than a guard: the temporary was pointless — it copied page
> bytes into a vector, then element-by-element into `result`. Copying straight
> into `result` **deletes the throw point** rather than guarding it, and drops a
> redundant copy of every page on the read path. Nothing between fetch and
> unpin can throw now, in either loop.

**Fix, in two places, deliberately.** The general mechanism is `PageGuard` — but it
belongs in P2, not P0. P2 changes `BufferPool`'s key type from `PageId` to a
composite (see [N6](#n6--every-vectorheap-owns-a-private-filemanager-and-a-private-buffer-pool))
and therefore rewrites every call site anyway; introducing the guard in P0 means
touching them all twice.

So: **P0 fixes the two leaks** with a local scope guard, closing the live defect.
**P2 introduces `PageGuard`** as the general mechanism, in the pass that already
rewrites those signatures. See [P2](#p2--storage-services-l) for the interface.

### Design gaps that shape later phases

#### N5 — `GetTuple` hands out a span into a frame that can be evicted underneath it [CLOSED in P2]

`Page::GetTuple` returns `std::span<const std::byte>` pointing into the frame's
buffer. Zero-copy is the right call — the alternative is a heap allocation per tuple
read — but nothing ties the span's lifetime to the pin that keeps those bytes valid.

Harmless today because every caller consumes the span before unpinning. It stops
being harmless the first time a P4 operator holds a tuple across an operator
boundary: a `Next()` up the pipeline can trigger a fetch, which can trigger an
eviction, which rewrites the bytes the caller is still reading. Silent wrong data,
and non-deterministic — it only fires under memory pressure.

`PageGuard` answers this too. The contract becomes *"a span obtained through a guard
must not outlive that guard"*, documented on both `GetTuple` and `PageGuard`, and
pinned by a test that holds a span across a forced eviction and asserts the guard
prevents it.

#### N6 — every `VectorHeap` owns a private `FileManager` and a private buffer pool [CLOSED in P2]

[`vector_heap.h:105-106`](../include/storage/vector_heap.h#L105):

```cpp
mutable FileManager file_manager_;
mutable BufferPool  buffer_pool_;
```

Ten vector columns means ten independent 128-frame caches (512 KB each) that nothing
can rebalance, plus another for the row store. Consequences:

- **No global memory budget.** `StorageConfig` cannot express "use 256 MB of cache",
  only "use 128 frames, N times, for an N nobody counts".
- **No sharing.** A query scanning a table and reading its embeddings has two working
  sets that cannot trade frames, even when one is idle.
- **No group commit.** `Sync()` on one heap can never join the row store's, so a row
  plus its embedding costs two fsyncs where one would do.

**Root cause, and why it must be fixed early.** `PageId` is `int32_t` and
file-local. One pool structurally *cannot* address two files — the page table key
is ambiguous. Fixing it changes the key type, which touches every storage call
site. There are four today. By P7 there will be hundreds.

**Decision: fix in P2.** `StorageManager` owns `FileId → FileManager` and one
`BufferPool`. See [P2](#p2--storage-services-l).

#### N7 — `mutable` members, and no thread-safety contract [CLOSED in P2]

The `mutable` on both members above exists solely so `Get() const` can pin pages.
Two costs: `const VectorHeap&` conveys nothing, and it hides that a read mutates
shared cache state — which matters the moment two threads share a heap. The N6
inversion removes it: the heap holds a `StorageManager&`, and `const` becomes
honest, because pinning mutates the manager rather than the heap.

Separately, `VectorHeap` has no mutex, `vector_count_` and `durable_count_` are
plain `std::size_t`, and the class carries none of the explicit thread-safety
documentation that `buffer_pool.h` earned in B8. A concurrent `Put` is a data race
on the id counter, which hands two callers the same `VectorId`. That should be
written down now and enforced in P5.

#### N8 — `VectorHeap` addressing overflows silently at the edges

[`vector_heap.cpp:81`](../src/storage/vector_heap.cpp#L81):

```cpp
page_id = static_cast<PageId>(1 + id * pages_per_records);
```

`id` is `uint64_t`, `PageId` is `int32_t`, and the narrowing is unchecked. Likewise
`page_span` is a `uint16_t` computed from `(stride + PAGE_SIZE - 1) / PAGE_SIZE`,
which a dimension near `UINT32_MAX` overflows outright — producing a *smaller*
span than the record needs, so `Put` writes a truncated vector and `Get` returns
one, with no error.

The real ceilings are far away (int32 pages caps a heap file at 8 TiB). But B2's
entire argument for the heap design was that silent wrong answers are the failure
mode to engineer out, and these are silent wrong answers.

**Fix.** Reject an oversized stride at construction — the A6 pattern, fail fast on
a parameter the design cannot honour — and range-check the `PageId` narrowing in
`LocateVector`, throwing rather than truncating.

#### N9 — `VectorHeap` data pages have nowhere to put a `page_lsn` [CLOSED in P2]

Vector records are memcpy'd starting at byte 0 of each data page. Those pages have
no `PageHeader` at all — the heap does not need one, since addressing is computed
rather than stored.

P5's redo needs a `page_lsn` on every page it recovers, to know whether a logged
change is already reflected. Vector data pages have no field to hold one.

Two options, and **the choice must be made before HNSW exists**, because an HNSW
graph layered on the same addressing inherits whichever is chosen:

| Option | Cost | Benefit |
|---|---|---|
| **A** — give vector data pages the 16-byte `PageHeader` | ~0.4% density; `records_per_page` arithmetic changes; the existing addressing tests need updating | One uniform recovery story. Every page in the system recovers the same way. |
| **B** — declare vector heaps re-derivable, excluded from redo | No format change | Recovery must then re-`Put` every vector from the log, or the heap must be rebuilt from the row store. Both are more machinery than option A. |

**Recommendation: A.** Uniformity is worth 0.4%, and option B's "re-derive" is not
actually cheaper — it is a second recovery path, written and tested separately, for
the subsystem this database exists for.

**This is the mechanical reason WAL cannot wait until after HNSW.** The decision is
cheap now and expensive once a graph depends on the layout.

#### N10 — `AllocatePage` flushes per call, and can desync `page_count_` from the file

[`file_manager.cpp:117`](../src/storage/file_manager.cpp#L117) — `file_.flush()` on
every allocation. Two issues:

1. **Throughput.** Bulk load becomes one stream flush per 4 KB. Not a correctness
   problem, but it will dominate any P2 load benchmark and is worth removing before
   anyone measures.
2. **Consistency.** On a partial write, `page_count_.fetch_sub` rewinds the
   in-memory counter while the bytes may already be on disk. Reopening derives
   `page_count_ = file_size / page_size` and counts a page the previous run
   disowned. The disowned page then holds garbage that a later `FetchPage` will
   read as a valid `Page` — which is [N1](#n1--page-never-validates-a-header-or-slot-entry-that-came-off-disk)'s
   trigger, reachable without any external corruption at all.

Low severity in isolation, but N10 + N1 together is a real path from a failed write
to an out-of-bounds read. Both are P0. The proper answer to the second is a free
list, which P2 needs anyway for `TableHeap`'s free-space map.

### Process and hygiene

#### N11 — the design documents are gitignored

`.gitignore` ends with:

```
# Notes and working documents: only README.md is tracked.
# Everything else (progress reports, docs/, ADRs) stays local.
*.md
!README.md
```

This is deliberate, not a bug. The consequence is still worth stating plainly:
`audit.md` (1151 lines), this document, and every other design record exist only on
one machine. A fresh clone gets none of it, CI cannot reference it, and the
reasoning behind every decision in the storage layer — much of which no longer
exists in the code comments either (see the note in §6.5) — is one disk failure from
gone.

**Applied:** the rule is narrowed to keep scratch notes local while shipping the
architecture record:

```
*.md
!README.md
!docs/*.md
```

Progress reports and ADR drafts outside `docs/` still stay local. To revert, delete
the `!docs/*.md` line.

#### N12 — stale docs teach the wrong format

| Doc | Wrong | Reality |
|---|---|---|
| `tuple_explained.md` | Offsets-plus-`0xFFFF`-sentinel layout; `Value` as a fat struct with `.str`/`.vec`; VECTOR variable-length; presents the A1 bug as still live | Replaced by A1 (length table) and B5 (variant, VECTOR fixed at 8 bytes) |
| `core_documentation.md` | `LogLevel::ERROR`; `cli/main.cpp` "90 lines, no query loop" | B3 renamed to `kError`; the CLI is 451 lines with a REPL |
| `axolotl_progress_report.md` | "All 6 storage source files are empty stubs"; "Phase 1 — 60% complete" | Five are implemented and tested |
| `README.md` | "Implement storage foundation" unticked | Done |

`tuple_explained.md` is the dangerous one — it is a teaching document that
confidently explains a format the code no longer uses, including a worked example of
the A1 corruption presented as current behaviour. Rewrite it against the length-table
format or delete it; leaving it is worse than having no document.

#### N13 — build-file duplication

```cmake
add_library(AxolotlDB ${LIB_SOURCES} include/storage/vector_heap.h)
add_executable(AxolotlDB-cli cli/main.cpp src/storage/vector_heap.cpp)
```

The first lists a header as a source (harmless, but it means the glob is not
actually the source of truth). The second compiles `vector_heap.cpp` a second time,
into a target that already links the library containing it — an ODR violation
waiting for that file to gain a non-inline global. Both look like IDE artefacts.

#### N14 — minor

- **Five tests that assert nothing.** `btree_test`, `hnsw_test`, `parser_test`,
  `query_test`, `transaction_test` contain only `SUCCEED()`. This is the
  files-not-behaviour pattern `audit.md` section E identified, reproduced inside the
  test suite — and worse there, because a green suite that proves nothing is how C2
  went unnoticed for so long. Delete them; each phase below creates its own.
- **`body.json`** is an unrelated tracked file at the repo root
  (`{"client_name":"h1-yby-dcr-test", ...}`).
- **`OccupiedFrames()` is `noexcept` but takes a lock.** `std::mutex::lock` can
  throw `std::system_error`; inside a `noexcept` function that is `std::terminate`.
  Drop the `noexcept` or use a non-throwing path.
- **`FreeSpace()` is undocumented and surprising.** It returns *the largest payload
  that can still be inserted* — already net of one `SlotEntry` of overhead — not
  "free bytes". Correct, and correctly tested by
  `FreeSpaceShrinksByPayloadPlusSlotOverhead`, but P2's free-space map consumes this
  value and a wrong reading of it produces an off-by-four in every page.

---

## 3. Architecture

### 3.1 Where the layers sit after P2

```
                         ┌──────────────────────────────┐
   P9   network          │  Server / Session / CLI      │
                         └──────────────┬───────────────┘
                                        │ SQL text
                         ┌──────────────▼───────────────┐
   P3   front end        │  Lexer → Parser → Binder     │──► Catalog
                         └──────────────┬───────────────┘
                                        │ LogicalPlan
                         ┌──────────────▼───────────────┐
   P4/P8 planning        │  Rewriter → Optimizer        │──► Statistics
                         └──────────────┬───────────────┘
                                        │ PhysicalPlan
                         ┌──────────────▼───────────────┐
   P4   execution        │  Executor  (Volcano)         │
                         └───┬──────────────────────┬───┘
                             │                      │
              ┌──────────────▼──────┐   ┌───────────▼──────────┐
   P2/P6/P7   │ TableHeap │ B+Tree  │   │ VectorHeap │ HNSW    │
              └──────────────┬──────┘   └───────────┬──────────┘
                             │      RID / VectorRef │
                         ┌───▼──────────────────────▼───┐
   P2   services         │  PageGuard                   │
                         ├──────────────────────────────┤
                         │  StorageManager              │
                         │    BufferPool  (one, shared) │◄── ILogManager  (P5)
                         │    FileId → FileManager      │
                         └──────────────┬───────────────┘
                                        │ PageRef
                         ┌──────────────▼───────────────┐
   P1   core             │  Page  ·  FileManager        │
                         └──────────────────────────────┘
```

Three things to note about this shape:

**One buffer pool, many files.** [N6](#n6--every-vectorheap-owns-a-private-filemanager-and-a-private-buffer-pool)
is why. Everything that stores anything — tables, indexes, vector heaps, the
catalog — is a file registered with the `StorageManager` and gets frames from the
same budget.

**Everything above P2 addresses data by `RID` or `VectorRef`, never by `PageRef`.**
That is the layering boundary. If a P4 operator ever needs a `PageRef`, something
has leaked.

**`ILogManager` enters the pool from the side, not from above.** The WAL rule — never
write a page before the log record describing it — is enforced in the one place every
page write funnels through, rather than at each of the dozens of call sites that
could forget.

### 3.2 The four load-bearing invariants

Stated once, here, because everything below depends on them:

1. **A page's bytes on disk are self-describing and self-validating.** Header,
   checksum, slot directory. Nothing outside the page is needed to know whether it
   is intact. ([N1](#n1--page-never-validates-a-header-or-slot-entry-that-came-off-disk))
2. **A pin is released exactly once, by a destructor.** No manual unpin survives P2.
   ([N4](#n4--pin-leaks-on-the-exception-paths-and-no-raii-to-prevent-them))
3. **A reference carries proof of what it refers to.** `VectorRef` carries its
   `HeapId` (B5); `RID` carries its page. A reference resolved against the wrong
   container throws rather than returning plausible data.
4. **Durability begins when `Sync()` returns, and not before.** `WritePage` is
   buffered. This is A4's contract and it propagates upward: `TableHeap::Insert`
   returns an RID that is *visible*, not *durable*.

---

## 4. On-disk format catalogue

Every byte layout in one place. Native endianness throughout — a deliberate
non-goal is cross-architecture file portability, and revisiting it means byte-swapping
in exactly the places listed here.

### 4.1 Page — 4096 bytes, fixed at compile time

```
 offset  size  field
 ──────  ────  ─────────────────────────────────────────────────
      0     4  page_id        int32
      4     2  slot_count     uint16
      6     2  free_ptr       uint16   payloads grow down from here
      8     8  _pad[8]        RESERVED ─┐
                                        ├─ P2 claims this, see below
     16   4·n  SlotEntry[n]   {uint16 offset, uint16 length}
              ... free space ...
free_ptr        tuple payloads, growing backward toward free_ptr
   4096
```

`_pad[8]` was reserved by B1, which moved the dirty flag out of the page bytes. **P2
claims it:**

```
      8     8  page_lsn       uint64   last log record that modified this page (P5)
```

and the checksum needs its own space, which the header does not have. **P2 grows
`PageHeader` to 24 bytes** to add:

```
     16     4  checksum       uint32   CRC32 over bytes [20, 4096)
     20     4  _pad2          RESERVED
```

with the `static_assert` updated to 24 and the slot array starting at 24. The
checksum deliberately excludes itself and `_pad2`, so computing it is a single
contiguous CRC with no hole to skip.

**Why grow rather than pack both into 8 bytes.** A 32-bit LSN wraps after 4 billion
log records, which is reachable, and a 16-bit checksum has a 1-in-65536 collision
rate against exactly the random corruption it exists to catch. Eight bytes of page
is 0.2%.

**Migration.** None — nothing has shipped. Existing test fixtures regenerate.

### 4.2 Tuple wire format

```
 ┌────────────┬─────────────────┬──────────────────┬──────────────┐
 │ ver  1B    │ null bitmap     │ fixed columns    │ uint16 length│  var data
 │            │ ceil(N/8) bytes │ packed, NULLs=0B │ table, one   │  payloads
 │            │                 │                  │ per VARCHAR  │
 └────────────┴─────────────────┴──────────────────┴──────────────┘
```

Current format is everything after `ver`. **P2 adds the leading version byte** — see
[P2's format-versioning note](#tuple-format-versioning). Fixed widths:

| Type | Bytes | Note |
|---|---|---|
| `INT32`, `FLOAT32` | 4 | |
| `INT64`, `FLOAT64` | 8 | |
| `BOOLEAN` | 1 | |
| `VECTOR` | 8 | a `VectorRef`, not the floats (B5) |
| `VARCHAR` | variable | length in the table, capped at `MAX_VAR_COLUMN_BYTES` = 65535 (A8) |

A NULL column sets its bitmap bit and contributes **zero** bytes to both the fixed
region and the var data; a NULL VARCHAR's length slot is written as `0`, its true
length. That is A1's fix and the reason no slot's meaning depends on a neighbour's.

### 4.3 VectorRef — 8 bytes, a fixed-width column payload

```
      0     4  heap_id        uint32   0 = INVALID_HEAP_ID, reserved
      4     4  index          uint32
```

Self-identifying by design (B5): a bare index used against the wrong heap resolves
to a real, plausible vector belonging to another column, and nothing downstream can
tell. `VectorHeap::Get` rejects a foreign `heap_id`.

### 4.4 VectorHeap file

```
 page 0        VectorHeapHeader
      0     4  dimension      uint32
      4     4  heap_id        uint32
      8     8  vector_count   uint64   THE COMMIT POINT -- written last, by Sync()
 page 1..n     records, fixed stride = dimension * 4
```

Two addressing regimes, chosen by `stride` vs `PAGE_SIZE`:

```
stride <= PAGE_SIZE     packed, floor(PAGE_SIZE / stride) records per page
                        page = 1 + id / rpp,  offset = (id % rpp) * stride

stride >  PAGE_SIZE     page-aligned runs of ceil(stride / PAGE_SIZE) pages
                        page = 1 + id * ppr,  offset = 0
```

No per-record pointer is ever stored — a record's location is *computed*. That is
what makes the heap's arithmetic worth its own 43 tests.

**P2 change per [N9](#n9--vectorheap-data-pages-have-nowhere-to-put-a-page_lsn):**
data pages gain the 24-byte `PageHeader`, so the usable region per page becomes
`PAGE_SIZE - sizeof(PageHeader)` and both regimes' arithmetic changes accordingly.
The existing boundary tests (`RecordRollsOntoTheNextPageAtTheBoundary`,
`VectorExactlyOnePageWide`) are exactly the ones that must be updated, and their
failing is the signal the change landed correctly.

### 4.5 Catalog — a table inside the database

Not a JSON sidecar. One durability story, one recovery path, and it is what
PostgreSQL and SQLite do. Full schema in [P2](#p2--storage-services-l).

**The bootstrap**, since it is the non-obvious part: the catalog table's own schema
cannot be read from the catalog. It is hardcoded in the binary, at a fixed
`FileId`/`PageId`, and the catalog is opened by constructing that known `Schema` in
code and handing it to a `TableHeap`. Everything else — user tables, columns,
indexes, heap ids — is then read normally.

### 4.6 WAL record — P5

Specified in [P5](#p5--wal--recovery--latching-l). The contract that P2 must
satisfy in advance: every record carries `{lsn, txn_id, type, page_ref}` and the
pool refuses to write a page whose `page_lsn` exceeds the durable log tail.

---

## 5. Phases P0–P10

### How a phase runs

**Division of labour.** Youssef writes all of `src/` and `include/`. Claude writes
tests and does review passes.

**Test-first.** Each phase begins with Claude writing the gate test and unit suites
against the interfaces specified below. They fail — that is the point. Implementation
proceeds until green. This makes `audit.md` section E's rule operational rather than
aspirational:

> A phase is complete when a test proves the behaviour, not when the file exists.

**Consequence worth naming.** Because those tests compile against the signatures in
this document, **the signatures are load-bearing.** Changing the shape of
`TableHeap` or `PageGuard` is a conversation at the start of a phase, not a merge
conflict in the middle of one. If a signature here is wrong, say so before the tests
are written — and record the change in the [amendment log](#7-amendment-log).

**Definition of done — all four, every phase:**

1. The gate test passes, and demonstrably failed before the work.
2. All seven CI surfaces green.
3. No new compiler warnings; ASan and UBSan clean.
4. This document's status table updated.

---

### P0 — Hardening (S)

→ Resources and checklist: [`phases/P0.md`](phases/P0.md)

Close the four blocking findings and re-establish a green baseline. Everything else
in this document is gated on it.

**Order is not arbitrary:**

| # | Work | Why here |
|---|---|---|
| 1 | [N2](#n2--stdformat-without-include-format) — drop `std::format` | Nothing is verifiable until the project compiles where the tests run |
| 2 | [N1](#n1--page-never-validates-a-header-or-slot-entry-that-came-off-disk) — `Page::ValidateOrThrow` | The memory-safety defect |
| 3 | [N3](#n3--unpinpage-silently-discards-the-dirty-flag-on-an-unbalanced-unpin) — `UnpinPage` ordering | Two lines, silent data loss |
| 4 | [N4](#n4--pin-leaks-on-the-exception-paths-and-no-raii-to-prevent-them) — scope-guard the two `VectorHeap` loops | The live leak, not yet the general mechanism |
| 5 | [N10](#n10--allocatepage-flushes-per-call-and-can-desync-page_count_-from-the-file) | Compounds N1 |
| 6 | [N11](#n11--the-design-documents-are-gitignored)–[N14](#n14--minor) | Hygiene |

**New interface:**

```cpp
// include/storage/page.h
class Page {
public:
    /// Validate that this page's header and slot directory are self-consistent.
    /// Call on every Page whose bytes came from outside this process.
    /// @throws AxolotlDB::utils::CorruptPageException naming the violated invariant.
    void ValidateOrThrow() const;
};

// include/utils/errors.h
class CorruptPageException : public StorageException {
public:
    CorruptPageException(PageId page_id, const std::string& invariant);
};
```

**Gate.** The existing 144 tests green **on all seven CI surfaces** — which, per
[N2](#n2--stdformat-without-include-format), has probably never happened — plus a
new `PageCorruption.*` suite:

| Test | Feeds |
|---|---|
| `SlotCountBeyondPageCapacityThrows` | `slot_count = 60000` |
| `FreePtrBeyondPageSizeThrows` | `free_ptr = 65535` |
| `FreePtrBelowSlotArrayThrows` | overlapping regions |
| `SlotOffsetOutsidePayloadRegionThrows` | `offset` past `PAGE_SIZE` |
| `SlotExtendingPastPageEndThrows` | `offset + length > PAGE_SIZE` |
| `ValidPageWithMaximumSlotsIsAccepted` | the exact boundary, so the guard is not off by one |
| `CorruptPageSurvivesReadPathWithoutOutOfBounds` | full `ReadPage` → `GetTuple` → `InsertTuple` path |

Each writes malformed bytes directly to the file, reopens through `FileManager`, and
asserts `CorruptPageException` rather than an ASan report. **The suite fails against
today's code** — under ASan it does not merely fail, it reports the heap-buffer-overflow
directly. That is the evidence N1 is real.

---

### P2 — Storage services (L)

→ Resources and checklist: [`phases/P2.md`](phases/P2.md)

The missing layer. `Page` gives you 4096 bytes; a query engine needs tables, rows by
RID, iteration, and a catalog. Nothing above this can start until it exists.

**Order within the phase:** `StorageManager` → `PageGuard` → `TableHeap` → `Catalog`
→ WAL contract. Each depends on the previous.

#### StorageManager

The [N6](#n6--every-vectorheap-owns-a-private-filemanager-and-a-private-buffer-pool)
fix. One pool, many files.

```cpp
// include/storage/storage_manager.h
using FileId = uint32_t;
inline constexpr FileId INVALID_FILE_ID = 0;   // reserved, mirrors INVALID_HEAP_ID

/// Identifies a page within the whole database, not within one file.
struct PageRef {
    FileId file_id = INVALID_FILE_ID;
    PageId page_no = INVALID_PAGE_ID;

    /// Packed key for the page table. 32 bits of file id, 32 of page number:
    /// keeps unordered_map<uint64_t, size_t>'s shape and hashing cost rather
    /// than paying for a custom hash over a struct.
    [[nodiscard]] constexpr uint64_t Key() const noexcept {
        return (static_cast<uint64_t>(file_id) << 32) |
                static_cast<uint32_t>(page_no);
    }
    friend bool operator==(const PageRef&, const PageRef&) noexcept = default;
};

class StorageManager {
public:
    /// @param pool_size Frames shared across every registered file.
    explicit StorageManager(std::size_t pool_size = DEFAULT_POOL_SIZE);

    /// Register a file, creating it if absent.
    /// @returns Its FileId, stable for this process's lifetime.
    [[nodiscard]] FileId OpenFile(const std::string& path);
    void                 CloseFile(FileId id);

    [[nodiscard]] ReadPageGuard  FetchRead (PageRef ref);
    [[nodiscard]] WritePageGuard FetchWrite(PageRef ref);
    [[nodiscard]] WritePageGuard NewPage   (FileId file, PageRef& out);

    void FlushFile(FileId id);   ///< write dirty pages of one file, then Sync it
    void FlushAll();             ///< every file, one Sync per file

    [[nodiscard]] PageId PageCount(FileId id) const;
    [[nodiscard]] std::size_t PoolCapacity() const noexcept;
};
```

**Why `FetchRead`/`FetchWrite` rather than one `FetchPage`.** The dirty decision
moves from a bool argument the caller passes at unpin time — which
[N3](#n3--unpinpage-silently-discards-the-dirty-flag-on-an-unbalanced-unpin) shows can
be dropped — into the type at fetch time. A `ReadPageGuard` has no non-const accessor,
so a read path *cannot* dirty a page, and a write path cannot forget to.

#### PageGuard

The [N4](#n4--pin-leaks-on-the-exception-paths-and-no-raii-to-prevent-them) and
[N5](#n5--gettuple-hands-out-a-span-into-a-frame-that-can-be-evicted-underneath-it)
fix.

```cpp
// include/storage/page_guard.h

/// RAII pin. Unpins in the destructor, so no exception path can leak a frame.
///
/// LIFETIME CONTRACT: any span obtained from the page (Page::GetTuple, RawData)
/// is valid only while this guard is alive. The frame may be evicted and reused
/// the instant it is destroyed.
class ReadPageGuard {
public:
    ReadPageGuard(ReadPageGuard&&) noexcept;
    ReadPageGuard& operator=(ReadPageGuard&&) noexcept;
    ReadPageGuard(const ReadPageGuard&)            = delete;
    ReadPageGuard& operator=(const ReadPageGuard&) = delete;
    ~ReadPageGuard();                       ///< unpins if still held

    [[nodiscard]] const Page& operator*()  const noexcept;
    [[nodiscard]] const Page* operator->() const noexcept;
    void Drop() noexcept;                   ///< unpin early, explicitly
};

/// As above, plus mutable access. Marks the page dirty on first mutable access,
/// so a caller cannot modify a page and fail to record it.
class WritePageGuard { /* ... same shape, plus Page& operator* ... */ };
```

Move-only, never copyable: a copied guard means two destructors and a double unpin.

#### TableHeap

Everything in P4 and every index in P6 addresses rows through this.

```cpp
// include/storage/table_heap.h
class TableHeap {
public:
    TableHeap(StorageManager& sm, FileId file, const Schema& schema);

    /// @returns The RID of the stored row. Visible immediately; durable after Flush.
    /// @throws  ValueTooLargeException if the serialised row exceeds one page.
    [[nodiscard]] RID Insert(std::span<const std::byte> tuple);

    /// @returns Bytes valid only while `guard` is alive -- see PageGuard's contract.
    [[nodiscard]] std::span<const std::byte> Get(RID rid, ReadPageGuard& guard) const;

    /// In place if the new row fits its slot; otherwise delete + insert, which
    /// CHANGES THE RID. Callers holding the old RID must re-read.
    RID  Update(RID rid, std::span<const std::byte> tuple);
    void Delete(RID rid);

    class Iterator { /* forward, skips tombstones */ };
    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end()   const;
};
```

**`Update` changing the RID is the interesting decision.** The alternatives are a
forwarding tombstone (PostgreSQL's approach — the old slot points at the new
location, so the RID stays stable at the cost of a chain to follow and to clean up)
or refusing to grow a row at all. Returning the new RID is chosen because it makes
the cost visible in the signature: a caller that ignores the return value has a
compile-time `[[nodiscard]]`-shaped reminder that its RID may be stale. P6's index
maintenance is the caller that most needs to know.

**B7 (page compaction) is implemented here**, because `TableHeap` is what makes
reclaimed space reusable. `DeletedSpaceIsNotReclaimed_B7` pins the current
behaviour; changing it is a deliberate, visible test change, exactly as intended.

**Free-space map:** a page in the table's own file, owned by `TableHeap`, mapping
page → largest insertable payload. Consumes `Page::FreeSpace()`, whose semantics
[N14](#n14--minor) says must be documented first.

#### Catalog

```cpp
// include/catalog/catalog.h
using TableId = uint32_t;

class Catalog {
public:
    explicit Catalog(StorageManager& sm);

    [[nodiscard]] TableId CreateTable(const std::string& name, const Schema& schema);
    void                  DropTable(const std::string& name);

    [[nodiscard]] const Schema& GetSchema(TableId id) const;
    [[nodiscard]] TableId       GetTableId(const std::string& name) const;
    [[nodiscard]] TableHeap&    GetTableHeap(TableId id);

    /// Resolve a VECTOR column to its heap, opening it if needed.
    [[nodiscard]] VectorHeap& GetVectorHeap(TableId table, std::size_t column);

    /// Allocate the next heap id. MONOTONIC AND PERSISTED: never reuses an id,
    /// even after a column is dropped. See below.
    [[nodiscard]] HeapId AllocateHeapId();
};
```

**The never-reuse rule, and why it is enforced this way.** `audit.md` B5 names it as
the catalog's one hard obligation: a reused `HeapId` makes a stale `VectorRef`
validate against a heap that knows nothing about it, resolving to whatever vector
sits at that index. Silent wrong data, and the exact failure `heap_id` exists to
prevent.

The tempting implementation — derive the next id from the highest live column's id —
is wrong: dropping the highest column lowers the maximum, and the next allocation
reissues it. So the counter is **persisted in the catalog and only ever incremented**,
independent of what is live.

**Bootstrap:** the catalog table's own `Schema` is hardcoded in the binary at a fixed
`FileId`/`PageId`, because you cannot read the catalog to learn how to read the
catalog.

#### The WAL contract, not the implementation

This is the boundary fix. Land the *contract* now so P5 changes no call sites:

```cpp
// include/storage/log_manager.h
class ILogManager {
public:
    virtual ~ILogManager() = default;
    /// Ensure every log record up to `lsn` is durable. Called by BufferPool
    /// before writing any page whose page_lsn exceeds the durable tail.
    virtual void FlushUpTo(uint64_t lsn) = 0;
    [[nodiscard]] virtual uint64_t DurableLsn() const = 0;
};

/// The P2 default. Every page has page_lsn == 0, so FlushUpTo is never
/// meaningfully called and this costs one virtual dispatch per page write.
class NullLogManager final : public ILogManager { /* no-ops */ };
```

`BufferPool` takes an `ILogManager&`, defaulted to a `NullLogManager`, and consults
it before every page write. In P5, a real `LogManager` is constructed instead. **No
call site changes.**

Also landed here: the `page_lsn` and `checksum` fields (§4.1), and
[N9](#n9--vectorheap-data-pages-have-nowhere-to-put-a-page_lsn)'s resolution — vector
data pages get the standard header.

#### Tuple format versioning

`audit.md` and the planning conversation settled on lock-based concurrency with MVCC
as a later possibility, and the question was whether to reserve `begin_ts`/`end_ts`
in the tuple header now.

**Reserving them outright costs 16 bytes on every row, forever, for a feature that
is out of scope** — which nearly doubles a small row. The cheaper way to buy the same
insurance is a **format version byte** plus a documented extension point: adding
version fields later becomes a bounded change to `Serialize`, `Deserialize` and
`TableHeap`, gated on `ver`, rather than an undetectable reinterpretation of old
bytes.

And because nothing has shipped, a later format change costs a test-fixture rebuild,
not a migration. Same protection against being trapped; no per-row tax. The version
byte is §4.2's leading field.

#### Gate

`CREATE TABLE` through the catalog → 10 000 rows through `TableHeap` → close every
object → reopen from the same files → full scan returns all 10 000 in RID order,
under a pool one-tenth the page count so eviction is real. Plus: deleted rows'
space is reused by subsequent inserts (B7), and a dropped-then-recreated vector
column gets a *different* `HeapId`.

---

### P3 — SQL front end (M)

→ Resources and checklist: [`phases/P3.md`](phases/P3.md)

Text in, bound logical plan out. No execution.

**Hand-written recursive descent, not Flex/Bison.** Delete `src/parser/lexer.l` and
`src/parser/parser.y`; drop `flex bison` from the CI toolchain install.

| | Bison | Recursive descent + Pratt |
|---|---|---|
| Expression precedence | Grammar rules per level | ~150 lines of precedence climbing |
| Error messages | Generated, notoriously poor | Written by hand, positioned |
| Build | Codegen step on 3 platforms | None |
| Debugging | Through a state machine | Step through a function |
| Precedent | — | SQLite and DuckDB both hand-roll |

Error quality *is* the product for a REPL, and it is the dimension where generated
parsers are weakest. The cost is the "compiler theory" learning goal, which
precedence climbing partly repays.

**Three passes, kept separate:**

```cpp
// include/parser/parser.h
class Parser {
public:
    /// @throws SyntaxException carrying line, column, and the offending token.
    [[nodiscard]] static std::unique_ptr<Statement> Parse(std::string_view sql);
};

// include/parser/binder.h
/// Resolves names and types against the catalog. Separate from Parser on
/// purpose: P4 can build and test plans without going through SQL text at all.
class Binder {
public:
    explicit Binder(const Catalog& catalog);
    /// @throws UnknownTableException, UnknownColumnException,
    ///         TypeMismatchException -- each naming the offending identifier.
    [[nodiscard]] std::unique_ptr<BoundStatement> Bind(const Statement& stmt);
};

// include/optimizer/logical_plan.h
class Planner {
public:
    [[nodiscard]] static std::unique_ptr<LogicalPlanNode> Plan(const BoundStatement&);
};
```

**Scope: the full grammar P4 will execute.** Not a single-table subset.

```sql
CREATE TABLE t (id INT32 PRIMARY KEY, name VARCHAR(64), emb VECTOR(768) NULL);
DROP TABLE t;
INSERT INTO t (id, name) VALUES (1, 'a'), (2, 'b');
UPDATE t SET name = 'c' WHERE id = 1;
DELETE FROM t WHERE id > 5;
SELECT a.id, COUNT(*) FROM a JOIN b ON a.id = b.a_id
  WHERE a.x > 5 AND b.y = 'z'
  GROUP BY a.id HAVING COUNT(*) > 2
  ORDER BY a.id DESC LIMIT 10;
```

**Why the full grammar here rather than growing it in P4.** A phase boundary that
cuts through the parser means P4 reopens finished, tested code to add `JOIN`. Parsing
the full form is barely harder than the simple one, the parser test suite gets
written once, and P4 never touches the front end. This is the *boundaries between
components, not through them* principle, and P3 is where it pays best.

**Gate.** Every statement in the target grammar produces a bound plan a test asserts
on structurally. Unknown table, unknown column and type mismatch each produce a named
error carrying the offending identifier and its position. A malformed statement
produces a `SyntaxException` pointing at the right token — asserted on the position,
not just the type, because "it threw" is not the same as "it diagnosed".

---

### P4 — Execution engine (XL)

→ Resources and checklist: [`phases/P4.md`](phases/P4.md)

**Volcano iterator model.**

```cpp
// include/executor/executor.h
class AbstractExecutor {
public:
    virtual ~AbstractExecutor() = default;
    virtual void Init() = 0;
    /// @returns false when exhausted. `tuple` and `rid` are only written on true.
    [[nodiscard]] virtual bool Next(Tuple& tuple, RID& rid) = 0;
    [[nodiscard]] virtual const Schema& OutputSchema() const = 0;
};
```

Chosen over vectorized/push execution because vectorization is retrofitted for speed
once correctness exists, not a foundation — and the iterator model is what every
reference this project learns from uses, which matters when debugging.

**Operators:** SeqScan, Filter, Projection, Insert, Delete, Update, NestedLoopJoin,
HashJoin, HashAggregate, Sort, Limit.

**The logical/physical split, defined here even though P8 uses it.** If P4 hardcodes
"a join is a hash join", P8 becomes a rewrite rather than an addition. So
`LogicalJoin` maps to a `PhysicalJoin` interface with `NestedLoopJoin` and
`HashJoin` implementations from day one; P4 picks with a fixed rule, P8 picks with a
cost model, and nothing else changes.

**Rewrites only, no cost model:** predicate pushdown, projection pushdown, constant
folding. Each is correct by construction and testable by asserting on the rewritten
plan tree.

**De-risking — this is one of two phases where the project is most likely to stall.**
Get SeqScan + Filter + Projection running end-to-end against real files *before*
touching joins. That is a working database. Everything after it is addition rather
than integration, and the difference matters when momentum runs out.

**Gate.** A multi-table query with a join, a filter and a `GROUP BY`, executed against
real files, compared against a result the test computes independently in plain C++.
Comparing against a hand-written expected literal would pass a query that is
consistently wrong.

---

### P5 — WAL + recovery + latching (L)

→ Resources and checklist: [`phases/P5.md`](phases/P5.md)

**Moved up from Phase 7.** P5 is the last point where the set of things that write
pages is small — `TableHeap`, `Catalog`, `VectorHeap`. After B+Tree and HNSW exist,
all of them need redo records, and retrofitting recovery into a finished index is
strictly more work than writing the index against a logging API that already exists.
[N9](#n9--vectorheap-data-pages-have-nowhere-to-put-a-page_lsn) is the mechanical
proof.

*Why not immediately after P2?* Recovery correctness is proven by a crash test, and a
crash test needs a workload; raw `TableHeap` calls are a weaker one than SQL. And
`TransactionManager`'s shape is determined by how the executor threads a transaction
through operators — designing it before the executor exists means guessing. P2 landing
the *contract* and P5 the *implementation* gets both, with zero call-site churn.

```cpp
// include/storage/wal.h
enum class LogRecordType : uint8_t {
    kBegin, kCommit, kAbort, kInsert, kUpdate, kDelete, kNewPage, kCheckpoint,
};

struct LogRecordHeader {
    uint64_t      lsn;
    uint64_t      prev_lsn;      ///< previous record of the same txn -- undo chain
    uint32_t      txn_id;
    LogRecordType type;
    uint8_t       _pad[3];
    uint32_t      size;          ///< total record bytes, header included
};

class LogManager final : public ILogManager {
public:
    LogManager(StorageManager& sm, const std::string& log_path);
    [[nodiscard]] uint64_t Append(const LogRecordHeader& hdr,
                                  std::span<const std::byte> payload);
    void     FlushUpTo(uint64_t lsn) override;
    uint64_t DurableLsn() const override;
};

class RecoveryManager {
public:
    RecoveryManager(StorageManager& sm, LogManager& lm, Catalog& cat);
    void Recover();   ///< analysis -> redo -> undo. Idempotent.
};
```

**The WAL protocol lives inside `BufferPool`.** Before writing any page, the pool
calls `log_manager_.FlushUpTo(page.PageLsn())`. It is not a rule callers follow — a
rule callers can forget is a rule that will be forgotten, which is the same lesson
A4 taught when `Sync()` had zero call sites.

**Steal / no-force**, because the pool already steals (it evicts dirty pages). That
implies both redo and undo, hence ARIES-lite: analysis rebuilds the dirty-page and
active-transaction tables, redo replays forward from the earliest dirty-page LSN,
undo rolls back losers via `prev_lsn` chains.

**Two-phase locking**, table- then row-granularity. MVCC is a non-goal (§6.3):
attempting MVCC and crash recovery in one phase is where hobby databases stall.

**B8's page latch lands here, and it is not optional.** Without it, P9's concurrent
clients sit on a buffer pool whose own header documents pages as unsafe to share.
P5 is the natural home anyway — 2PL needs latches, and `buffer_pool.h`'s deferral
note already points at "Phase 7 alongside transactions".

**Gate — a real crash test.** A child process opens the database, commits some
transactions, leaves others open, and is killed with no cleanup. The parent reopens,
runs recovery, and asserts committed rows present and uncommitted rows absent.

Unlike the fsync-reaches-the-platter gap `audit.md` correctly calls untestable, this
one is genuinely testable: killing a process proves the *recovery algorithm* is
correct, which is the claim being made. A4's untestable claim was about hardware.

---

### P6 — Relational indexing (L)

→ Resources and checklist: [`phases/P6.md`](phases/P6.md)

```cpp
// include/index/index.h
class Index {
public:
    virtual ~Index() = default;
    virtual void Insert(const Value& key, RID rid) = 0;
    virtual void Delete(const Value& key, RID rid) = 0;
    [[nodiscard]] virtual std::vector<RID> Lookup(const Value& key) const = 0;
};

class BPlusTreeIndex final : public Index {
public:
    BPlusTreeIndex(StorageManager& sm, FileId file, ColumnType key_type);
    [[nodiscard]] std::vector<RID> Range(const Value& lo, const Value& hi) const;
    class Iterator { /* ordered forward traversal */ };
};
```

Disk-backed, one node per page through `StorageManager`. Split and merge, range
scan, iterator. Hash index for equality-only. IndexScan operator. Index maintenance
on every DML path — including `TableHeap::Update`'s RID change, which is the case
most likely to be missed. `CREATE INDEX` / `DROP INDEX`.

**WAL-aware and latch-aware from the first line,** because both exist by now. That is
the entire point of the reordering.

**One deferral:** latch crabbing. Concurrent B+Tree traversal takes the tree latch
rather than hand-over-hand. The contract is written down, not implied.

**Why indexing sits after execution, when CMU's order is the reverse.** The
counter-argument is real: IndexScan and index selection both depend on the B+Tree.
It loses because a B+Tree with no query path is testable only by unit test, whereas a
seq-scan executor is a *demonstrable database* — and because IndexScan is one more
operator inside an established framework, not churn, given P4 defined the operator
interface properly.

---

### P7 — Vector indexing / ANN (XL)

→ Resources and checklist: [`phases/P7.md`](phases/P7.md)

**Build brute-force KNN first, and it is not a throwaway.**

A linear scan over `VectorHeap` computing exact distances is a day's work. It makes
the vector path end-to-end queryable immediately — and, critically, **it is the
ground truth.** "The HNSW index works" is a meaningless claim without a recall
number, and recall is *defined* as agreement with the exact nearest neighbours.
Brute force is the oracle every later HNSW test needs.

```cpp
// include/vector/distance.h
enum class DistanceMetric : uint8_t { kL2, kCosine, kInnerProduct };
[[nodiscard]] float Distance(std::span<const float> a, std::span<const float> b,
                             DistanceMetric metric) noexcept;

// include/vector/vector_index.h
struct Neighbor { VectorId id; float distance; };

class VectorIndex {
public:
    virtual ~VectorIndex() = default;
    virtual void Add(VectorId id, std::span<const float> vec) = 0;
    [[nodiscard]] virtual std::vector<Neighbor> Search(std::span<const float> query,
                                                       std::size_t k) const = 0;
};

class BruteForceIndex final : public VectorIndex { /* the oracle */ };
class HnswIndex        final : public VectorIndex {
public:
    HnswIndex(StorageManager& sm, FileId graph_file, VectorHeap& heap,
              DistanceMetric metric, std::size_t M, std::size_t ef_construction);
};
```

Eigen is already a dependency, so SIMD distance kernels come free.

**Graph persistence: a second fixed-stride heap, not rebuild-on-load.** An HNSW
node's neighbour list is `M` entries per layer — a **fixed-stride record**, which is
exactly the addressing `VectorHeap` already implements and has 43 tests for. Reuse it.

Rebuild-on-load is the tempting shortcut and it is wrong at the scale this database
targets: a million 1536-dim vectors is minutes of startup, slow enough that nobody
runs the restart gate test, which is precisely the test that catches persistence
bugs. Rebuild-on-load stays as an early milestone, not the destination.

Deletes are tombstones — the same accepted debt as B7 and `VectorHeap` itself, and
consistent rather than a new pattern.

**Also:** KNN operator, hybrid `filter + vector` execution, recall/latency benchmarks
under `benchmarks/vector_bench` against a fixed ground-truth set. IVF optional.

**Gate.** Recall@10 ≥ 0.95 against `BruteForceIndex` on a fixed 10 000-vector,
768-dim dataset, and the index survives a full close-and-reopen with the same recall.

---

### P8 — Optimizer + statistics (M)

→ Resources and checklist: [`phases/P8.md`](phases/P8.md)

Split out of the old Phase 3 because **a cost model with no statistics and no indexes
to choose between is a random number generator.** By P8 both exist.

```cpp
// include/catalog/statistics.h
struct ColumnStats {
    uint64_t distinct_count;
    uint64_t null_count;
    Value    min, max;
    std::vector<uint64_t> histogram;   ///< equi-depth buckets
};

// include/optimizer/cost_model.h
struct Cost { double io; double cpu; [[nodiscard]] double Total() const; };
class CostModel {
public:
    [[nodiscard]] Cost Estimate(const PhysicalPlanNode&, const Statistics&) const;
};
```

Cost model, index selection, join ordering — DP for ≤ 8 relations, greedy beyond,
because DP is exponential and 8 is where it stops being free.

**Gate.** A query whose *physical* plan demonstrably changes when an index is added —
asserted on the plan tree, not on timing. Expressible only because P4 defined the
logical/physical split.

---

### P9 — Client / server / CLI (M)

→ Resources and checklist: [`phases/P9.md`](phases/P9.md)

Length-prefixed framing over asio (already a dependency). Session management, thread
pool, CLI REPL replacing the current echo loop, `table` / `json` / `csv` output.

**Explicitly not the PostgreSQL wire protocol.** That is a compatibility project —
weeks of message-format work with no database learning in it — unless `psql`
compatibility is a goal worth naming. If it is, it belongs in its own phase.

Genuinely concurrent, because P5 delivered the page latch. Without it this phase
would have had to serialise every query behind one lock.

---

### P10 — Embeddings (M, optional)

→ Resources and checklist: [`phases/P10.md`](phases/P10.md)

Moved last and made optional. It is the only phase adding no database machinery, and
it pulls ONNX Runtime — ~200 MB of platform-specific binaries with no clean
FetchContent story — for what is a thin client of P7.

```cpp
// include/vector/embedding.h
class IEmbeddingProvider {
public:
    virtual ~IEmbeddingProvider() = default;
    [[nodiscard]] virtual std::vector<float> Embed(std::string_view text) = 0;
    [[nodiscard]] virtual std::vector<std::vector<float>>
                          EmbedBatch(std::span<const std::string> texts) = 0;
    [[nodiscard]] virtual uint32_t Dimension() const = 0;
};

/// The default. The client computes embeddings; the database stores and searches
/// them. Everything P10 demonstrates works through this.
class ExternalEmbeddingProvider final : public IEmbeddingProvider { /* ... */ };

/// Behind ENABLE_ONNX=OFF.
class OnnxEmbeddingProvider final : public IEmbeddingProvider { /* ... */ };
```

The interface is declared in **P7**, so the vector path is complete without this
phase ever running. Auto-embed on INSERT, batch embed for bulk ingestion,
text → vector → ranking. The build stays clonable either way.

---

## 6. Cross-cutting

### 6.1 CI

Seven surfaces, all of which must be green for any phase to close: `build-linux`
(ubuntu-22.04 and 24.04 × gcc and clang), `build-windows`, `sanitizers`, `coverage`,
`code-quality`, `docker`.

`code-quality` is advisory (`continue-on-error`) by design — the tree predates the
gates. **Make it blocking at the end of P2**, once `scripts/format_code.sh` has
normalised the tree and the clang-tidy findings are worked through. Leaving it
advisory forever is how C8 (`IndentWidth: 2` against a 4-space codebase) survived
unnoticed.

Also open from `audit.md`: mark FetchContent include directories `SYSTEM` to silence
spdlog's bundled `fmt` warnings without `_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS`,
which would silence our own translation units too.

### 6.2 Naming — pick one

`Axolotl` (repo) · `AxolotlDB` (namespace, CMake target) · `AxolotlDB` (CMake project,
docs) · `AxolotException` (exception base). Four spellings of one name. Nobody but
you can make this call; make it before P9 puts a name in a wire protocol and a
CLI banner.

### 6.3 Non-goals

Named so they stop being re-litigated:

- **MVCC.** Two-phase locking is the isolation mechanism. The tuple format's version
  byte keeps the door open; nothing else is spent on it.
- **Cross-architecture file portability.** Native endianness throughout. Revisiting
  means byte-swapping in exactly the places §4 lists.
- **PostgreSQL wire compatibility.** See P9.
- **Compression, columnar storage, distributed anything.**

### 6.4 Accepted debt, with trigger conditions

| Item | Trigger to fix |
|---|---|
| Latch crabbing in B+Tree (P6) | First measured contention on the tree latch |
| Vector heap deletion / compaction | First workload where dropped vectors are a material fraction |
| `NewPage` leaks a disk page if write-back throws (A3 residual) | When P2's free list exists |
| POSIX directory-entry fsync on file creation (A4 residual) | If a crash ever loses a whole file rather than its contents |
| `install(EXPORT)` incomplete (C14) | First external consumer of the library |
| I/O failure injection untestable — `FileManager` has no virtual methods | When `StorageManager` makes the seam natural (P2) |

### 6.5 A note on the working tree

At the time of writing, `git status` shows four modified files with a −122/+16 diff
that is almost entirely **deletion of explanatory comments** — the `VectorRef`
provenance rationale, the `HeapId`-reuse warning, the A1 arithmetic note, the A8
overflow note — plus several statements collapsed past the 100-column
`.clang-format` limit, which `code-quality` will flag once it is blocking.

No behaviour changed. But those comments were the only in-code record of *why* the
formats are shaped the way they are, and the `HeapId`-reuse warning in particular
documented an invariant the storage layer cannot enforce for itself.

That reasoning is now preserved here — §4 for the formats, §5's P2 for the heap-id
rule — so it survives independently of the comments. `git checkout -- .` restores
them if the stripping was not deliberate.

---

## 7. Amendment log

This plan will be wrong somewhere. Recording where beats silently rewriting it —
the same discipline `audit.md`'s *"First fix (superseded)"* entries follow, which
are among the most useful passages in that document.

### 2026-08-25 — initial

Five decisions changed during three review rounds, before anything was written:

| # | Was | Now | Why |
|---|---|---|---|
| 1 | `PageGuard` in P0 | `PageGuard` in P2; P0 fixes only the live leak | P0 is "make Phase 1 correct"; P2 rewrites every call site anyway, so P0 would touch them twice |
| 2 | P3 scoped to single-table `SELECT` | P3 parses the full grammar P4 executes | A boundary through the parser means P4 reopens finished code to add `JOIN` |
| 3 | B8's page latch deferred indefinitely | Lands in P5 | P9 introduces concurrent clients onto a pool documenting pages as unsafe to share — a hole in the plan |
| 4 | HNSW rebuild-on-load | Persisted fixed-stride graph; rebuild is an early milestone | Minutes of startup at scale means nobody runs the restart gate test, which is the test that catches persistence bugs |
| 5 | Reserve `begin_ts`/`end_ts` in every tuple | Format version byte + documented extension point | 16 bytes on every row forever for an out-of-scope feature; nothing has shipped, so a later change costs a fixture rebuild, not a migration |

One correction to the audit itself: [N11](#n11--the-design-documents-are-gitignored)
was initially filed as a bug. The `.gitignore` comment shows it was deliberate. It
is recorded as a decision with its consequence stated, and narrowed rather than
reversed.

### 2026-08-25 — after implementing P0

Three corrections, all found by writing the code rather than reading it:

| # | Was | Now | Why |
|---|---|---|---|
| 6 | N1's validation goes in `FileManager::ReadPage` | It goes in the slotted-page accessors; `ValidateOrThrow()` is an explicit opt-in pass | `ReadPage` cannot tell a slotted page from a zero-filled fresh page or a `VectorHeap` data page, and would reject both. Needs the page-type tag N9 adds in P2. |
| 7 | N4: "a throw on iteration *i* leaks *i* pins" in both `VectorHeap` loops | One pin, in `Get` only, on allocation failure | Each iteration unpins before the next begins. `Put` holds a pin only across non-throwing code. Fixed by deleting the temporary that was the throw point, not by guarding it. |
| 8 | N2 is a missing `#include <format>` | The committed line was `"literal" + PAGE_SIZE` — pointer arithmetic, UB everywhere | Worse than what it replaced: UB on all seven surfaces instead of a compile error on six. |

Correction 7 is the one worth dwelling on. The finding was written from reading
the shape of the code — a fetch and an unpin with something between them — and
the shape was right while the count was wrong. Writing the test forced the
question "leaks *what*, exactly", and the honest answer was smaller and more
specific than the claim. **The severity did not change** (one permanently
unevictable frame is still a bug worth fixing) but the description did, and a
finding that overstates its own blast radius is harder to trust the next time.

P0's gate was verified by mutation, not just by passing: neutering the two
validators makes exactly the 8 corruption tests fail while the healthy-page
controls keep passing. A suite that passes both before and after a fix proves
nothing, which is the lesson C2 taught this project the expensive way.

**Confirmed under AddressSanitizer** (MSVC `/fsanitize=address`, `ENABLE_EIGEN=OFF`
to skip an unrelated dependency). All 162 tests pass clean with the validation in
place. With the two validators neutered, `SlotCountBeyondPageCapacityThrows` and
`FreePtrBeyondPageSizeThrows` do not merely fail — both die with
`SEH exception 0xc0000005`, an access violation. The accesses are far enough out
of bounds (~236 KB read, ~61 KB write) to leave the process's mapped memory
entirely, so they fault before ASan's redzones are even reached. N1 was not a
theoretical severity.

---

The principle that produced amendments 1–3, worth keeping:

> **Phase boundaries should fall between components, not through them.**

### 2026-08-25 — phase guides, and two corrections

[`docs/phases/`](phases/README.md) added: one guide per phase, pairing a verified resource
list with a commit-sized implementation checklist. Companion to this document, not a
replacement — see the note under [§1.1](#11-the-one-page-table).

Verifying those resources surfaced two factual errors in this document. Recorded rather than
silently edited, per this section's purpose.

| # | Where | Wrong | Correct |
|---|---|---|---|
| 6 | [P3](#p3--sql-front-end-m), the **Precedent** row | *"SQLite and DuckDB both hand-roll"* | Neither does. **SQLite** uses [Lemon](https://sqlite.org/lemon.html), its own LALR(1) *generator*, written because YACC was non-reentrant — an argument for a better generator, not for hand-rolling. **DuckDB** used a Bison grammar forked from PostgreSQL (`third_party/libpg_query`) through v1.x and replaced it in [v2.0](https://duckdb.org/2026/08/20/duckdb-20-peg-parser) with a **PEG** parser — generator to generator. The real hand-rolled precedents are rustc, Go's `go/parser`, Roslyn and Clang, all citing error-message quality |
| 7 | [N11](#n11--the-design-documents-are-gitignored), filed as *Applied* | `!docs/*.md` | That pattern is one directory deep and leaves `docs/phases/*.md` ignored. Widened to `!docs/**/*.md`. N11 was resolved only for files directly in `docs/` |

**Correction 6 does not change the P3 decision.** The other four rows of that table —
expression precedence, error messages, no codegen step, debuggability — are untouched, and
DuckDB's stated reason for leaving Bison (LALR(1) grammars accrete shift/reduce conflicts as
the grammar grows) is a *stronger* argument against Bison here than the one that row made.
Only the row's evidence was wrong, not its conclusion.

Which is worth naming as a rule, since the correction cost nothing and the decision stood:

> **A decision can be right and its stated reason wrong.** Check the evidence separately
> from the conclusion — the conclusion surviving is not proof the evidence did.
