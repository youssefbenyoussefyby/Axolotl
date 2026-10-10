// tests/unit/executor_p42_test.cpp
//
// P4.2 proof: SeqScan + Filter + Projection.
//
// RULES FOR THIS FILE
//  * Every expected value comes from plain C++ over the generated dataset
//    (the ORACLE), never from the executors under test.
//  * Filter / Projection are tested against a ScriptedSource, so a SeqScan bug
//    cannot hide a Filter bug and vice versa. SeqScan is tested separately
//    against a real heap, and the full pipeline last.
//  * Every test group lists a MUTATION it must kill. Apply each mutation once,
//    confirm the named test FAILS, then revert. A suite that has not been seen
//    to fail proves nothing.
//
// MUTATIONS (apply one at a time)
//  M1  Filter: drop the unconsumed rest of a child batch when output fills (old bug)
//        -> NoRowsLostAcrossBatchBoundaries, ResultIdenticalAcrossAllBatchSizes
//  M2  Filter: replay leftovers with queue.back() (duplicates one row)
//        -> NoRowsLostAcrossBatchBoundaries (duplicate/exact-order check)
//  M3  Filter: IsTrue(NULL) returns true
//        -> NullPredicateExcludesRow, TruePlusFalsePartitionLeavesNullsOut
//  M4  Filter: leftover state not cleared in Init()
//        -> InitMidScanThenRerunMatchesFreshRun
//  M5  Filter: dereference childRids when ridBatch is nullopt
//        -> WorksWithoutRids
//  M6  Filter: do not push RIDs for leftover rows
//        -> RidsStayAlignedWithTuples, RealHeapScanFilterKeepsCorrectRids
//  M7  Any operator: stop clearing the output vectors at the start of Next()
//        -> OutputsAreClearedOnEveryCall
//  M8  Filter: output batch allowed to exceed batchSize
//        -> OutputBatchNeverExceedsBatchSize
//  M9  SeqScan: return a span into the frame instead of copying bytes (N5)
//        -> TuplesOwnTheirBytesAfterEviction
//  M10 SeqScan: skip Init() resetting the cursor
//        -> ReinitRestartsTheScan
//  M11 SeqScan: include tombstoned rows
//        -> SkipsTombstones
//  M12 Projection: reorder/evaluate columns from the wrong index
//        -> ComputedExpressionColumn, OutputSchemaMatchesProjectionList
//  M13 Projection: lose NULL bits when re-serialising
//        -> NullsSurviveProjection
//  M14 SeqScan: report a RID that is not the row's own RID (stale / shifted slot)
//        -> ReturnsEveryLiveRowInRidOrderUnderEviction (full page+slot check),
//           SkipsTombstones, RealHeapScanFilterKeepsCorrectRids
//  M15 Filter: return when one child pull passed nothing (early stop)
//        -> SingleMatchIsFoundWhereverItSits (and the batch-size sweeps)
//  M16 Filter: only push a row when plan_.predicate is non-null
//        -> NoPredicatePassesEveryRow
//  M17 Filter: leftover state shared (static/global) instead of per instance
//        -> InterleavedInstancesDoNotShareState, TwoStackedFiltersMatchOracleWithAlignedRids
//  M18 Filter: tolerate a RID-mode switch while rows are pending
//        -> SwitchingToRidsMidBatchIsRejected
//  M19 Filter: throw an unrelated exception where the RID misalignment check sits
//        -> ChildThatIgnoresRidRequestIsRejected (now checks type and message)

#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <executor/operators/filter.h>
#include <executor/operators/project.h>
#include <executor/operators/scan.h>
#include <gtest/gtest.h>
#include <optimizer/physical_plan.h>
#include <parser/ast/boundstmt.h>
#include <storage/buffer_pool.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace AxolotlDB;
using namespace AxolotlDB::executor;

// ============================================================================
// ADAPTER
// ============================================================================
namespace adapter {

struct Row {
    int32_t id;
    std::optional<int32_t> v;
};

storage::Schema TwoColSchema() {
    return storage::Schema({
        storage::ColumnDef{"id", storage::ColumnType::INT32},
        storage::ColumnDef{"v", storage::ColumnType::INT32, true},  // nullable: data has NULLs
    });
}

storage::Value IntValue(int32_t x) {
    return storage::Value::Int32(x);
}
storage::Value NullValue() {
    return storage::Value::Null(storage::ColumnType::INT32);
}

storage::RID MakeRid(int32_t n) {
    storage::RID r;
    r.page_id = static_cast<storage::PageId>(n);
    r.slot_idx = 0;
    return r;
}
int32_t RidToInt(const storage::RID& r) {
    return static_cast<int32_t>(r.page_id);
}

// Full RID equality (page AND slot). Used for real-heap RID checks, where many
// rows share one page and page_id alone cannot tell them apart.
bool RidEq(const storage::RID& a, const storage::RID& b) {
    return a.page_id == b.page_id && a.slot_idx == b.slot_idx;
}

storage::Tuple MakeTuple(const storage::Schema& s, const Row& r, bool withRid) {
    std::vector<storage::Value> vals{IntValue(r.id), r.v ? IntValue(*r.v) : NullValue()};
    auto bytes = storage::Tuple::Serialize(s, vals);
    // Single-argument construction, then SetRID: avoids depending on how the
    // two-argument overload is resolved.
    storage::Tuple t(std::move(bytes));
    if (withRid)
        t.SetRID(MakeRid(r.id));
    return t;
}

std::optional<int32_t> DecodeInt(const storage::Tuple& t, const storage::Schema& s, size_t col) {
    auto values = storage::Tuple::Deserialize(s, t.Span());
    const auto& v = values[col];
    if (v.IsNull())
        return std::nullopt;
    return v.AsInt32();
}

// ---- ExecutorContext factory ------------------------------------------------
// ScriptedSource tests never touch storage, but ExecutorContext needs live
// references. The global state owns a dummy SM + Catalog in a temp dir.
//
// FIX: the working directory is restored right after construction, so no test
// is left running inside a temp dir, and the dir is removed at exit.
// The scripted contexts never perform file I/O, so they do not need the cwd.

struct GlobalCtxState {
    std::filesystem::path dir;
    std::unique_ptr<storage::StorageManager> sm;
    std::unique_ptr<catalog::Catalog> cat;

    GlobalCtxState() {
        const auto orig = std::filesystem::current_path();
        // Unique per process: ctest runs every TEST() as its own process, and with
        // `ctest --parallel` a shared fixed name let one process remove_all() the
        // catalog another was still using.
        dir = std::filesystem::temp_directory_path() /
              ("axo_p42_scripted_ctx_" + std::to_string(std::random_device{}()) + "_" +
               std::to_string(static_cast<uint64_t>(
                   std::chrono::steady_clock::now().time_since_epoch().count())));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
        sm = std::make_unique<storage::StorageManager>(4);
        cat = std::make_unique<catalog::Catalog>(*sm);
        std::filesystem::current_path(orig);
    }
    ~GlobalCtxState() {
        cat.reset();
        sm.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);  // best effort; never throw from a dtor
    }
};

static GlobalCtxState& GlobalState() {
    static GlobalCtxState s;
    return s;
}

std::unique_ptr<ExecutorContext> MakeCtx(size_t batch) {
    auto& g = GlobalState();
    return std::make_unique<ExecutorContext>(*g.sm, *g.cat, nullptr, batch);
}

// ---- PhysicalFilter builders ------------------------------------------------
inline parser::ast::BoundExprPtr MakeColRef(uint32_t idx, storage::ColumnType t) {
    parser::ast::BoundColumnRef ref;
    ref.tableId = 0;
    ref.columnIndex = idx;
    ref.columnName = (idx == 0) ? "id" : "v";
    ref.type = t;
    return std::make_unique<parser::ast::BoundExpr>(std::move(ref), t);
}

inline parser::ast::BoundExprPtr MakeLitInt32(int32_t val) {
    parser::ast::BoundLiteral lit{storage::ColumnType::INT32, static_cast<int64_t>(val)};
    return std::make_unique<parser::ast::BoundExpr>(std::move(lit), storage::ColumnType::INT32);
}

// resultType: BOOLEAN for comparisons (default), INT32 for arithmetic. The old
// version typed EVERY node BOOLEAN, so `id + 1000` was declared boolean and
// Evaluate correctly refused it ("arithmetic result has non-numeric type").
inline parser::ast::BoundExprPtr MakeBinaryOp(
    parser::ast::BinaryOperator op, parser::ast::BoundExprPtr l, parser::ast::BoundExprPtr r,
    storage::ColumnType resultType = storage::ColumnType::BOOLEAN) {
    parser::ast::BoundBinaryOp biop{op, std::move(l), std::move(r), resultType};
    return std::make_unique<parser::ast::BoundExpr>(std::move(biop), resultType);
}

inline std::unique_ptr<optimizer::PhysicalFilter> MakeFilterPlan(
    parser::ast::BinaryOperator op, int32_t k, const storage::Schema& child_schema) {
    auto pred = MakeBinaryOp(op, MakeColRef(1, storage::ColumnType::INT32), MakeLitInt32(k));

    auto plan = std::make_unique<optimizer::PhysicalFilter>();
    plan->predicate = std::move(pred);

    for (size_t i = 0; i < child_schema.ColumnCount(); ++i) {
        optimizer::OutputColumn oc;
        oc.name = child_schema.columns[i].name;
        oc.type = child_schema.columns[i].type;
        oc.tableId = 0;
        oc.columnIndex = static_cast<uint32_t>(i);
        plan->outputSchema.push_back(std::move(oc));
    }
    return plan;
}

std::unique_ptr<optimizer::PhysicalFilter> PredGE(int32_t k) {
    return MakeFilterPlan(parser::ast::BinaryOperator::GREATER_EQUAL, k, TwoColSchema());
}
std::unique_ptr<optimizer::PhysicalFilter> PredLT(int32_t k) {
    return MakeFilterPlan(parser::ast::BinaryOperator::LESS, k, TwoColSchema());
}

// ---- PhysicalProject builder -------------------------------------------------
std::unique_ptr<optimizer::PhysicalProject> ProjIdVIdPlus1000() {
    auto plan = std::make_unique<optimizer::PhysicalProject>();

    {
        optimizer::ProjectColumn pc;
        pc.name = "id";
        pc.expr = MakeColRef(0, storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }
    {
        optimizer::ProjectColumn pc;
        pc.name = "v";
        pc.expr = MakeColRef(1, storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }
    {
        optimizer::ProjectColumn pc;
        pc.name = "id_plus_1000";
        pc.expr = MakeBinaryOp(parser::ast::BinaryOperator::ADD,
                               MakeColRef(0, storage::ColumnType::INT32), MakeLitInt32(1000),
                               storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }

    plan->outputSchema = {
        optimizer::OutputColumn{"id", storage::ColumnType::INT32, 0, 0},
        optimizer::OutputColumn{"v", storage::ColumnType::INT32, 0, 1},
        optimizer::OutputColumn{"id_plus_1000", storage::ColumnType::INT32, 0, 0},
    };
    return plan;
}

storage::Schema ProjOutSchema() {
    return storage::Schema({
        storage::ColumnDef{"id", storage::ColumnType::INT32},
        storage::ColumnDef{"v", storage::ColumnType::INT32, true},
        storage::ColumnDef{"id_plus_1000", storage::ColumnType::INT32},
    });
}

// ---- Real-heap helpers -------------------------------------------------------
static storage::Schema RealTableSchema() {
    return storage::Schema({
        storage::ColumnDef{"id", storage::ColumnType::INT32},
        storage::ColumnDef{"v", storage::ColumnType::INT32, true},
        storage::ColumnDef{"pad", storage::ColumnType::VARCHAR},
    });
}

struct TestDb {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    std::unique_ptr<storage::StorageManager> sm;
    std::unique_ptr<catalog::Catalog> cat;
    std::unique_ptr<ExecutorContext> ctx;
    std::string table = "t";
    std::vector<storage::RID> rids;  // rids[row_index]
    storage::Schema schema = RealTableSchema();

    // FIX: tear down in dependency order (ctx -> cat -> sm), then leave the temp
    // dir, then delete it. Without this every test leaked its dir and left the
    // process cwd inside it (which also blocks deletion on Windows).
    ~TestDb() {
        ctx.reset();
        cat.reset();
        sm.reset();
        std::error_code ec;
        if (!orig_dir.empty())
            std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }
};

static std::string MakePad() {
    return std::string(200, 'x');
}

std::unique_ptr<TestDb> BuildDb(const std::vector<Row>& rows, size_t poolFrames) {
    auto db = std::make_unique<TestDb>();
    db->dir = std::filesystem::temp_directory_path() /
              ("axo_p42_" + std::to_string(static_cast<uint64_t>(
                                std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    std::filesystem::remove_all(db->dir, ec);
    std::filesystem::create_directories(db->dir);
    db->orig_dir = std::filesystem::current_path();
    std::filesystem::current_path(db->dir);

    db->sm = std::make_unique<storage::StorageManager>(poolFrames);
    db->cat = std::make_unique<catalog::Catalog>(*db->sm);
    db->ctx = std::make_unique<ExecutorContext>(*db->sm, *db->cat, nullptr, 16);

    db->cat->CreateTable(db->table, db->schema);
    auto& heap = db->cat->GetTableHeap(db->table);

    for (const auto& r : rows) {
        std::vector<storage::Value> vals{
            storage::Value::Int32(r.id),
            r.v ? storage::Value::Int32(*r.v) : storage::Value::Null(storage::ColumnType::INT32),
            storage::Value::Varchar(MakePad()),
        };
        auto bytes = storage::Tuple::Serialize(db->schema, vals);
        db->rids.push_back(heap.Insert(bytes));
    }
    db->sm->FlushAll();
    return db;
}

void DeleteRows(TestDb& db, const std::vector<size_t>& datasetIdx) {
    auto& heap = db.cat->GetTableHeap(db.table);
    for (size_t i : datasetIdx)
        heap.Delete(db.rids[i]);
}

size_t TablePageCount(TestDb& db) {
    auto& heap = db.cat->GetTableHeap(db.table);
    std::set<storage::PageId> pages;
    for (auto it = heap.begin(); it != heap.end(); ++it) {
        pages.insert((*it).page_id);
    }
    return pages.empty() ? 1 : pages.size();
}

void Rebatch(TestDb& db, size_t batch) {
    db.ctx->SetBatchSize(batch);
}

std::unique_ptr<optimizer::PhysicalSeqScan> ScanPlan(TestDb& db) {
    auto plan = std::make_unique<optimizer::PhysicalSeqScan>();
    plan->tableName = db.table;
    plan->tableId = db.cat->GetTableId(db.table);
    plan->schema = db.schema;

    for (size_t i = 0; i < db.schema.ColumnCount(); ++i) {
        optimizer::OutputColumn oc;
        oc.name = db.schema.columns[i].name;
        oc.type = db.schema.columns[i].type;
        oc.tableId = plan->tableId;
        oc.columnIndex = static_cast<uint32_t>(i);
        plan->outputSchema.push_back(std::move(oc));
    }
    return plan;
}

}  // namespace adapter

using adapter::Row;

// ============================================================================
// Deterministic data + ORACLE
// ============================================================================
static std::vector<Row> MakeRows(int n) {
    std::vector<Row> rows;
    for (int i = 0; i < n; ++i) {
        Row r{i, static_cast<int32_t>((static_cast<int64_t>(i) * 37) % 100)};
        if (i % 7 == 3)
            r.v = std::nullopt;
        rows.push_back(r);
    }
    return rows;
}
static std::vector<int32_t> OracleGE(const std::vector<Row>& rows, int32_t k) {
    std::vector<int32_t> out;
    for (const auto& r : rows)
        if (r.v && *r.v >= k)
            out.push_back(r.id);
    return out;
}
static std::vector<int32_t> OracleLT(const std::vector<Row>& rows, int32_t k) {
    std::vector<int32_t> out;
    for (const auto& r : rows)
        if (r.v && *r.v < k)
            out.push_back(r.id);
    return out;
}

// ============================================================================
// ScriptedSource
// ============================================================================
class ScriptedSource : public AbstractExecutor {
   public:
    ScriptedSource(ExecutorContext* ctx, storage::Schema schema, std::vector<Row> rows,
                   size_t chunk, bool honourRidRequests = true)
        : AbstractExecutor(ctx),
          schema_(std::move(schema)),
          rows_(std::move(rows)),
          chunk_(chunk),
          honourRids_(honourRidRequests) {}

    void Init() override {
        pos_ = 0;
        ++initCalls;
    }

    bool Next(std::vector<storage::Tuple>* tb,
              std::optional<std::vector<storage::RID>>& rb) override {
        tb->clear();
        if (rb.has_value())
            rb->clear();
        if (pos_ >= rows_.size())
            return false;
        const size_t end = std::min(rows_.size(), pos_ + chunk_);
        for (; pos_ < end; ++pos_) {
            const bool wantRid = rb.has_value() && honourRids_;
            tb->push_back(adapter::MakeTuple(schema_, rows_[pos_], wantRid));
            if (wantRid)
                rb->push_back(adapter::MakeRid(rows_[pos_].id));
        }
        return true;
    }
    const storage::Schema& GetOutputSchema() const override {
        return schema_;
    }

    int initCalls = 0;

   private:
    storage::Schema schema_;
    std::vector<Row> rows_;
    size_t chunk_;
    bool honourRids_;
    size_t pos_ = 0;
};

// ============================================================================
// Drain helper
// ============================================================================
struct Drained {
    std::vector<int32_t> ids;
    std::vector<int32_t> ridInts;    // page_id only; fine for ScriptedSource (rid == id)
    std::vector<storage::RID> rids;  // full RIDs; use these for real-heap checks
    size_t calls = 0;
    size_t maxBatch = 0;
    bool ridMisaligned = false;
};

static Drained Drain(AbstractExecutor& ex, bool wantRids, bool callInit = true) {
    Drained d;
    if (callInit)
        ex.Init();
    const auto& schema = ex.GetOutputSchema();
    for (int guard = 0; guard < 1'000'000; ++guard) {
        std::vector<storage::Tuple> batch;
        std::optional<std::vector<storage::RID>> rids;
        if (wantRids)
            rids = std::vector<storage::RID>{};
        if (!ex.Next(&batch, rids)) {
            EXPECT_TRUE(batch.empty()) << "false return must leave the batch empty";
            break;
        }
        ++d.calls;
        d.maxBatch = std::max(d.maxBatch, batch.size());
        if (wantRids) {
            if (!rids.has_value() || rids->size() != batch.size())
                d.ridMisaligned = true;
            else
                for (auto& r : *rids) {
                    d.ridInts.push_back(adapter::RidToInt(r));
                    d.rids.push_back(r);
                }
        }
        for (auto& t : batch)
            d.ids.push_back(*adapter::DecodeInt(t, schema, 0));
    }
    return d;
}

static std::unique_ptr<FilterExecutor> MakeFilter(ExecutorContext* ctx,
                                                  const optimizer::PhysicalFilter& plan,
                                                  const std::vector<Row>& rows, size_t chunk,
                                                  bool honourRids = true) {
    auto src =
        std::make_unique<ScriptedSource>(ctx, adapter::TwoColSchema(), rows, chunk, honourRids);
    return std::make_unique<FilterExecutor>(ctx, plan, std::move(src));
}

// ============================================================================
// FILTER
// ============================================================================
TEST(FilterTest, NoRowsLostAcrossBatchBoundaries) {
    auto rows = MakeRows(1000);
    auto expected = OracleGE(rows, 50);
    ASSERT_GT(expected.size(), 300u);
    ASSERT_LT(expected.size(), 700u);

    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(5);
    auto f = MakeFilter(ctx.get(), *plan, rows, 7);
    auto got = Drain(*f, false);

    EXPECT_EQ(got.ids, expected);
    EXPECT_EQ(std::set<int32_t>(got.ids.begin(), got.ids.end()).size(), got.ids.size())
        << "duplicate rows returned";
}

TEST(FilterTest, ResultIdenticalAcrossAllBatchSizes) {
    auto rows = MakeRows(257);
    auto expected = OracleGE(rows, 50);
    auto plan = adapter::PredGE(50);
    for (size_t outBatch : {1u, 2u, 3u, 7u, 64u, 256u, 257u, 1024u})
        for (size_t chunk : {1u, 2u, 5u, 7u, 100u, 1024u}) {
            auto ctx = adapter::MakeCtx(outBatch);
            auto f = MakeFilter(ctx.get(), *plan, rows, chunk);
            EXPECT_EQ(Drain(*f, false).ids, expected)
                << "outBatch=" << outBatch << " chunk=" << chunk;
        }
}

TEST(FilterTest, OutputBatchNeverExceedsBatchSize) {
    auto rows = MakeRows(500);
    auto plan = adapter::PredGE(0);
    for (size_t outBatch : {1u, 3u, 7u, 50u}) {
        auto ctx = adapter::MakeCtx(outBatch);
        auto f = MakeFilter(ctx.get(), *plan, rows, 64);
        EXPECT_LE(Drain(*f, false).maxBatch, outBatch) << "outBatch=" << outBatch;
    }
}

TEST(FilterTest, NullPredicateExcludesRow) {
    auto rows = MakeRows(700);
    auto plan = adapter::PredGE(0);
    auto ctx = adapter::MakeCtx(16);
    auto f = MakeFilter(ctx.get(), *plan, rows, 9);
    auto got = Drain(*f, false);
    EXPECT_EQ(got.ids, OracleGE(rows, 0));
    for (int32_t id : got.ids)
        EXPECT_NE(id % 7, 3) << "a NULL-valued row leaked through";
}

TEST(FilterTest, TruePlusFalsePartitionLeavesNullsOut) {
    auto rows = MakeRows(700);
    auto ge = adapter::PredGE(50);
    auto lt = adapter::PredLT(50);
    auto ctx = adapter::MakeCtx(11);
    auto a = Drain(*MakeFilter(ctx.get(), *ge, rows, 13), false).ids;
    auto b = Drain(*MakeFilter(ctx.get(), *lt, rows, 13), false).ids;
    size_t nonNull = 0;
    for (auto& r : rows)
        if (r.v)
            ++nonNull;
    EXPECT_EQ(a.size() + b.size(), nonNull);
    EXPECT_EQ(a, OracleGE(rows, 50));
    EXPECT_EQ(b, OracleLT(rows, 50));
    std::set<int32_t> sa(a.begin(), a.end());
    for (int32_t id : b)
        EXPECT_EQ(sa.count(id), 0u) << "row in both partitions";
}

TEST(FilterTest, WorksWithoutRids) {
    auto rows = MakeRows(400);
    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(4);
    auto f = MakeFilter(ctx.get(), *plan, rows, 9);
    EXPECT_EQ(Drain(*f, /*wantRids=*/false).ids, OracleGE(rows, 50));
}

TEST(FilterTest, RidsStayAlignedWithTuples) {
    auto rows = MakeRows(600);
    auto plan = adapter::PredGE(50);
    for (size_t outBatch : {1u, 3u, 7u, 64u}) {
        auto ctx = adapter::MakeCtx(outBatch);
        auto f = MakeFilter(ctx.get(), *plan, rows, 5);
        auto got = Drain(*f, /*wantRids=*/true);
        EXPECT_FALSE(got.ridMisaligned) << "outBatch=" << outBatch;
        EXPECT_EQ(got.ridInts, got.ids) << "RID != row id at some position, outBatch=" << outBatch;
        EXPECT_EQ(got.ids, OracleGE(rows, 50));
    }
}

TEST(FilterTest, OutputsAreClearedOnEveryCall) {
    auto rows = MakeRows(50);
    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(3);
    auto f = MakeFilter(ctx.get(), *plan, rows, 4);
    f->Init();
    std::vector<storage::Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::vector<storage::RID>{};
    size_t total = 0;
    while (f->Next(&batch, rids)) {
        total += batch.size();
        EXPECT_EQ(batch.size(), rids->size());
    }
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(rids->empty());
    EXPECT_EQ(total, OracleGE(rows, 50).size());
}

TEST(FilterTest, NextAfterExhaustionKeepsReturningFalse) {
    auto rows = MakeRows(40);
    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(3);
    auto f = MakeFilter(ctx.get(), *plan, rows, 5);
    Drain(*f, false);
    for (int i = 0; i < 3; ++i) {
        std::vector<storage::Tuple> b;
        std::optional<std::vector<storage::RID>> r;
        EXPECT_FALSE(f->Next(&b, r));
        EXPECT_TRUE(b.empty());
    }
}

TEST(FilterTest, InitMidScanThenRerunMatchesFreshRun) {
    auto rows = MakeRows(300);
    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(5);
    auto f = MakeFilter(ctx.get(), *plan, rows, 64);
    f->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> r;
    ASSERT_TRUE(f->Next(&b, r));

    auto second = Drain(*f, false);  // Drain calls Init()
    EXPECT_EQ(second.ids, OracleGE(rows, 50));
}

TEST(FilterTest, SelectivityExtremesAndEmptyChild) {
    auto ctx = adapter::MakeCtx(4);
    {
        auto rows = MakeRows(100);
        auto none = adapter::PredGE(1000);
        EXPECT_TRUE(Drain(*MakeFilter(ctx.get(), *none, rows, 7), false).ids.empty());
    }
    {
        auto rows = MakeRows(100);
        auto all = adapter::PredGE(0);
        EXPECT_EQ(Drain(*MakeFilter(ctx.get(), *all, rows, 7), false).ids, OracleGE(rows, 0));
    }
    {
        std::vector<Row> empty;
        auto p = adapter::PredGE(0);
        EXPECT_TRUE(Drain(*MakeFilter(ctx.get(), *p, empty, 7), false).ids.empty());
    }
}

// TIGHTENED. The old version used EXPECT_ANY_THROW and kept passing when an
// unrelated exception (the CANARY run) was thrown. Now: it must be a
// std::logic_error, the message must name RIDs, and no rows may escape first.
TEST(FilterTest, ChildThatIgnoresRidRequestIsRejected) {
    auto rows = MakeRows(50);
    auto plan = adapter::PredGE(50);
    auto ctx = adapter::MakeCtx(4);
    auto f = MakeFilter(ctx.get(), *plan, rows, 5, /*honourRids=*/false);
    f->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> r = std::vector<storage::RID>{};
    try {
        f->Next(&b, r);
        FAIL() << "a child that returns no RIDs when asked must be rejected";
    } catch (const std::logic_error& e) {
        EXPECT_NE(std::string(e.what()).find("RID"), std::string::npos)
            << "message does not mention RIDs: " << e.what();
    }
    EXPECT_TRUE(b.empty()) << "rows escaped before the misalignment was detected";
    EXPECT_TRUE(!r.has_value() || r->empty());
}

// ---- Added tests -----------------------------------------------------------

static std::vector<Row> RowsWithSingleMatchAt(int n, int pos) {
    std::vector<Row> rows;
    for (int i = 0; i < n; ++i)
        rows.push_back({i, i == pos ? 60 : 0});  // only `pos` has v >= 50
    return rows;
}

// M15 (early stop). The sharpest probe for "a pull that passed nothing ended the
// stream": 499 rows fail, then exactly one passes, at the start, middle or end.
TEST(FilterTest, SingleMatchIsFoundWhereverItSits) {
    auto plan = adapter::PredGE(50);
    for (int pos : {0, 250, 499}) {
        auto rows = RowsWithSingleMatchAt(500, pos);
        for (size_t outBatch : {1u, 3u, 64u})
            for (size_t chunk : {1u, 7u, 64u, 500u}) {
                auto ctx = adapter::MakeCtx(outBatch);
                auto f = MakeFilter(ctx.get(), *plan, rows, chunk);
                EXPECT_EQ(Drain(*f, false).ids, std::vector<int32_t>{pos})
                    << "pos=" << pos << " outBatch=" << outBatch << " chunk=" << chunk;
            }
    }
}

// M16 (null predicate). A filter with no predicate passes EVERYTHING, NULL-valued
// rows included. The oracle is simply "all ids in order".
TEST(FilterTest, NoPredicatePassesEveryRow) {
    auto rows = MakeRows(300);
    std::vector<int32_t> all;
    for (auto& r : rows)
        all.push_back(r.id);

    auto plan = std::make_unique<optimizer::PhysicalFilter>();  // predicate left null
    for (size_t outBatch : {1u, 3u, 1024u}) {
        auto ctx = adapter::MakeCtx(outBatch);
        auto f = MakeFilter(ctx.get(), *plan, rows, 5);
        EXPECT_EQ(Drain(*f, false).ids, all) << "outBatch=" << outBatch;
    }
}

// M17 (global / shared state, nested pending logic). Filter over Filter, RIDs
// requested at both levels, against a plain-C++ oracle: 25 <= v < 75, NULL excluded.
TEST(FilterTest, TwoStackedFiltersMatchOracleWithAlignedRids) {
    auto rows = MakeRows(600);
    std::vector<int32_t> expected;
    for (auto& r : rows)
        if (r.v && *r.v >= 25 && *r.v < 75)
            expected.push_back(r.id);
    ASSERT_GT(expected.size(), 100u);

    auto ge = adapter::PredGE(25);
    auto lt = adapter::PredLT(75);
    for (size_t outBatch : {1u, 3u, 7u, 64u})
        for (size_t chunk : {2u, 5u, 13u}) {
            auto ctx = adapter::MakeCtx(outBatch);
            auto src =
                std::make_unique<ScriptedSource>(ctx.get(), adapter::TwoColSchema(), rows, chunk);
            auto inner = std::make_unique<FilterExecutor>(ctx.get(), *ge, std::move(src));
            FilterExecutor outer(ctx.get(), *lt, std::move(inner));
            auto got = Drain(outer, /*wantRids=*/true);
            EXPECT_EQ(got.ids, expected) << "outBatch=" << outBatch << " chunk=" << chunk;
            EXPECT_FALSE(got.ridMisaligned);
            EXPECT_EQ(got.ridInts, got.ids) << "outBatch=" << outBatch << " chunk=" << chunk;
        }
}

// M17 again, from the other side: two filter instances advanced alternately must
// not share leftover state (the old queue lived at namespace scope).
TEST(FilterTest, InterleavedInstancesDoNotShareState) {
    auto rows = MakeRows(400);
    auto geP = adapter::PredGE(50);
    auto ltP = adapter::PredLT(50);
    auto ctx = adapter::MakeCtx(3);
    auto a = MakeFilter(ctx.get(), *geP, rows, 5);
    auto b = MakeFilter(ctx.get(), *ltP, rows, 5);
    a->Init();
    b->Init();

    std::vector<int32_t> ga, gb;
    bool doneA = false, doneB = false;
    auto pull = [&](FilterExecutor& f, std::vector<int32_t>& out, bool& done) {
        if (done)
            return;
        std::vector<storage::Tuple> t;
        std::optional<std::vector<storage::RID>> none;
        if (!f.Next(&t, none)) {
            done = true;
            return;
        }
        for (auto& x : t)
            out.push_back(*adapter::DecodeInt(x, f.GetOutputSchema(), 0));
    };
    for (int guard = 0; (!doneA || !doneB) && guard < 100000; ++guard) {
        pull(*a, ga, doneA);
        pull(*b, gb, doneB);
    }
    EXPECT_EQ(ga, OracleGE(rows, 50));
    EXPECT_EQ(gb, OracleLT(rows, 50));
}

// Policy test: switching from "no RIDs" to "RIDs" while rows are still pending
// would misalign tuples and RIDs, so it must be rejected, not silently tolerated.
// (If you choose a different policy, change this test and document the policy.)
TEST(FilterTest, SwitchingToRidsMidBatchIsRejected) {
    auto rows = MakeRows(200);
    auto plan =
        adapter::PredGE(0);  // passes every non-NULL row: output fills, remainder stays pending
    auto ctx = adapter::MakeCtx(4);
    auto f = MakeFilter(ctx.get(), *plan, rows, 64);
    f->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> none;
    ASSERT_TRUE(f->Next(&b, none));
    ASSERT_EQ(b.size(), 4u);  // 60 child rows are still pending inside the filter

    std::optional<std::vector<storage::RID>> rids = std::vector<storage::RID>{};
    EXPECT_THROW(f->Next(&b, rids), std::logic_error);
}

// ============================================================================
// PROJECTION  (over ScriptedSource)
// ============================================================================
static std::unique_ptr<ProjectionExecutor> MakeProj(ExecutorContext* ctx,
                                                    const optimizer::PhysicalProject& plan,
                                                    const std::vector<Row>& rows, size_t chunk) {
    auto src = std::make_unique<ScriptedSource>(ctx, adapter::TwoColSchema(), rows, chunk);
    return std::make_unique<ProjectionExecutor>(ctx, plan, std::move(src));
}

TEST(ProjectionTest, OutputSchemaMatchesProjectionList) {
    auto plan = adapter::ProjIdVIdPlus1000();
    auto ctx = adapter::MakeCtx(8);
    auto p = MakeProj(ctx.get(), *plan, MakeRows(5), 3);
    auto expected = adapter::ProjOutSchema();
    ASSERT_EQ(p->GetOutputSchema().ColumnCount(), expected.ColumnCount());
    for (size_t i = 0; i < expected.ColumnCount(); ++i) {
        EXPECT_EQ(p->GetOutputSchema().columns[i].name, expected.columns[i].name);
        EXPECT_EQ(p->GetOutputSchema().columns[i].type, expected.columns[i].type);
    }
}

TEST(ProjectionTest, ComputedExpressionColumn) {
    auto rows = MakeRows(120);
    auto plan = adapter::ProjIdVIdPlus1000();
    for (size_t chunk : {1u, 7u, 1024u}) {
        auto ctx = adapter::MakeCtx(8);
        auto p = MakeProj(ctx.get(), *plan, rows, chunk);
        p->Init();
        const auto& s = p->GetOutputSchema();
        size_t i = 0;
        std::vector<storage::Tuple> b;
        std::optional<std::vector<storage::RID>> none;
        while (p->Next(&b, none)) {
            for (auto& t : b) {
                ASSERT_LT(i, rows.size());
                EXPECT_EQ(adapter::DecodeInt(t, s, 0), std::optional<int32_t>(rows[i].id));
                EXPECT_EQ(adapter::DecodeInt(t, s, 1), rows[i].v);
                EXPECT_EQ(adapter::DecodeInt(t, s, 2), std::optional<int32_t>(rows[i].id + 1000));
                ++i;
            }
        }
        EXPECT_EQ(i, rows.size()) << "chunk=" << chunk;
    }
}

TEST(ProjectionTest, NullsSurviveProjection) {
    std::vector<Row> rows;
    for (int i = 0; i < 30; ++i)
        rows.push_back({i, std::nullopt});
    auto plan = adapter::ProjIdVIdPlus1000();
    auto ctx = adapter::MakeCtx(4);
    auto p = MakeProj(ctx.get(), *plan, rows, 7);
    p->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> none;
    size_t seen = 0;
    while (p->Next(&b, none))
        for (auto& t : b) {
            EXPECT_FALSE(adapter::DecodeInt(t, p->GetOutputSchema(), 1).has_value());
            ++seen;
        }
    EXPECT_EQ(seen, rows.size());
}

// Weak by design: Projection ignores RID requests, so this only guards against
// a PARTIAL RID list. Policy (throw or leave empty) is up to the implementation.
TEST(ProjectionTest, RequestingRidsNeverReturnsMisalignedRids) {
    auto plan = adapter::ProjIdVIdPlus1000();
    auto ctx = adapter::MakeCtx(8);
    auto p = MakeProj(ctx.get(), *plan, MakeRows(20), 5);
    p->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> r = std::vector<storage::RID>{};
    try {
        while (p->Next(&b, r))
            EXPECT_TRUE(r->empty() || r->size() == b.size())
                << "partial RID list is worse than none";
    } catch (const std::exception&) {
        SUCCEED() << "documented policy: reject";
    }
}

// An empty child must end the stream immediately and stay ended.
TEST(ProjectionTest, EmptyChildTerminatesAndStaysTerminated) {
    auto plan = adapter::ProjIdVIdPlus1000();
    auto ctx = adapter::MakeCtx(8);
    auto p = MakeProj(ctx.get(), *plan, {}, 5);
    p->Init();
    for (int i = 0; i < 3; ++i) {
        std::vector<storage::Tuple> b;
        std::optional<std::vector<storage::RID>> none;
        EXPECT_FALSE(p->Next(&b, none));
        EXPECT_TRUE(b.empty());
    }
}

// ============================================================================
// REAL-HEAP TESTS: SeqScan and the full pipeline
// ============================================================================
static std::unique_ptr<SeqScanExecutor> MakeScan(adapter::TestDb& db,
                                                 const optimizer::PhysicalSeqScan& plan) {
    return std::make_unique<SeqScanExecutor>(db.ctx.get(), plan);
}

static std::optional<int32_t> DecodeId(const storage::Tuple& t, const storage::Schema& s) {
    auto vals = storage::Tuple::Deserialize(s, t.Span());
    if (vals[0].IsNull())
        return std::nullopt;
    return vals[0].AsInt32();
}
static std::optional<int32_t> DecodeV(const storage::Tuple& t, const storage::Schema& s) {
    auto vals = storage::Tuple::Deserialize(s, t.Span());
    if (vals[1].IsNull())
        return std::nullopt;
    return vals[1].AsInt32();
}

static Drained DrainScan(SeqScanExecutor& scan, bool wantRids) {
    Drained d;
    scan.Init();
    const auto& schema = scan.GetOutputSchema();
    for (int guard = 0; guard < 1'000'000; ++guard) {
        std::vector<storage::Tuple> batch;
        std::optional<std::vector<storage::RID>> rids;
        if (wantRids)
            rids = std::vector<storage::RID>{};
        if (!scan.Next(&batch, rids)) {
            EXPECT_TRUE(batch.empty());
            break;
        }
        ++d.calls;
        d.maxBatch = std::max(d.maxBatch, batch.size());
        if (wantRids) {
            if (!rids.has_value() || rids->size() != batch.size())
                d.ridMisaligned = true;
            else
                for (auto& r : *rids) {
                    d.ridInts.push_back(adapter::RidToInt(r));
                    d.rids.push_back(r);
                }
        }
        for (auto& t : batch)
            d.ids.push_back(*DecodeId(t, schema));
    }
    return d;
}

// Every returned RID must be the heap's own RID for that row: page AND slot.
// db.rids[id] is where row `id` was inserted (dataset index == id).
static void ExpectRidsAreTheRowsOwn(const adapter::TestDb& db, const Drained& got,
                                    const std::string& what) {
    ASSERT_FALSE(got.ridMisaligned) << what;
    ASSERT_EQ(got.rids.size(), got.ids.size()) << what;
    for (size_t i = 0; i < got.ids.size(); ++i) {
        const auto& want = db.rids[static_cast<size_t>(got.ids[i])];
        EXPECT_TRUE(adapter::RidEq(want, got.rids[i]))
            << what << ": row id " << got.ids[i] << " came back with page=" << got.rids[i].page_id
            << " slot=" << got.rids[i].slot_idx << ", heap says page=" << want.page_id
            << " slot=" << want.slot_idx;
    }
}

TEST(SeqScanTest, ReturnsEveryLiveRowInRidOrderUnderEviction) {
    auto rows = MakeRows(2000);
    auto db = adapter::BuildDb(rows, /*poolFrames=*/4);
    ASSERT_GE(adapter::TablePageCount(*db), 40u) << "fixture too small to force eviction";
    for (size_t batch : {1u, 3u, 7u, 1024u}) {
        adapter::Rebatch(*db, batch);
        auto plan = adapter::ScanPlan(*db);
        auto scan = MakeScan(*db, *plan);
        auto got = DrainScan(*scan, /*wantRids=*/true);
        std::vector<int32_t> allIds;
        for (auto& r : rows)
            allIds.push_back(r.id);
        EXPECT_EQ(got.ids, allIds) << "batch=" << batch;
        ExpectRidsAreTheRowsOwn(*db, got, "scan batch=" + std::to_string(batch));
    }
}

TEST(SeqScanTest, SkipsTombstones) {
    auto rows = MakeRows(1500);
    auto db = adapter::BuildDb(rows, 4);
    std::vector<size_t> del;
    std::set<int32_t> deleted;
    for (size_t i = 0; i < rows.size(); i += 3) {
        del.push_back(i);
        deleted.insert(rows[i].id);
    }
    adapter::DeleteRows(*db, del);

    std::vector<int32_t> expected;
    for (auto& r : rows)
        if (!deleted.count(r.id))
            expected.push_back(r.id);

    for (size_t batch : {1u, 5u, 1024u}) {
        adapter::Rebatch(*db, batch);
        auto plan = adapter::ScanPlan(*db);
        auto scan = MakeScan(*db, *plan);
        auto got = DrainScan(*scan, /*wantRids=*/true);
        EXPECT_EQ(got.ids, expected) << "batch=" << batch;
        ExpectRidsAreTheRowsOwn(*db, got, "tombstones batch=" + std::to_string(batch));
    }
}

TEST(SeqScanTest, EmptyTableTerminatesImmediately) {
    auto db = adapter::BuildDb({}, 4);
    adapter::Rebatch(*db, 8);
    auto plan = adapter::ScanPlan(*db);
    auto scan = MakeScan(*db, *plan);
    scan->Init();
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> r;
    EXPECT_FALSE(scan->Next(&b, r));
    EXPECT_TRUE(b.empty());
    EXPECT_FALSE(scan->Next(&b, r));
}

TEST(SeqScanTest, ReinitRestartsTheScan) {
    auto rows = MakeRows(300);
    auto db = adapter::BuildDb(rows, 4);
    adapter::Rebatch(*db, 7);
    auto plan = adapter::ScanPlan(*db);
    auto scan = MakeScan(*db, *plan);
    auto first = DrainScan(*scan, false);
    auto second = DrainScan(*scan, false);
    EXPECT_EQ(first.ids.size(), rows.size());
    EXPECT_EQ(second.ids, first.ids);
}

TEST(SeqScanTest, TuplesOwnTheirBytesAfterEviction) {
    auto rows = MakeRows(2000);
    auto db = adapter::BuildDb(rows, 4);
    ASSERT_GE(adapter::TablePageCount(*db), 40u);
    adapter::Rebatch(*db, 16);
    auto plan = adapter::ScanPlan(*db);
    auto scan = MakeScan(*db, *plan);
    scan->Init();
    std::vector<storage::Tuple> kept;
    std::vector<storage::Tuple> b;
    std::optional<std::vector<storage::RID>> none;
    while (scan->Next(&b, none))
        for (auto& t : b)
            kept.push_back(std::move(t));

    ASSERT_EQ(kept.size(), rows.size());
    const auto& s = scan->GetOutputSchema();
    for (size_t i = 0; i < kept.size(); ++i) {
        EXPECT_EQ(DecodeId(kept[i], s), std::optional<int32_t>(rows[i].id))
            << "row " << i << " was overwritten after its page was evicted";
        EXPECT_EQ(DecodeV(kept[i], s), rows[i].v);
    }
}

// ---- Shared plan builders for the 3-column real table -----------------------
static std::unique_ptr<optimizer::PhysicalFilter> RealFilterPlan(const adapter::TestDb& db) {
    auto pred = adapter::MakeBinaryOp(parser::ast::BinaryOperator::GREATER_EQUAL,
                                      adapter::MakeColRef(1, storage::ColumnType::INT32),
                                      adapter::MakeLitInt32(50));
    auto plan = std::make_unique<optimizer::PhysicalFilter>();
    plan->predicate = std::move(pred);
    for (size_t i = 0; i < db.schema.ColumnCount(); ++i) {
        optimizer::OutputColumn oc;
        oc.name = db.schema.columns[i].name;
        oc.type = db.schema.columns[i].type;
        oc.tableId = 0;
        oc.columnIndex = static_cast<uint32_t>(i);
        plan->outputSchema.push_back(std::move(oc));
    }
    return plan;
}

static std::unique_ptr<optimizer::PhysicalProject> RealProjPlan() {
    auto plan = std::make_unique<optimizer::PhysicalProject>();
    {
        optimizer::ProjectColumn pc;
        pc.name = "id";
        pc.expr = adapter::MakeColRef(0, storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }
    {
        optimizer::ProjectColumn pc;
        pc.name = "v";
        pc.expr = adapter::MakeColRef(1, storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }
    {
        optimizer::ProjectColumn pc;
        pc.name = "id_plus_1000";
        pc.expr = adapter::MakeBinaryOp(parser::ast::BinaryOperator::ADD,
                                        adapter::MakeColRef(0, storage::ColumnType::INT32),
                                        adapter::MakeLitInt32(1000), storage::ColumnType::INT32);
        plan->projections.push_back(std::move(pc));
    }
    plan->outputSchema = {
        optimizer::OutputColumn{"id", storage::ColumnType::INT32, 0, 0},
        optimizer::OutputColumn{"v", storage::ColumnType::INT32, 0, 1},
        optimizer::OutputColumn{"id_plus_1000", storage::ColumnType::INT32, 0, 0},
    };
    return plan;
}

// NEW. Scan -> Filter on the REAL heap with RIDs requested. This is the property
// a later UPDATE/DELETE depends on: each surviving tuple carries ITS OWN RID
// (page + slot). The scripted tests only simulate this; here many rows share a
// page, so a shifted or stale slot cannot hide behind page_id alone.
TEST(ExecutorPipelineP42, RealHeapScanFilterKeepsCorrectRids) {
    auto rows = MakeRows(2500);
    auto db = adapter::BuildDb(rows, /*poolFrames=*/4);
    ASSERT_GE(adapter::TablePageCount(*db), 40u);

    std::vector<size_t> del;
    std::set<int32_t> deleted;
    for (size_t i = 5; i < rows.size(); i += 11) {
        del.push_back(i);
        deleted.insert(rows[i].id);
    }
    adapter::DeleteRows(*db, del);

    std::vector<int32_t> expected;
    for (auto& r : rows)
        if (!deleted.count(r.id) && r.v && *r.v >= 50)
            expected.push_back(r.id);
    ASSERT_GT(expected.size(), 500u);

    for (size_t batch : {1u, 3u, 7u, 100u, 1024u}) {
        adapter::Rebatch(*db, batch);
        auto scanPlan = adapter::ScanPlan(*db);
        auto filterPlan = RealFilterPlan(*db);
        auto scan = std::make_unique<SeqScanExecutor>(db->ctx.get(), *scanPlan);
        FilterExecutor filter(db->ctx.get(), *filterPlan, std::move(scan));

        auto got = Drain(filter, /*wantRids=*/true);
        EXPECT_EQ(got.ids, expected) << "batch=" << batch;
        ExpectRidsAreTheRowsOwn(*db, got, "scan+filter batch=" + std::to_string(batch));
    }
}

// ---- Milestone: Scan -> Filter -> Projection on real files ------------------
TEST(ExecutorMilestoneP42, ScanFilterProjectMatchesOracleFromDisk) {
    auto rows = MakeRows(2500);
    auto db = adapter::BuildDb(rows, /*poolFrames=*/4);
    ASSERT_GE(adapter::TablePageCount(*db), 40u);

    std::vector<size_t> del;
    std::set<int32_t> deleted;
    for (size_t i = 5; i < rows.size(); i += 11) {
        del.push_back(i);
        deleted.insert(rows[i].id);
    }
    adapter::DeleteRows(*db, del);

    std::vector<int32_t> expected;
    for (auto& r : rows)
        if (!deleted.count(r.id) && r.v && *r.v >= 50)
            expected.push_back(r.id);
    ASSERT_GT(expected.size(), 500u);

    storage::Schema projSchema = adapter::ProjOutSchema();

    for (size_t batch : {1u, 3u, 7u, 100u, 1024u}) {
        adapter::Rebatch(*db, batch);
        auto scanPlan = adapter::ScanPlan(*db);
        auto filterPlan = RealFilterPlan(*db);
        auto projPlan = RealProjPlan();

        auto scan = std::make_unique<SeqScanExecutor>(db->ctx.get(), *scanPlan);
        auto filter = std::make_unique<FilterExecutor>(db->ctx.get(), *filterPlan, std::move(scan));
        ProjectionExecutor proj(db->ctx.get(), *projPlan, std::move(filter));

        proj.Init();
        std::vector<int32_t> gotIds, gotPlus;
        std::vector<storage::Tuple> b;
        std::optional<std::vector<storage::RID>> none;
        while (proj.Next(&b, none))
            for (auto& t : b) {
                auto vals = storage::Tuple::Deserialize(projSchema, t.Span());
                gotIds.push_back(vals[0].AsInt32());
                gotPlus.push_back(vals[2].AsInt32());
            }
        EXPECT_EQ(gotIds, expected) << "batch=" << batch;
        for (size_t i = 0; i < gotIds.size(); ++i)
            ASSERT_EQ(gotPlus[i], gotIds[i] + 1000);
    }
}

// SQL text -> Parser -> Binder -> Planner -> physical -> executors -> rows.
// FIX: DISABLED_ tests are still compiled and linked, so the undefined RunSql
// broke the link. The whole block is now behind a macro. Define
// AXOLOT_HAS_RUNSQL (and implement adapter::RunSql) once the SQL front end is
// wired to the executors.
#ifdef AXOLOT_HAS_RUNSQL
namespace adapter {
std::vector<std::vector<std::optional<int32_t>>> RunSql(TestDb& db, const std::string& sql);
}
TEST(ExecutorMilestoneP42, SelectWithWhereReturnsCorrectRowsFromDisk) {
    auto rows = MakeRows(2000);
    auto db = adapter::BuildDb(rows, 4);
    for (size_t batch : {1u, 7u, 1024u}) {
        adapter::Rebatch(*db, batch);
        auto result = adapter::RunSql(*db, "SELECT id, v FROM t WHERE v >= 50;");
        auto expected = OracleGE(rows, 50);
        ASSERT_EQ(result.size(), expected.size()) << "batch=" << batch;
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(result[i][0], std::optional<int32_t>(expected[i]));
            EXPECT_EQ(result[i][1], rows[expected[i]].v);
        }
    }
}
#endif  // AXOLOT_HAS_RUNSQL