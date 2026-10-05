# AxolotlDB

> A from-scratch relational + vector database engine written in C++20.
> Built for learning, research, and eventually production-grade performance.

[![CI](https://github.com/youssefbenyoussefyby/Axolotl/actions/workflows/main.yml/badge.svg?branch=Axolot)](https://github.com/youssefbenyoussefyby/Axolotl/actions/workflows/main.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

---

## What is AxolotlDB?

AxolotlDB is an educational database engine built from first principles in C++20.
The goal is to implement every layer of a modern database system — storage, parsing,
query planning, execution, indexing, and transaction management — without relying on
existing database libraries.

The project is structured as an incremental series of phases, each building on the last.
Each phase has a detailed specification in [`docs/phases/`](docs/phases/README.md) and a
clear gate condition before the next phase begins.

---

## Current Status — v0.1.0

The **storage engine** (Phases P0–P2) is complete and well-tested.

| Phase | Scope | Status |
|-------|-------|--------|
| **P0** | Hardening — page validation, CI baseline, 0 warnings | ✅ Done |
| **P1** | Storage core — Page, FileManager, BufferPool, Tuple, VectorHeap | ✅ Done |
| **P2** | Storage services — StorageManager, PageGuard, TableHeap, Catalog, WAL contract | ✅ Done |
| **P3** | SQL front end — lexer, parser, binder, logical plan | 🔜 Next |
| **P4** | Execution engine — Volcano operators, rule-based rewrites | ⬜ Planned |
| **P5** | WAL + recovery + page latching | ⬜ Planned |
| **P6** | Relational indexing — B+Tree, hash index, IndexScan | ⬜ Planned |
| **P7** | Vector indexing / ANN — brute force → HNSW | ⬜ Planned |
| **P8** | Optimizer + statistics | ⬜ Planned |
| **P9** | Client / server / CLI | ⬜ Planned |
| **P10** | Embeddings — provider interface, optional ONNX | ⬜ Planned |

### What works today

The storage layer stores and retrieves rows — including rows with embeddings larger
than a single page — and survives a full close-and-reopen cycle. It is covered by
**144 tests** across 9 suites.

| Component | Location | Description |
|-----------|----------|-------------|
| `Page` | `src/storage/page.cpp` | Slotted page: insert / get / delete, tombstones, validated header |
| `FileManager` | `src/storage/file_manager.cpp` | page_id ↔ file offset mapping, real `fsync` |
| `BufferPool` | `src/storage/buffer_pool.cpp` | LRU eviction, pin/unpin, strong exception guarantee |
| `Tuple` | `src/storage/tuple.cpp` | `[null bitmap | fixed | lengths | var data]` layout, region-validated |
| `VectorHeap` | `src/storage/vector_heap.cpp` | Fixed-stride computed addressing for large embeddings; Sync/Rollback |
| `ConfigManager` | `src/utils/config.cpp` | YAML config + `AXOLOTLDB_*` env variable overrides |
| `Logger` | `src/utils/logger.cpp` | spdlog facade, console + rotating file sink |
| `Allocator` | `src/utils/memory.cpp` | Raw / tracking / guarded allocator, over-alignment support |

---

## Building

### Prerequisites

- **C++20** compiler: GCC ≥ 12, Clang ≥ 16, or MSVC ≥ 19.30 (VS 2022)
- **CMake** ≥ 3.21
- **Ninja** (recommended) or MSBuild / Make
- Internet access on first build (CMake `FetchContent` pulls GoogleTest, spdlog, yaml-cpp, Eigen, Asio, CLI11, nlohmann/json)

### Quick start

```bash
# Clone
git clone https://github.com/youssefbenyoussefyby/Axolotl.git
cd Axolotl

# Configure (Linux / macOS)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=ON

# Configure (Windows — no Ninja needed, MSVC auto-detected)
cmake -B build -DENABLE_TESTING=ON

# Build
cmake --build build --parallel

# Run tests
ctest --test-dir build --output-on-failure --no-tests=error
```

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTING` | `ON` | Build and register test targets with CTest |
| `ENABLE_COVERAGE` | `OFF` | Add `--coverage` flags (GCC/Clang only) |
| `BUILD_SERVER` | `ON` | Build the server binary |
| `BUILD_CLI` | `ON` | Build the CLI binary |

---

## Project structure

```
.
├── include/          # Public headers (storage/, utils/, catalog/, …)
├── src/              # Implementation (.cpp files mirroring include/)
│   ├── storage/      # Page, FileManager, BufferPool, Tuple, VectorHeap, …
│   └── utils/        # Logger, ConfigManager, Allocator
├── cli/              # CLI binary (axolotldb-cli)
├── server/           # Server binary (axolotldb-server) — stub
├── tests/            # GoogleTest suites
├── benchmarks/       # Micro-benchmarks (Google Benchmark)
├── docs/
│   ├── ROADMAP.md    # Architecture, on-disk format spec, phase gates
│   ├── audit.md      # Full Phase 1 audit findings (A1–A8, B1–B8, C1–C14, D1–D9, N1–N14)
│   └── phases/       # Per-phase reading lists and implementation checklists
├── cmake/            # CMake helper modules and package config template
├── third_party/      # Vendored / FetchContent dependencies
├── Dockerfile        # Multi-stage build image
├── docker-compose.yml
├── .github/workflows/main.yml  # CI: Linux (GCC+Clang), Windows/MSVC, ASan/UBSan, Coverage, Docker
└── THIRD_PARTY_NOTICES.md
```

---

## Running via Docker

```bash
# Build the image
docker build -t axolotldb:dev .

# Run the server (exits immediately in current stub)
docker run --rm axolotldb:dev axolotldb-server

# Or use docker-compose
docker-compose up
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Architecture, on-disk format catalogue, per-phase API specs and gates |
| [`docs/audit.md`](docs/audit.md) | Full Phase 1 audit — 45 findings across storage, CI, build, style |
| [`docs/phases/README.md`](docs/phases/README.md) | Phase index with reading lists and implementation checklists |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Licences for all third-party dependencies |

---

## Contributing

This is a personal research and learning project. Issues and PRs are welcome, but
there is no support commitment at this stage.

Before opening a PR, please:
1. Run `clang-format` (config in [`.clang-format`](.clang-format))
2. Ensure all tests pass: `ctest --test-dir build --output-on-failure`
3. Keep commits focused and messages descriptive

---

## License

[GNU General Public License v3.0](LICENSE) — © 2026 Youssef Ben Youssef