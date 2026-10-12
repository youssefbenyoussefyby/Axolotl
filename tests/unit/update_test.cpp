// P4.3 -- UpdateExecutor.
//
// Real SQL through Parser -> Binder -> Planner -> CreateExecutor. Contract
// pinned here (docs/phases/P4.md §4.3):
//
//  * One child (Scan, or Filter over Scan) that supplies the rows and RIDs.
//  * Output: one row, (rows_affected INT64), then exhausted.
//  * Every assignment is evaluated against the row as it was before the
//    statement (`SET a = b, b = a` swaps).
//  * The RID set is materialised before the first TableHeap::Update, so a row
//    the update moves is never visited twice (the Halloween problem).
//  * TableHeap::Update may move a row; it must stay visible exactly once.
//  * Bad input (wrong type, NULL into NOT NULL, INT32 overflow) throws before
//    anything is written.
#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <gtest/gtest.h>
#include <optimizer/physical_plan.h>
#include <optimizer/planner.h>
#include <parser/binder.h>
#include <parser/parser.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace AxolotlDB;
using storage::ColumnDef;
using storage::ColumnType;
using storage::RID;
using storage::Schema;
using storage::Tuple;
using storage::Value;

namespace {

struct Query {
    optimizer::PhysicalNodePtr plan;  // executors hold the plan by reference
    std::unique_ptr<executor::AbstractExecutor> exec;
};

struct ScanResult {
    std::vector<std::vector<Value>> rows;
    std::vector<RID> rids;
};

}  // namespace

class UpdateTest : public ::testing::Test {
   protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;
    std::unique_ptr<storage::StorageManager> sm_;
    std::unique_ptr<catalog::Catalog> cat_;
    std::unique_ptr<executor::ExecutorContext> ctx_;

    // Two-argument ColumnDefs are NOT NULL.
    Schema users_{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR},
                   ColumnDef{"age", ColumnType::INT32}}};
    Schema pairs_{{ColumnDef{"a", ColumnType::INT32}, ColumnDef{"b", ColumnType::INT32}}};

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("axo_update_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);
        Open(/*batch=*/1024);
        cat_->CreateTable("users", users_);
        cat_->CreateTable("pairs", pairs_);
    }

    void TearDown() override {
        Close();
        std::error_code ec;
        std::filesystem::current_path(orig_dir_, ec);
        std::filesystem::remove_all(dir_, ec);
    }

    void Open(std::size_t batch) {
        sm_ = std::make_unique<storage::StorageManager>(16);
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

    ScanResult Run(const std::string& sql, bool wantRids = false) {
        Query q = Prepare(sql);
        q.exec->Init();
        ScanResult out;
        std::vector<Tuple> batch;
        std::optional<std::vector<RID>> rids =
            wantRids ? std::optional<std::vector<RID>>(std::vector<RID>{}) : std::nullopt;
        while (q.exec->Next(&batch, rids)) {
            for (const auto& t : batch) {
                out.rows.push_back(Tuple::Deserialize(q.exec->GetOutputSchema(), t.Span()));
            }
            if (rids.has_value()) {
                out.rids.insert(out.rids.end(), rids->begin(), rids->end());
            }
        }
        return out;
    }

    int64_t Affected(const std::string& sql) {
        auto r = Run(sql);
        EXPECT_EQ(r.rows.size(), 1u) << "DML must emit exactly one rows_affected row";
        return r.rows.empty() ? -1 : r.rows[0][0].AsInt64();
    }

    // id -> age, read back through SQL.
    std::map<int32_t, int32_t> Ages() {
        std::map<int32_t, int32_t> m;
        for (const auto& row : Run("SELECT id, age FROM users;").rows) {
            EXPECT_TRUE(m.emplace(row[0].AsInt32(), row[1].AsInt32()).second)
                << "id " << row[0].AsInt32() << " is visible more than once";
        }
        return m;
    }

    // Every live row with its RID, read straight from the heap. SELECT cannot
    // be used for this: Projection does not pass RIDs through.
    ScanResult HeapRows() {
        ScanResult out;
        auto& heap = cat_->GetTableHeap("users");
        for (auto it = heap.begin(); it != heap.end(); ++it) {
            const RID rid = *it;
            storage::ReadPageGuard guard;
            auto bytes = heap.Get(rid, guard);
            out.rows.push_back(Tuple::Deserialize(users_, bytes));
            out.rids.push_back(rid);
        }
        return out;
    }

    void Seed(int n, std::size_t nameLen = 8) {
        std::string sql = "INSERT INTO users VALUES ";
        for (int i = 1; i <= n; ++i) {
            if (i > 1) {
                sql += ", ";
            }
            sql += "(" + std::to_string(i) + ", '" + std::string(nameLen, 'a') + "', " +
                   std::to_string(i * 10) + ")";
        }
        sql += ";";
        ASSERT_EQ(Affected(sql), n);
    }
};

// ---- output and basic effect ---------------------------------------------------

TEST_F(UpdateTest, ReportsAffectedRowCount) {
    Seed(5);
    Query q = Prepare("UPDATE users SET age = 0 WHERE id > 2;");
    const auto& schema = q.exec->GetOutputSchema();  // available before Init()
    ASSERT_EQ(schema.ColumnCount(), 1u);
    EXPECT_EQ(schema.columns[0].name, "rows_affected");
    EXPECT_EQ(schema.columns[0].type, ColumnType::INT64);

    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(Tuple::Deserialize(schema, batch[0].Span())[0].AsInt64(), 3);
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_TRUE(batch.empty());
}

TEST_F(UpdateTest, UpdatesOnlyMatchingRows) {
    Seed(3);
    ASSERT_EQ(Affected("UPDATE users SET age = 99 WHERE id = 2;"), 1);
    auto ages = Ages();
    EXPECT_EQ(ages[1], 10);
    EXPECT_EQ(ages[2], 99);
    EXPECT_EQ(ages[3], 30);
}

TEST_F(UpdateTest, WithoutWhereUpdatesEveryRow) {
    Seed(4);
    ASSERT_EQ(Affected("UPDATE users SET age = 1;"), 4);
    for (const auto& [id, age] : Ages()) {
        EXPECT_EQ(age, 1) << "id " << id;
    }
}

TEST_F(UpdateTest, MatchingNoRowsEmitsZeroAndChangesNothing) {
    Seed(3);
    EXPECT_EQ(Affected("UPDATE users SET age = 0 WHERE id > 100;"), 0);
    EXPECT_EQ(Ages()[3], 30);
}

TEST_F(UpdateTest, ExpressionReadsTheRowsOwnColumns) {
    Seed(3);
    ASSERT_EQ(Affected("UPDATE users SET age = age + 1;"), 3);
    auto ages = Ages();
    EXPECT_EQ(ages[1], 11);
    EXPECT_EQ(ages[2], 21);
    EXPECT_EQ(ages[3], 31);
}

TEST_F(UpdateTest, AssignmentsSeeTheRowAsItWasBeforeTheStatement) {
    ASSERT_EQ(Affected("INSERT INTO pairs VALUES (1, 2);"), 1);
    ASSERT_EQ(Affected("UPDATE pairs SET a = b, b = a;"), 1);
    auto rows = Run("SELECT a, b FROM pairs;").rows;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 2);
    EXPECT_EQ(rows[0][1].AsInt32(), 1) << "b must get the old a, not the new one";
}

TEST_F(UpdateTest, VarcharColumnCanBeUpdated) {
    Seed(2);
    ASSERT_EQ(Affected("UPDATE users SET name = 'renamed' WHERE id = 1;"), 1);
    auto rows = Run("SELECT id, name FROM users WHERE id = 1;").rows;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][1].AsVarchar(), "renamed");
}

// ---- P4.md gate: a moved row stays visible exactly once ----------------------

// Fill pages with mid-sized rows, then grow one row so it cannot stay in place:
// TableHeap::Update gives it a new RID. A scan afterwards must see it once.
TEST_F(UpdateTest, GrowingRowUpdatesRidAndStaysVisibleExactlyOnce) {
    Seed(30, /*nameLen=*/300);

    auto before = HeapRows();
    ASSERT_EQ(before.rids.size(), 30u);
    RID oldRid{};
    for (size_t i = 0; i < before.rows.size(); ++i) {
        if (before.rows[i][0].AsInt32() == 1) {
            oldRid = before.rids[i];
        }
    }

    const std::string big(2500, 'z');
    ASSERT_EQ(Affected("UPDATE users SET name = '" + big + "' WHERE id = 1;"), 1);

    auto after = HeapRows();
    ASSERT_EQ(after.rows.size(), 30u) << "no row may vanish or be duplicated";
    int seen = 0;
    for (size_t i = 0; i < after.rows.size(); ++i) {
        if (after.rows[i][0].AsInt32() == 1) {
            ++seen;
            EXPECT_EQ(after.rows[i][1].AsVarchar(), big);
            EXPECT_FALSE(after.rids[i] == oldRid)
                << "precondition: the grown row should have moved to a new RID";
        }
    }
    EXPECT_EQ(seen, 1);
}

// ---- P4.md gate: the Halloween problem ------------------------------------------

// Every row matches its own post-update predicate, and growing each row moves
// it to a later page, ahead of a live scan's cursor. A pipelined update would
// keep revisiting moved rows; materialising the RID set first makes each row
// change exactly once.
TEST_F(UpdateTest, UpdateMatchingItsOwnPredicateTerminates) {
    Seed(20, /*nameLen=*/300);
    const std::string grown(600, 'g');
    ASSERT_EQ(Affected("UPDATE users SET age = age + 1, name = '" + grown + "' WHERE age > 0;"),
              20);
    auto ages = Ages();
    ASSERT_EQ(ages.size(), 20u);
    for (const auto& [id, age] : ages) {
        EXPECT_EQ(age, id * 10 + 1) << "id " << id << " must be incremented exactly once";
    }
}

// ---- bad input: throw, write nothing --------------------------------------------

TEST_F(UpdateTest, StringIntoIntColumnThrowsAndUpdatesNothing) {
    Seed(3);
    EXPECT_THROW((void)Run("UPDATE users SET age = 'old';"), utils::TypeMismatchException);
    EXPECT_EQ(Ages()[1], 10);
}

TEST_F(UpdateTest, NumberIntoVarcharColumnThrowsAndUpdatesNothing) {
    Seed(2);
    EXPECT_THROW((void)Run("UPDATE users SET name = 5;"), utils::TypeMismatchException);
    auto rows = Run("SELECT name FROM users;").rows;
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].AsVarchar(), "aaaaaaaa");
}

TEST_F(UpdateTest, DecimalIntoIntColumnThrowsRatherThanTruncating) {
    Seed(1);
    EXPECT_THROW((void)Run("UPDATE users SET age = 2.5;"), utils::TypeMismatchException);
    EXPECT_EQ(Ages()[1], 10);
}

TEST_F(UpdateTest, NullIntoNotNullColumnThrowsAndUpdatesNothing) {
    Seed(3);
    EXPECT_ANY_THROW((void)Run("UPDATE users SET age = NULL;"));
    EXPECT_EQ(Ages()[2], 20);
}

TEST_F(UpdateTest, Int32OverflowThrowsAndUpdatesNothing) {
    Seed(2);
    EXPECT_ANY_THROW((void)Run("UPDATE users SET age = 3000000000;"));
    EXPECT_EQ(Ages()[1], 10);
}

// A failing row late in the scan must not leave earlier rows updated.
TEST_F(UpdateTest, FailureOnALaterRowLeavesEarlierRowsUnchanged) {
    Seed(4);
    // age * 100000000 overflows INT32 only for ids 3 and 4 (30e8, 40e8).
    EXPECT_ANY_THROW((void)Run("UPDATE users SET age = age * 100000000;"));
    auto ages = Ages();
    EXPECT_EQ(ages[1], 10);
    EXPECT_EQ(ages[2], 20);
}

// ---- Volcano behaviour and durability --------------------------------------------

TEST_F(UpdateTest, InitAloneUpdatesNothing) {
    Seed(2);
    Query q = Prepare("UPDATE users SET age = 0;");
    q.exec->Init();
    EXPECT_EQ(Ages()[1], 10);
}

TEST_F(UpdateTest, CallingNextAfterExhaustionDoesNotUpdateAgain) {
    Seed(2);
    Query q = Prepare("UPDATE users SET age = age + 1;");
    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_EQ(Ages()[1], 11);
}

TEST_F(UpdateTest, RowCountIsIndependentOfBatchSize) {
    Close();
    Open(/*batch=*/2);
    Seed(7);
    EXPECT_EQ(Affected("UPDATE users SET age = 5;"), 7);
    EXPECT_EQ(Ages().size(), 7u);
}

TEST_F(UpdateTest, UpdatesSurviveFlushAndReopen) {
    Seed(3);
    ASSERT_EQ(Affected("UPDATE users SET age = 77 WHERE id = 3;"), 1);
    sm_->FlushAll();
    Close();
    Open(/*batch=*/1024);
    EXPECT_EQ(Ages()[3], 77);
}
