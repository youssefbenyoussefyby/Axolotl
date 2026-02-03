# Axolotl

Vector DB

## TODO (Based on Project Plan)

### Phase 1: Foundation & Infrastructure (Weeks 1-6)
- [x] Finalize repository/build baseline (`CMakeLists.txt`, toolchain, presets)
- [x] Add CI pipeline (build + test on PR)
- [x] Configure formatting/linting (`clang-format`, warnings policy)
- [x] Set up test harness (GoogleTest/GoogleMock)
- [x] Implement core utils: logger, config, memory, errors
- [x] Implement storage foundation: page, file manager, buffer pool (LRU), tuple
- [x] Implement vector storage: separate `VectorHeap` for embeddings larger than a page
- [ ] Implement storage services: `StorageManager`, `PageGuard`, `TableHeap`, catalog
- [ ] Add unit tests for core components and target >80% coverage

### Phase 2: Parser & Catalog (Weeks 7-10)
- [ ] Define SQL subset grammar in `src/parser/parser.y` and tokens in `src/parser/lexer.l`
- [ ] Implement AST + semantic analyzer
- [ ] Support DDL: `CREATE TABLE`, `CREATE INDEX`, `DROP`
- [ ] Implement catalog metadata persistence (tables/columns/indexes)
- [ ] Add statistics collection for optimizer
- [ ] Add schema/type/name validation
- [ ] Produce initial logical plan for basic SELECT

### Phase 3: Execution Engine (Weeks 11-16)
- [ ] Implement operators: scan, filter, project, insert, update, delete
- [ ] Implement joins: nested loop, hash join (sort-merge optional)
- [ ] Implement aggregation: GROUP BY + COUNT/SUM/AVG/MIN/MAX + HAVING
- [ ] Implement optimizer core: cost model, predicate/projection pushdown
- [ ] Add join ordering strategy and index selection
- [ ] Add integration tests for complex SELECT, joins, aggregates

### Phase 4: Relational Indexing (Weeks 17-20)
- [ ] Implement disk-backed B+ tree (split/merge/range scan)
- [ ] Implement hash index for equality lookup
- [ ] Add index scan operators
- [ ] Add index maintenance on DML operations
- [ ] Wire index selection into optimizer
- [ ] Add `CREATE INDEX`/`DROP INDEX` execution path + tests

### Phase 5: Vector Indexing & ANN (Weeks 21-26)
- [ ] Implement vector type and vector column storage
- [ ] Implement distance metrics (L2, cosine)
- [ ] Implement HNSW graph/index/search
- [ ] Add IVF index as optional path
- [ ] Add KNN and hybrid query execution (`filter + vector`)
- [ ] Add recall/latency benchmarks under `benchmarks/vector_bench`

### Phase 6: Semantic Search & Embeddings (Weeks 27-30)
- [ ] Integrate ONNX Runtime embedding inference pipeline
- [ ] Load sentence-transformer model from `models/sentence-bert`
- [ ] Implement auto-embedding on INSERT/UPDATE
- [ ] Add batch embedding for bulk ingestion
- [ ] Add semantic query flow: text -> embedding -> vector search -> ranking
- [ ] Benchmark semantic vs keyword retrieval quality

### Phase 7: Transactions & Durability (Weeks 31-34)
- [ ] Implement WAL record format and append/replay flow
- [ ] Implement BEGIN/COMMIT/ROLLBACK transaction manager
- [ ] Add locking or MVCC baseline
- [ ] Implement crash recovery procedure and tests
- [ ] Add isolation support (minimum: READ COMMITTED)
- [ ] Add concurrency/deadlock tests

### Phase 8: Client/Server & CLI (Weeks 35-36)
- [ ] Define network protocol and message framing
- [ ] Implement TCP server + session management + thread pool
- [ ] Implement CLI REPL and script execution mode
- [ ] Add output formatting modes (table/JSON/CSV)
- [ ] Add multi-client load tests

## Cross-Cutting Quality Gates
- [ ] Keep `-Wall -Wextra` warning-free
- [ ] Add API/docs for public modules
- [ ] Track performance targets (SELECT, JOIN, KNN, TPS)
- [ ] Verify memory safety (ASan/Valgrind where applicable)

## Immediate Next Tasks
- [x] Complete storage engine MVP (`page`, `file_manager`, `buffer_pool`, `tuple`)
- [x] Add tests for storage read/write + eviction behavior
- [x] Harden Phase 1 (P0): page validation, CI baseline — see [docs/ROADMAP.md](docs/ROADMAP.md)
- [ ] Build the storage services layer (P2), then land the first end-to-end path:
      parse -> plan -> table scan -> result

> The phase breakdown below is the original plan. It has been restructured —
> see **[docs/ROADMAP.md](docs/ROADMAP.md)** for the current phases, the Phase 1
> audit, and the on-disk format specification.

## Project Structure

```text
.
|-- include/
|   |-- parser/
|   |-- optimizer/
|   |-- executor/
|   |-- storage/
|   |-- index/
|   |-- vector/
|   |-- catalog/
|   |-- network/
|   `-- utils/
|-- src/
|-- cli/
|-- server/
|-- tests/
|-- benchmarks/
|-- third_party/
`-- models/
```