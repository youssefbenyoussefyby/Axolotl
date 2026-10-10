# AxolotlDB — phase guides

Companion to [`../ROADMAP.md`](../ROADMAP.md). The roadmap says **what** each phase
delivers and **why** the boundaries fall where they do. These files say **what to read
before you write it** and **what to do, in order, in commit-sized pieces**.

The roadmap stays the source of truth for signatures, on-disk formats and gate tests.
These files link back to it rather than restating it — where they disagree, the roadmap
wins, and the disagreement is a bug in this directory.

---

## Index

| | Phase | State | Guide |
|---|---|---|---|
| **P0** | Hardening — N1–N4, N10–N14 | **NEXT** | [P0.md](P0.md) |
| **P1** | Storage core | DONE, modulo P0 | [P1.md](P1.md) — retrospective |
| **P2** | Storage services | Not started | [P2.md](P2.md) |
| **P3** | SQL front end | Not started | [P3.md](P3.md) |
| **P4** | Execution engine | Not started | [P4.md](P4.md) |
| **P5** | WAL + recovery + latching | Not started | [P5.md](P5.md) |
| **P6** | Relational indexing | Not started | [P6.md](P6.md) |
| **P7** | Vector indexing / ANN | Not started | [P7.md](P7.md) |
| **P8** | Optimizer + statistics | Not started | [P8.md](P8.md) |
| **P9** | Client / server / CLI | Not started | [P9.md](P9.md) |
| **P10** | Embeddings | Not started, optional | [P10.md](P10.md) |

---

## How to use a phase file

Each file has the same shape:

1. **Before you start — the minimum.** One to three things. Read these; everything else
   in Resources is lookup you reach for when stuck. The distinction matters: a phase that
   starts with "read the whole book" is a phase that does not start.
2. **Resources.** Split into specs, books (with chapter numbers), video (with lecture
   numbers), code to read, and — importantly — **Skip**, naming the well-known resource
   that looks relevant and is not.
3. **Design decisions already made.** Pointers into the roadmap for choices you should not
   re-open in the middle of the phase.
4. **Implementation checklist.** One item ≈ one commit. Every item names a file and at
   least one test. Tests come before implementation within each component, per the
   roadmap's division of labour.
5. **Gate** and **Done when.**

**Checking items off.** These are tracked markdown files; tick the boxes as you go and
commit that alongside the work. A half-ticked phase file is the most accurate status
report the project has.

---

## The core reading spine

Five resources cover most of P2–P8. Everything else in the per-phase files is a
supplement to these.

| | What it is | Where it earns its keep |
|---|---|---|
| **CMU 15-445/645** — [course](https://15445.courses.cs.cmu.edu/) · [Fall 2025 schedule](https://15445.courses.cs.cmu.edu/fall2025/schedule.html) · [Fall 2025 playlist](https://www.youtube.com/playlist?list=PLSE8ODhjZXjYMAgsGH-GtY5rJYZ6zjsd5) | Andy Pavlo's undergrad DBMS course, 25 lectures, slides + notes + video, free | Every phase. The single highest-value resource for this project |
| **[BusTub](https://github.com/cmu-db/bustub)** | The C++ teaching DBMS 15-445 is built around: buffer pool, B+Tree, execution, concurrency | P2, P4, P5, P6 — read it for *shape*, not to copy |
| **Petrov, *Database Internals*** (O'Reilly, 2019) | Part I is storage engines: page layout, B-Trees, transaction processing and recovery | P2, P5, P6 |
| **Sciore, *Database Design and Implementation*, 2e** (Springer, 2020) | Chapters 3–11 build one engine component per chapter, bottom-up (SimpleDB) | P2, P3, P4, P5 — closest match to this project's *sequence* |
| **Hellerstein, Stonebraker & Hamilton, [*Architecture of a Database System*](https://dsf.berkeley.edu/papers/fntdb07-architecture.pdf)** (FnTDB, 2007) | ~120 pages, the whole system in one document, free | Read once before P2. It is the map the roadmap's §3 diagram is drawn from |

Free and worth having open: **[Readings in Database Systems, 5e](http://www.redbook.io/)**
(the "Red Book", [full PDF](http://www.redbook.io/pdf/redbook-5th-edition.pdf)) —
curated papers with editorial introductions.

**A note on the CMU video.** Lecture numbering shifts every semester. These files cite the
**Fall 2025** playlist and schedule because that edition is complete and its numbering
matches the schedule table below. The current course site is now Fall 2026 and renumbers;
if you follow that one instead, match by topic, not by number.

---

## Lecture → phase map

CMU 15-445/645, [Fall 2025 numbering](https://15445.courses.cs.cmu.edu/fall2025/schedule.html).

| Phase | Lectures | Topics |
|---|---|---|
| P2 | 3, 4, 5 | Database Storage I, Memory Management, Database Storage II |
| P3 | 1, 2 | Relational Model & Algebra, Modern SQL |
| P4 | 11, 12, 13, 14 | Sorting & Aggregations, Join Algorithms, Query Execution I & II |
| P5 | 17, 18, 21, 22 | Concurrency Control Theory, Two-Phase Locking, Logging, Recovery |
| P6 | 7, 8, 9, 10 | Hash Tables, Indexes & Filters I & II, Index Concurrency Control |
| P7 | 9 | Vector-index section only — the HNSW paper does the real work |
| P8 | 15, 16 | Query Planning & Optimization I & II |
| P9 | — | No lecture maps; this is systems programming, not database theory |
| — | **19, 20** | **Skip** — Timestamp Ordering, MVCC. [§6.3](../ROADMAP.md#63-non-goals) makes MVCC a non-goal |
| — | **23, 24** | **Skip** — Distributed Databases I & II. Out of scope, same section |
| — | 6 | Optional — Storage Models & Compression. Column stores are a §6.3 non-goal, but the n-ary/decomposition framing is useful background |

---

## Book → phase map

| Phase | Petrov, *Database Internals* | Sciore, *Database Design & Implementation* 2e |
|---|---|---|
| P2 | ch 3 (file formats, slotted pages, cell layout) | ch 3 (disk/file mgmt), ch 4 (memory mgmt / buffers), ch 6 (record pages), ch 7 (metadata / catalog) |
| P3 | — | ch 9 (parsing) |
| P4 | — | ch 8 (query processing / scans), ch 10 (planning) |
| P5 | ch 5 (transaction processing and recovery) | ch 5 (transaction management, recovery, concurrency) |
| P6 | ch 2 (B-Tree basics), ch 4 (implementing B-Trees) | ch 12 (indexing) |
| P8 | — | ch 10 (planning), ch 15 (query optimization) |

Petrov's Part II is distributed systems — a §6.3 non-goal. Part I is the whole of what
this project needs from that book.

---

## Papers, once each

Cited from the phase that needs them; collected here so they are findable.

| Paper | Phase | Link |
|---|---|---|
| Hellerstein, Stonebraker, Hamilton — *Architecture of a Database System* (2007) | orientation, P2, P4 | [PDF](https://dsf.berkeley.edu/papers/fntdb07-architecture.pdf) |
| Graefe — *Volcano: An Extensible and Parallel Query Evaluation System* (TKDE 1994) | P4 | [PDF](https://cs-people.bu.edu/mathan/reading-groups/papers-classics/volcano.pdf) · [DOI](https://dl.acm.org/doi/10.1109/69.273032) |
| Graefe — *Query Evaluation Techniques for Large Databases* (1993) | P4, P8 | [PDF](https://cs.uwaterloo.ca/~david/cs848s13/graefe.pdf) |
| Mohan et al. — *ARIES* (TODS 17(1), 1992, pp. 94–162) | P5 | [PDF](https://cs.stanford.edu/people/chrismre/cs345/rl/aries.pdf) · [DOI](https://dl.acm.org/doi/10.1145/128765.128770) |
| Graefe — *A Survey of B-Tree Locking Techniques* (TODS 35(3), 2010) | P6 | [PDF](https://15721.courses.cs.cmu.edu/spring2016/papers/a16-graefe.pdf) · [DOI](https://dl.acm.org/doi/10.1145/1806907.1806908) |
| Graefe — *A Survey of B-Tree Logging and Recovery Techniques* (TODS 37(1), 2012) | P6 | [PDF](https://people.eecs.berkeley.edu/~kubitron/courses/cs262a-F12/handouts/papers/a1-graefe.pdf) · [DOI](https://dl.acm.org/doi/10.1145/2109196.2109197) |
| Graefe — *Modern B-Tree Techniques* (FnTDB 3(4), 2011) | P6 | [DOI](https://dl.acm.org/doi/10.1561/1900000028) — paywalled; the locking survey above is the free substitute |
| Malkov & Yashunin — *HNSW* (2016/2018) | P7 | [arXiv:1603.09320](https://arxiv.org/abs/1603.09320) |

---

## Conventions in these files

- `→ path` names the file the item touches. Paths are repo-relative.
- `→ test:` names the GoogleTest `Suite.Name` that proves the item. **An item without a
  test is not an item** — that is [ROADMAP §5](../ROADMAP.md#how-a-phase-runs)'s rule
  ("a phase is complete when a test proves the behaviour, not when the file exists"),
  applied at checklist granularity.
- **Division of labour**, per ROADMAP §5: Youssef writes all of `src/` and `include/`.
  Claude writes the tests and does the review passes. Test items therefore precede
  implementation items within every component.
- **Correction** boxes flag places where the roadmap states something factually wrong.
  They are filed here and offered to [§7's amendment log](../ROADMAP.md#7-amendment-log)
  rather than silently edited into §5, which is that section's whole discipline.
