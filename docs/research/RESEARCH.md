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
- `ROADMAP.md` §1.1 still shows P3 as NEXT and P4 as not started. It needs a status update.
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

## 7. Literature search plan

Start with these areas and record each source with a one-line relevance note. Verify every citation before using it in a paper or CV.

- Cost-based optimization foundations: Selinger et al. (SIGMOD 1979), already in the P8 reading list.
- Learned and model-guided query optimizers.
- LLM-powered or semantic operators in query engines.
- Vector search and hybrid filter + ANN queries: the HNSW paper and the pgvector design.
- Data summarization, lossy compression and retention policies in databases.

## 8. Next steps

1. Finish P4 (joins, aggregation, DML) while keeping the public repo clean. The repo goes public at the P4.2 milestone.
2. Update `ROADMAP.md` §1.1 to match reality.
3. Do the literature search and fill in section 7 with real, verified references.
4. Pick one small first experiment that needs only P4, and write its hypothesis, dataset and metric.
5. Find one reader (professor or researcher) for this plan.