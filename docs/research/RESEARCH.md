# AxolotDB — Research Plan

*Status as of 2026-10-06. Living document: update it as findings and decisions change.*

## 1. Summary

AxolotDB is a relational + vector database written from scratch in C++20 (namespace `axolotdb`). The engineering goal is a complete, tested stack, from page storage to SQL to ANN search. The research goal is to use that stack as a testbed for **language-model-aware database features**. Because every layer is owned and tested, a research change can be isolated to one component and measured against a trusted baseline.

## 2. Current system state

| Phase | Scope | State |
|---|---|---|
| P0 | Hardening: N1–N4, N10–N14, green CI | Done |
| P1 | Storage core: Page, FileManager, BufferPool, Tuple, VectorHeap | Done |
| P2 | Storage services: StorageManager, PageGuard, TableHeap, Catalog, WAL contract | Done |
| P3 | SQL front end: lexer, parser, binder, logical planner | Done |
| P4 | Execution engine (Volcano) | In progress. **P4.2 milestone reached**: SeqScan + Filter + Projection run end-to-end against real files |
| P5–P10 | WAL/recovery, B+Tree, HNSW, cost-based optimizer, server, embeddings | Not started |

Notes:
- `ROADMAP.md` §1.1 was updated on 2026-10-09 to show P3 done and P4 in progress.
- Experiments follow the protocol in [`experiments/README.md`](../../experiments/README.md) (pre-registration, pinned seeds, results committed under the producing SHA). The first one, [E001](../../experiments/E001-semantic-filter-ordering/README.md), is defined but not run.
- The CLI has a dev mode (`:dev`) with SQL execution, a roadmap view, demo seeding and a `queries.json` gate that blocks SQL features not yet built.
- Design invariants relevant to research:
  - Logical and physical plans are separate, so an optimizer can be swapped in without touching executors.
  - Recall is defined against an exact brute-force oracle.
  - Plan-tree assertions replace timing assertions in tests.
  - References carry proof of what they refer to, so silent wrong answers are engineered out.

## 3. Research directions

Three ideas were chosen and are meant to be combined. A fourth (an inference-oriented DB) was dropped.

### 3.1 Semantic filters for text columns
Predicates over text columns evaluated by meaning rather than by string match, for example a filter that keeps rows whose text satisfies a natural-language condition. The questions are where the predicate sits in the plan, how it interacts with ordinary predicates and vector search, and how its cost should be modelled, since it is orders of magnitude more expensive than a comparison.

- **Depends on:** P4 (operator framework), P7 (vector search), P10 (embeddings or a model call-out).
- **Natural experiment:** predicate ordering and pushdown when one predicate has a model call and the others are cheap.

### 3.2 Model-assisted query optimization
A decision model consumes the plan tree plus statistics and proposes or ranks plans. The first idea was to send plan trees and statistics to a decision model. This builds directly on the logical/physical split from P4 and the cost-model work in P8.

- **Baseline:** the Selinger-style DP optimizer and the cost model planned for P8.
- **Correctness guard:** every model-chosen plan must return identical rows to the baseline plan, using the same cross-validation technique the project already uses for joins.
- **Open question:** how to handle a model that proposes an invalid or slower plan (fallback policy, validation cost).

### 3.3 Forgetting / memory-like storage with semantic compression
For text-heavy systems, old or low-value rows are compressed semantically rather than deleted outright, loosely like memory consolidation. This fits the project's existing accepted debt around tombstones and compaction (B7 and vector heap deletion).

- **Open questions:** what "value" means (access frequency, recency, redundancy), what recall or answer-quality loss is acceptable, and how to make the lossy step explicit in the schema so it never produces a silent wrong answer.

### 3.4 Possible extension
Storing knowledge graphs and making them queryable from the same engine. Not yet scoped.

## 4. Why this system is a good testbed

- **Single-component isolation.** Each research feature can replace one component (an operator, the optimizer, or a storage policy) while the rest stays fixed.
- **Trusted oracles already exist.** Brute-force KNN gives exact ground truth, and a plain C++ oracle checks query results.
- **Deterministic evaluation.** Plan-tree assertions avoid flaky timing in CI, and benchmarks live separately as evidence.

## 5. Evaluation approach (draft)

| Direction | Metric | Baseline |
|---|---|---|
| Semantic filters | Result quality against labelled ground truth; end-to-end latency and model calls per query | Exact string/regex predicates; evaluate-all-then-filter |
| Model-assisted optimizer | Plan cost and latency vs. the DP optimizer; rate of invalid plans | P8 cost-based optimizer; rule-only P4 optimizer |
| Forgetting / compression | Storage saved vs. recall/answer quality on a fixed query set | No compression; plain deletion |

All experiments should be reproducible, with fixed seeds, committed datasets or generators, and results recorded in-repo so a regression shows up as a diff.

## 6. Constraints and open decisions

- **Compute:** public LLM API keys only, no local GPU mentioned. This favours experiments that call a hosted model sparingly and cache results.
- **Harness:** undecided whether the experiment harness is Python wrapped around the C++ engine or lives entirely in C++.
- **Mentorship:** no professor or mentor has reviewed the plan yet. Getting one reader would be the highest-value next step for credibility.
- **Timeline:** no hard deadlines, but the literature search should start now.
- **Sequencing:** most directions need P7 (vector search) and P8 (optimizer), so there is a long engineering path before the first real experiment. A cheaper early experiment on top of P4 would keep the research moving. Semantic filters evaluated as a plain operator are the most promising candidate.

## 7. Related work (starting list)

Every entry below is **unverified**: it was compiled from memory as a starting point.
Before citing any of them, confirm the title, authors, venue and year against the
publisher's page, then change its tag to *verified*. Each entry has a one-line note on
why it matters here.

### 7.1 Expensive and semantic predicates (direction 3.1)

| Reference | Status | Relevance |
|---|---|---|
| Hellerstein & Stonebraker, "Predicate Migration: Optimizing Queries with Expensive Predicates", SIGMOD 1993 | unverified | Classic rank ordering for expensive predicates; the rule tested in E001 H2. |
| Hellerstein, "Optimization Techniques for Queries with Expensive Methods", ACM TODS 1998 | unverified | Extends predicate migration to joins; cost model for user-defined functions. |
| Patel et al., "LOTUS: Enabling Semantic Queries with LLMs Over Tables of Unstructured and Structured Data" (semantic operators), arXiv 2024 | unverified | Defines semantic filter/join/top-k operators and optimizes them with cheap proxy models; closest prior system. |
| Liu et al., "Palimpzest: Optimizing AI-Powered Analytics with Declarative Query Processing", CIDR 2025 | unverified | Cost/quality optimizer for LLM-backed operators; baseline for cost modelling. |
| Shankar et al., "DocETL: Agentic Query Rewriting and Evaluation for Complex Document Processing", arXiv 2024 | unverified | LLM-driven rewrites of document-processing pipelines. |
| Kang et al., "NoScope: Optimizing Neural Network Queries over Video at Scale", VLDB 2017 | unverified | Cheap specialised models in front of an expensive model, which is the cascade pattern for semantic filters. |
| Lu et al., "Accelerating Machine Learning Inference with Probabilistic Predicates", SIGMOD 2018 | unverified | Cheap pre-filters with accuracy guarantees ahead of ML predicates. |

### 7.2 Learned and model-assisted optimization (direction 3.2)

| Reference | Status | Relevance |
|---|---|---|
| Selinger et al., "Access Path Selection in a Relational Database Management System", SIGMOD 1979 | unverified | The DP baseline built in P8. |
| Leis et al., "How Good Are Query Optimizers, Really?", VLDB 2015 | unverified | Join Order Benchmark; shows cardinality error dominates plan quality. |
| Marcus et al., "Neo: A Learned Query Optimizer", VLDB 2019 | unverified | End-to-end learned optimizer; the "replace the optimizer" design point. |
| Marcus et al., "Bao: Making Learned Query Optimization Practical", SIGMOD 2021 | unverified | Steers a classical optimizer with hints instead of replacing it, which matches this project's "classical fallback" stance. |
| Kipf et al., "Learned Cardinalities: Estimating Correlated Joins with Deep Learning", CIDR 2019 | unverified | Learned cardinality estimation; an alternative insertion point for a model. |
| Akioyamen et al., "The Unreasonable Effectiveness of LLMs for Query Optimization", arXiv 2024 | unverified | Direct use of LLM embeddings/outputs for plan selection; nearest work to 3.2. |

### 7.3 Vector search and hybrid queries (P7 and 3.1)

| Reference | Status | Relevance |
|---|---|---|
| Malkov & Yashunin, "Efficient and Robust Approximate Nearest Neighbor Search Using Hierarchical Navigable Small World Graphs", IEEE TPAMI 2018 | unverified | The HNSW index P7 implements. |
| Jégou, Douze & Schmid, "Product Quantization for Nearest Neighbor Search", IEEE TPAMI 2011 | unverified | Compression for vectors, relevant to both P7 and 3.3. |
| Aumüller, Bernhardsson & Faithfull, "ANN-Benchmarks", Information Systems 2020 | unverified | Standard recall/QPS methodology to copy for P7 evaluation. |
| Wang et al., "Milvus: A Purpose-Built Vector Data Management System", SIGMOD 2021 | unverified | System design reference for filtered vector queries. |
| Gollapudi et al., "Filtered-DiskANN: Graph Algorithms for Approximate Nearest Neighbor Search with Filters", WWW 2023 | unverified | Combining attribute filters with graph ANN, which is where semantic and relational predicates meet. |

### 7.4 Forgetting and semantic compression (direction 3.3)

| Reference | Status | Relevance |
|---|---|---|
| Kersten & Sidirourgos, "A Database System with Amnesia", CIDR 2017 | unverified | Proposes a DBMS that deliberately forgets data under a storage budget; closest prior framing of 3.3. |
| Babu, Garofalakis & Rastogi, "SPARTAN: A Model-Based Semantic Compression System for Massive Data Tables", SIGMOD 2001 | unverified | Lossy, error-bounded semantic compression of tables; template for making the loss explicit. |
| Jagadish, Madar & Ng, "Semantic Compression and Pattern Extraction with Fascicles", VLDB 1999 | unverified | Earlier semantic compression with per-attribute tolerances. |
| Park et al., "Generative Agents: Interactive Simulacra of Human Behavior", UIST 2023 | unverified | Memory retrieval scored by recency, importance and relevance; a candidate definition of row "value". |
| Packer et al., "MemGPT: Towards LLMs as Operating Systems", arXiv 2023 | unverified | Tiered memory with summarisation/eviction for LLM agents; the workload side of 3.3. |

### 7.5 Still to search

- Knowledge-graph storage and querying inside a relational engine (direction 3.4).
- Hybrid relational + vector benchmarks with filters, to pick E002's dataset.
- Cost models for remote model calls (latency, price, rate limits) in query processing.

## 8. Next steps

1. Finish P4 (joins, aggregation, DML) while keeping the public repo clean. The repo goes public at the P4.2 milestone.
2. ~~Update `ROADMAP.md` §1.1 to match reality.~~ Done 2026-10-09.
3. Verify the starting list in section 7 entry by entry, then extend it (7.5).
4. ~~Pick one small first experiment that needs only P4.~~ Defined as [E001](../../experiments/E001-semantic-filter-ordering/README.md); it is ready to run once P4.3 INSERT and a `SemanticFilter` operator exist.
5. Find one reader (professor or researcher) for this plan.