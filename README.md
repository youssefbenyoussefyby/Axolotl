# AxolotlDB

> A from-scratch relational + vector database engine in C++20, built as an
> instrumented testbed for database research.

[![CI](https://github.com/youssefbenyoussefyby/Axolotl/actions/workflows/main.yml/badge.svg?branch=Axolot)](https://github.com/youssefbenyoussefyby/Axolotl/actions/workflows/main.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Cite](https://img.shields.io/badge/cite-CITATION.cff-informational.svg)](CITATION.cff)

---

## What is AxolotlDB?

AxolotlDB implements every layer of a database system itself: storage, SQL parsing,
planning, execution, indexing, recovery and (later) vector search, with no existing
database library underneath.

It has two goals:

1. **Learning, done properly.** Every phase has a written spec, a gate test and an
   audit trail. A phase is complete when a test proves the behaviour, not when the file exists.
2. **Research.** Owning every layer makes the engine a controllable testbed for three
   questions about AI-assisted databases (see [Research directions](#research-directions)).

---

## Status

The engine runs end to end: SQL text is lexed, parsed, bound, planned, converted to a
physical plan and executed over real files, under a buffer pool small enough to force eviction.

| Phase | Scope | Status |
|-------|-------|--------|
| **P0** | Hardening: page validation, green CI baseline | ✅ Done |
| **P1** | Storage core: Page, FileManager, BufferPool, Tuple, VectorHeap | ✅ Done |
| **P2** | Storage services: StorageManager, PageGuard, TableHeap, Catalog, WAL contract | ✅ Done |
| **P3** | SQL front end: lexer, parser (recursive descent + Pratt), binder, logical plan | ✅ Done |
| **P4** | Execution engine (Volcano iterator) | 🚧 In progress: framework, the **4.2 milestone** (SeqScan + Filter + Projection) and **4.3 DML** (INSERT, UPDATE, DELETE) done; joins (4.4), aggregation/sort/limit (4.5) and rewrite rules (4.6) next |
| **P5** | WAL + ARIES-style recovery + page latching + 2PL | ⬜ Planned |
| **P6** | Relational indexing: B+Tree, hash index, IndexScan | ⬜ Planned |
| **P7** | Vector indexing: exact KNN oracle, then persisted HNSW | ⬜ Planned |
| **P8** | Optimizer + statistics (Selinger-style baseline) | ⬜ Planned |
| **P9** | Client / server / CLI | ⬜ Planned |
| **P10** | Embeddings (optional) | ⬜ Planned |

Per-phase specs, reading lists and commit-sized checklists: [`docs/phases/`](docs/phases/README.md).
Architecture, on-disk formats and gate tests: [`docs/ROADMAP.md`](docs/ROADMAP.md).

### What works today

| Layer | Components |
|-------|------------|
| Storage | `Page` (slotted, validated against corrupt headers), `FileManager` (real `fsync`), `BufferPool` (LRU, pin/unpin), `StorageManager` (one pool shared across files), `PageGuard` (RAII pins), `TableHeap` (RID addressing, free-space map, compaction), `VectorHeap` (fixed-stride, large embeddings, commit/rollback) |
| Catalog | Tables, schemas and vector-heap ids stored in a table inside the database |
| Front end | Lexer, parser, binder (name/type resolution against the catalog; INSERT checks for NOT NULL and literal types), logical planner, with positioned error messages |
| Execution | Physical plan, executor factory, SeqScan, Filter, Projection, Insert, Update, Delete. DML is all-or-nothing against bad input (every row is checked before the first write), UPDATE materialises its RID set first (no Halloween problem), and values are type-checked and converted per column (no silent truncation, NULL rejected in NOT NULL columns) |
| Utilities | YAML config with env overrides, spdlog logger, tracking/guarded allocator |

### What does not exist yet

Be aware before you try it: `CREATE TABLE` / `DROP TABLE` through SQL (they parse and
bind, but have no executor yet; use `:seed` for a demo table), joins (P4.4), aggregation,
sorting and limit (P4.5), rewrite rules (P4.6), transactions and crash recovery (P5), any
index (P6/P7), the optimizer (P8) and the network server (P9). Vector values cannot be
written in SQL yet (no vector literal). The server binary is a stub.

---

## Building

### Prerequisites

- **C++20** compiler: GCC ≥ 12, Clang ≥ 16, or MSVC ≥ 19.30 (VS 2022)
- **CMake** ≥ 3.21
- **Ninja** (recommended)
- Internet access on first configure (`FetchContent` pulls the dependencies)

### Build and test

```bash
git clone https://github.com/youssefbenyoussefyby/Axolotl.git
cd Axolotl

# Linux / macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=ON

# Windows (MSVC auto-detected)
cmake -B build -DENABLE_TESTING=ON

cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
```

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTING` | `ON` | Build and register test targets |
| `ENABLE_COVERAGE` | `OFF` | `--coverage` flags (GCC/Clang) |
| `BUILD_SERVER` | `ON` | Build the server stub |
| `BUILD_CLI` | `ON` | Build the CLI |

---

## The CLI

The CLI embeds the engine and runs the full pipeline:
**lex → parse → bind → plan → physical plan → execute → render**.

```bash
# Linux / macOS
./build/AxolotlDB-cli --dev

# Windows
.\build\Release\AxolotlDB-cli.exe --dev
```

Run it from the repository root so it can find `cli/data/phases.json` and
`cli/data/queries.json` (or pass `--meta-dir`).

### Options

| Flag | Meaning |
|------|---------|
| `-c, --config <file>` | YAML config file |
| `--log-level <lvl>` | `trace\|debug\|info\|warn\|error\|critical\|off` |
| `-d, --data-dir <dir>` | Database directory (default `AxolotlDB_data`) |
| `--meta-dir <dir>` | Directory with `phases.json` and `queries.json` |
| `--dev` | Start directly in dev mode |
| `--no-anim` | Disable animations (automatic when output is piped) |

### Modes

| Mode | Purpose |
|------|---------|
| `user`, `admin` | Placeholders: input is echoed back. They exist so the console has a home for those modules later |
| **`dev`** | Everything built so far: SQL on the real engine, roadmap view, demo data, animations |

**Dev mode** gates SQL by `cli/data/queries.json`. A statement runs only if every
feature it uses is marked `available`; otherwise the CLI tells you which phase and task
will deliver it. This keeps the console honest while the engine grows.

### Commands

| Command | Description |
|---------|-------------|
| `:dev` (`:debug`), `:user`, `:admin` | Switch mode |
| `:queries` | SQL that works today, and what is planned and when |
| `:status` (`:roadmap`) | Progress bar per phase, generated from the checklists in `docs/phases/` |
| `:tasks [P4]` | Task checklist of a phase (default: the current stage) |
| `:seed` | Create the demo table `king_ya_bay` (id, name, age, aura) with 6 rows. Needed because `CREATE TABLE` has no executor yet |
| `:tables` | List tables |
| `:swim` | Watch the axolotl swim by |
| `:clear`, `:help`, `:exit` (`:quit`, `:q`) | Utilities |

SQL ends with `;` and may span several lines. `\c` or `:cancel` discards a half-typed statement.

### Example session

```
꒰• - •꒱> :dev
꒰◉ - ◉꒱> :seed
  ✔ created table `king_ya_bay` (id, name, age, aura) with 6 rows
꒰◉ - ◉꒱> SELECT id, name FROM king_ya_bay WHERE age >= 23;
꒰◉ - ◉꒱> INSERT INTO king_ya_bay VALUES (7, 'Zied', 24, 8.5);
꒰◉ - ◉꒱> UPDATE king_ya_bay SET age = age + 1 WHERE id = 7;
꒰◉ - ◉꒱> DELETE FROM king_ya_bay WHERE aura < 5.0;
```

Every column of the demo table is `NOT NULL`, so an `INSERT` must give all four values.

Syntax errors report line and column with a caret under the offending token.
Data lives in the data directory and survives restarts.

---

## Research directions

The engine is built so that research ideas can be implemented, instrumented and measured
against a classical baseline in the same codebase. All three directions are **planned work**;
no results are claimed yet. The draft research plan, with a starting related-work list,
lives in [`docs/research/RESEARCH.md`](docs/research/RESEARCH.md). Experiments are
pre-registered and reproducible under [`experiments/`](experiments/README.md); the first,
[E001](experiments/E001-semantic-filter-ordering/README.md) (ordering a semantic predicate
among cheap ones), is defined and needs only a `SemanticFilter` operator now that P4.3 is done.

| # | Direction | Core question | Baseline |
|---|-----------|---------------|----------|
| 1 | **Semantic filters for text columns** | Can predicates evaluated by an embedding or LLM model (e.g. "the review is about shipping delays") be placed in a query plan cost-aware, running cheap relational predicates first? | Cheap-predicates-first plan; model predicate evaluated on every row |
| 2 | **Model-assisted query optimization** | Can a decision model, given the plan tree and column statistics, pick better plans than a classical cost model, with the classical optimizer kept as fallback? | The Selinger-style optimizer built in P8 |
| 3 | **Forgetting / memory-like storage** | Can a text-heavy store compress old data semantically (summarise, merge, evict) while keeping query answers useful under a storage budget? | Retain everything; LRU-style eviction |

Future work: storing and querying **knowledge graphs** in the same engine.

### Why this engine suits the research

- **Instrumented at every layer.** Plans are inspectable trees, so a model can consume a plan and
  a test can assert on it.
- **Ground truth built in.** P7's `BruteForceIndex` defines recall for vector search; every
  rewrite and plan choice is checked by comparing row results against an unoptimised plan.
- **Reproducibility.** Fixed seeds, committed benchmark results (`benchmarks/`), and gate
  tests that assert on plan structure rather than wall-clock time.

---

## Engineering principles

- **Test-first.** Gate tests are written against the specified interfaces and must fail before the work.
- **Gates, not dates.** Phase sizes are relative; completion is a passing gate.
- **Trust nothing that came off disk.** Every on-disk structure gets a validator.
- **Audit and amend.** Findings and corrections are recorded in the
  [amendment log](docs/ROADMAP.md#7-amendment-log) instead of silently rewritten.
- **CI on seven surfaces:** Linux (GCC and Clang, Ubuntu 22.04 and 24.04), Windows/MSVC,
  ASan/UBSan, coverage, code quality, Docker.

---

## Project structure

```
.
├── include/            Public headers (storage/, catalog/, parser/, optimizer/, executor/, utils/)
├── src/                Implementation, mirroring include/
├── cli/                Interactive console (main.cpp, sql_runner, mini_json)
│   └── data/           phases.json (generated from docs/phases/), queries.json (SQL gate)
├── server/             Server binary (stub)
├── tests/              GoogleTest suites (unit/, integration/)
├── benchmarks/         Micro-benchmarks
├── experiments/        Pre-registered research experiments and committed results
├── tools/              gen_phases.py: docs/phases/ checklists → cli/data/phases.json
├── docs/
│   ├── ROADMAP.md      Architecture, on-disk formats, phase gates, amendment log
│   ├── audit.md        Storage-layer audit findings
│   ├── phases/         Per-phase reading lists and checklists
│   └── research/       Research plan
├── cmake/              CMake helpers
├── Dockerfile, docker-compose.yml
└── THIRD_PARTY_NOTICES.md
```

---

## Docker

```bash
docker build -t axolotldb:dev .
docker run --rm axolotldb:dev axolotldb-server   # server is currently a stub
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Architecture, formats, phase specs and gates |
| [`docs/phases/README.md`](docs/phases/README.md) | Phase guides: what to read and what to build, in order |
| [`docs/research/RESEARCH.md`](docs/research/RESEARCH.md) | Research plan: hypotheses, baselines, evaluation, related work |
| [`experiments/README.md`](experiments/README.md) | Experiment protocol and status table |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Build, test, formatting and commit conventions |
| [`SECURITY.md`](SECURITY.md) | How to report a vulnerability |
| [`docs/audit.md`](docs/audit.md) | Audit findings and how each was resolved |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Third-party licences |

---

## Contributing

Personal research and learning project; issues and PRs are welcome, with no support
commitment. See [`CONTRIBUTING.md`](CONTRIBUTING.md). In short:

1. Format with clang-format **18.1.8** (config in [`.clang-format`](.clang-format))
2. Make sure `ctest --test-dir build --output-on-failure` passes
3. Keep commits focused and describe the *why*

Participation follows the [Code of Conduct](CODE_OF_CONDUCT.md).

---

## Citing

If AxolotlDB is useful in your research, please cite it. GitHub's **"Cite this
repository"** button reads [`CITATION.cff`](CITATION.cff):

```bibtex
@software{benyoussef_axolotldb_2026,
  author  = {Ben Youssef, Youssef},
  title   = {{AxolotlDB}: a from-scratch relational and vector database engine as a testbed for database research},
  year    = {2026},
  version = {0.1.0},
  url     = {https://github.com/youssefbenyoussefyby/Axolotl}
}
```

---

## License

[GNU General Public License v3.0](LICENSE) © 2026 Youssef Ben Youssef