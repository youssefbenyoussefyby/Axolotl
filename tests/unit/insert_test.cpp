// P4.3 -- InsertExecutor.
//
// Every test drives real SQL through Parser -> Binder -> Planner -> physical
// plan -> CreateExecutor, so the factory wiring and the binder's INSERT checks
// are exercised too. Contract pinned here (see docs/phases/P4.md §4.3):
//
//  * INSERT ... VALUES has no child; the rows live in PhysicalInsert::rows.
//  * Output: exactly one row, schema (rows_affected INT64), then exhausted.
//  * Side effects happen in Next(), never in Init().
//  * All-or-nothing against bad input: every row is evaluated, checked and
//    serialized before the first TableHeap::Insert, so a failing row leaves
//    the table untouched.
#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <gtest/gtest.h>
#include <optimizer/physical_plan.h>
#include <optimizer/planner.h>
#include <parser/binder.h>
#include <parser/parser.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace AxolotlDB;
using storage::ColumnDef;
using storage::ColumnType;
using storage::Schema;
using storage::Tuple;
using storage::Value;

namespace {

// Keeps the physical plan alive as long as the executor: executors hold the
// plan node by reference.
struct Query {
    optimizer::PhysicalNodePtr plan;
    std::unique_ptr<executor::AbstractExecutor> exec;
};

}  // namespace

class InsertTest : public ::testing::Test {
   protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;
    std::unique_ptr<storage::StorageManager> sm_;
    std::unique_ptr<catalog::Catalog> cat_;
    std::unique_ptr<executor::ExecutorContext> ctx_;

    // Two-argument ColumnDefs are NOT NULL.
    Schema users_{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR},
                   ColumnDef{"age", ColumnType::INT32}}};
    Schema items_{{ColumnDef{"id", ColumnType::INT32, false},
                   ColumnDef{"note", ColumnType::VARCHAR, true},
                   ColumnDef{"qty", ColumnType::INT64, true}}};

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("axo_insert_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);
        Open(/*batch=*/1024);
        cat_->CreateTable("users", users_);
        cat_->CreateTable("items", items_);
    }

    void TearDown() override {
        Close();
        std::error_code ec;
        std::filesystem::current_path(orig_dir_, ec);
        std::filesystem::remove_all(dir_, ec);
    }

    void Open(std::size_t batch) {
        sm_ = std::make_unique<storage::StorageManager>(8);
        cat_ = std::make_unique<catalog::Catalog>(*sm_);
        ctx_ = std::make_unique<executor::ExecutorContext>(*sm_, *cat_, nullptr, batch);
    }

    void Close() {
        ctx_.reset();
        cat_.reset();
        sm_.reset();
    }

    Query Prepare(const std::string& sql) {
        utils::DiagnosticSink sink;
        parser::Parser p(sql, sink);
        auto stmt = p.Parse();
        binder::Binder b(*cat_, &sink);
        auto bound = b.Bind(stmt);
        optimizer::Planner planner;
        Query q;
        q.plan = optimizer::ToPhysicalPlan(planner.Plan(*bound));
        q.exec = executor::CreateExecutor(*q.plan, ctx_.get());
        return q;
    }

    // Runs a statement to completion and returns every output row, decoded.
    std::vector<std::vector<Value>> Run(const std::string& sql) {
        Query q = Prepare(sql);
        q.exec->Init();
        std::vector<std::vector<Value>> rows;
        std::vector<Tuple> batch;
        std::optional<std::vector<storage::RID>> rids = std::nullopt;
        while (q.exec->Next(&batch, rids)) {
            for (const auto& t : batch) {
                rows.push_back(Tuple::Deserialize(q.exec->GetOutputSchema(), t.Span()));
            }
        }
        return rows;
    }

    int64_t InsertCount(const std::string& sql) {
        auto rows = Run(sql);
        EXPECT_EQ(rows.size(), 1u) << "INSERT must emit exactly one rows_affected row";
        return rows.empty() ? -1 : rows[0][0].AsInt64();
    }
};

// ---- P4.md gate tests --------------------------------------------------------

TEST_F(InsertTest, ReportsAffectedRowCount) {
    Query q = Prepare("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);");

    const auto& schema = q.exec->GetOutputSchema();  // available before Init()
    ASSERT_EQ(schema.ColumnCount(), 1u);
    EXPECT_EQ(schema.columns[0].name, "rows_affected");
    EXPECT_EQ(schema.columns[0].type, ColumnType::INT64);

    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(Tuple::Deserialize(schema, batch[0].Span())[0].AsInt64(), 3);

    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_TRUE(batch.empty()) << "Next() must clear the batch even when exhausted";
}

TEST_F(InsertTest, InsertedRowsAreVisibleToASubsequentScan) {
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1, 'ann', 31), (2, 'bob', 42);"), 2);

    auto rows = Run("SELECT id, name, age FROM users;");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].AsInt32(), 1);
    EXPECT_EQ(rows[0][1].AsVarchar(), "ann");
    EXPECT_EQ(rows[0][2].AsInt32(), 31);
    EXPECT_EQ(rows[1][0].AsInt32(), 2);
    EXPECT_EQ(rows[1][1].AsVarchar(), "bob");
    EXPECT_EQ(rows[1][2].AsInt32(), 42);
}

// ---- column mapping ------------------------------------------------------------

TEST_F(InsertTest, ExplicitColumnListMapsByColumnIndexAndFillsTheRestWithNull) {
    ASSERT_EQ(InsertCount("INSERT INTO items (qty, id) VALUES (5, 1);"), 1);

    auto rows = Run("SELECT id, note, qty FROM items;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 1);
    EXPECT_TRUE(rows[0][1].IsNull()) << "unlisted nullable column must be NULL";
    EXPECT_EQ(rows[0][2].AsInt64(), 5);
}

TEST_F(InsertTest, ExplicitNullIsStoredAsNull) {
    ASSERT_EQ(InsertCount("INSERT INTO items VALUES (1, NULL, NULL);"), 1);
    auto rows = Run("SELECT note, qty FROM items;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0][0].IsNull());
    EXPECT_TRUE(rows[0][1].IsNull());
}

// ---- types -------------------------------------------------------------------

TEST_F(InsertTest, NegativeLiteralRoundTrips) {
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (-7, 'neg', 0);"), 1);
    auto rows = Run("SELECT id FROM users;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), -7);
}

// The binder leaves `1 + 2` as an INT64 expression (it only checks the type
// family), so the executor must convert the evaluated Value to the column's
// width before Tuple::Serialize.
TEST_F(InsertTest, NumericExpressionIsConvertedToTheColumnWidth) {
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1 + 2, 'sum', 4);"), 1);
    auto rows = Run("SELECT id FROM users;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 3);
}

// ---- Volcano behaviour -----------------------------------------------------------

TEST_F(InsertTest, InitAloneInsertsNothing) {
    Query q = Prepare("INSERT INTO users VALUES (1, 'a', 1);");
    q.exec->Init();  // and never pull
    EXPECT_TRUE(Run("SELECT id FROM users;").empty());
}

TEST_F(InsertTest, CallingNextAfterExhaustionDoesNotInsertAgain) {
    Query q = Prepare("INSERT INTO users VALUES (1, 'a', 1);");
    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_EQ(Run("SELECT id FROM users;").size(), 1u);
}

TEST_F(InsertTest, RowCountIsIndependentOfBatchSize) {
    Close();
    Open(/*batch=*/2);
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1, 'a', 1), (2, 'b', 2), (3, 'c', 3), "
                          "(4, 'd', 4), (5, 'e', 5);"),
              5);
    EXPECT_EQ(Run("SELECT id FROM users;").size(), 5u);
}

// ---- all-or-nothing against bad input ------------------------------------------

// The second row is too large for any page (TableHeap throws
// ValueTooLargeException). The first row must not have been written.
TEST_F(InsertTest, OversizedRowLeavesTheTableUntouched) {
    // A successful insert first, so this test cannot pass on an executor that
    // simply throws.
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1, 'kept', 1);"), 1);
    const std::string huge(6000, 'x');
    EXPECT_ANY_THROW((void)Run("INSERT INTO users VALUES (2, 'ok', 2), (3, '" + huge + "', 3);"));
    EXPECT_EQ(Run("SELECT id FROM users;").size(), 1u) << "row 2 must not have been written";
}

// `NULL + 1` is only known to be NULL at execution time, so the executor must
// enforce NOT NULL itself; the binder cannot.
TEST_F(InsertTest, ComputedNullIntoNotNullColumnThrowsAndInsertsNothing) {
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1, 'kept', 1);"), 1);
    EXPECT_ANY_THROW((void)Run("INSERT INTO users VALUES (2, 'ok', 2), (NULL + 1, 'b', 3);"));
    EXPECT_EQ(Run("SELECT id FROM users;").size(), 1u) << "row 2 must not have been written";
}

// ---- durability (as far as P4 goes: flushed pages, no WAL) --------------------

TEST_F(InsertTest, InsertedRowsSurviveFlushAndReopen) {
    ASSERT_EQ(InsertCount("INSERT INTO users VALUES (1, 'a', 1), (2, 'b', 2);"), 2);
    sm_->FlushAll();
    Close();
    Open(/*batch=*/1024);
    EXPECT_EQ(Run("SELECT id FROM users;").size(), 2u);
}

// ---- type checks inside the executor -------------------------------------------
//
// Through SQL, the binder already rejects a string literal for an INT column.
// These build the physical plan by hand so the executor's own check is what
// runs: a plan from any other producer (a future INSERT ... SELECT, a
// rewrite rule, a test) must not be able to store the wrong type.

namespace {

parser::ast::BoundExprPtr Lit(storage::ColumnType t,
                              std::variant<int64_t, double, std::string, bool, std::nullptr_t> v) {
    return std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundLiteral(t, std::move(v)), t);
}

}  // namespace

class InsertTypeCheckTest : public InsertTest {
   protected:
    // users(id INT32, name VARCHAR, age INT32), all three listed.
    std::unique_ptr<optimizer::PhysicalInsert> UsersPlan() {
        auto p = std::make_unique<optimizer::PhysicalInsert>();
        p->tableName = "users";
        p->tableId = cat_->GetTableId("users");
        p->columnIndices = {0, 1, 2};
        p->targetColumns = users_.columns;
        return p;
    }

    static optimizer::InsertRow Row(parser::ast::BoundExprPtr a, parser::ast::BoundExprPtr b,
                                    parser::ast::BoundExprPtr c) {
        optimizer::InsertRow r;
        r.values.push_back(std::move(a));
        r.values.push_back(std::move(b));
        r.values.push_back(std::move(c));
        return r;
    }

    void ExpectRejected(optimizer::PhysicalInsert& plan) {
        auto exec = executor::CreateExecutor(plan, ctx_.get());
        exec->Init();
        std::vector<Tuple> batch;
        std::optional<std::vector<storage::RID>> rids = std::nullopt;
        EXPECT_THROW((void)exec->Next(&batch, rids), utils::TypeMismatchException);
        EXPECT_TRUE(Run("SELECT id FROM users;").empty()) << "nothing may be written";
    }
};

TEST_F(InsertTypeCheckTest, StringIntoIntColumnIsRejected) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::VARCHAR, std::string("seven")),
                             Lit(ColumnType::VARCHAR, std::string("a")),
                             Lit(ColumnType::INT32, int64_t{1})));
    ExpectRejected(*plan);
}

TEST_F(InsertTypeCheckTest, NumberIntoVarcharColumnIsRejected) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::INT32, int64_t{1}),
                             Lit(ColumnType::INT64, int64_t{42}),
                             Lit(ColumnType::INT32, int64_t{1})));
    ExpectRejected(*plan);
}

TEST_F(InsertTypeCheckTest, DecimalIntoIntColumnIsRejectedNotTruncated) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::FLOAT64, 2.9),
                             Lit(ColumnType::VARCHAR, std::string("a")),
                             Lit(ColumnType::INT32, int64_t{1})));
    ExpectRejected(*plan);
}

TEST_F(InsertTypeCheckTest, BadSecondRowLeavesTheFirstUnwritten) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::INT32, int64_t{1}),
                             Lit(ColumnType::VARCHAR, std::string("ok")),
                             Lit(ColumnType::INT32, int64_t{1})));
    plan->rows.push_back(Row(Lit(ColumnType::INT32, int64_t{2}), Lit(ColumnType::BOOLEAN, true),
                             Lit(ColumnType::INT32, int64_t{2})));
    ExpectRejected(*plan);
}

// An INT64-typed value (what an unconverted literal or `1 + 2` produces) is
// narrowed into an INT32 column, with a range check.
TEST_F(InsertTypeCheckTest, Int64ValueIsNarrowedIntoInt32Column) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::INT64, int64_t{9}),
                             Lit(ColumnType::VARCHAR, std::string("n")),
                             Lit(ColumnType::INT64, int64_t{30})));
    auto exec = executor::CreateExecutor(*plan, ctx_.get());
    exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    ASSERT_TRUE(exec->Next(&batch, rids));
    auto rows = Run("SELECT id, age FROM users;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 9);
    EXPECT_EQ(rows[0][1].AsInt32(), 30);
}

TEST_F(InsertTypeCheckTest, Int64OutOfInt32RangeIsRejected) {
    auto plan = UsersPlan();
    plan->rows.push_back(Row(Lit(ColumnType::INT64, int64_t{5000000000LL}),
                             Lit(ColumnType::VARCHAR, std::string("big")),
                             Lit(ColumnType::INT32, int64_t{1})));
    auto exec = executor::CreateExecutor(*plan, ctx_.get());
    exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    EXPECT_THROW((void)exec->Next(&batch, rids), utils::AxolotException);
    EXPECT_TRUE(Run("SELECT id FROM users;").empty());
}
