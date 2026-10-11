// P4.3 -- DeleteExecutor.
//
// Drives real SQL through Parser -> Binder -> Planner -> physical plan ->
// CreateExecutor to verify DeleteExecutor end-to-end.
//
// Contract pinned here (see docs/phases/P4.md §4.3):
//  * DELETE pulls tuples and RIDs from its child (SeqScan or Filter over SeqScan).
//  * Output: exactly one row, schema (rows_affected INT64), then exhausted.
//  * Side effects happen in Next(), never in Init().
//  * Deleted rows disappear from subsequent scans immediately and after flush/reopen.
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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace AxolotlDB;
using storage::ColumnDef;
using storage::ColumnType;
using storage::Schema;
using storage::Tuple;
using storage::Value;

namespace {

struct Query {
    optimizer::PhysicalNodePtr plan;
    std::unique_ptr<executor::AbstractExecutor> exec;
};

}  // namespace

class DeleteTest : public ::testing::Test {
   protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;
    std::unique_ptr<storage::StorageManager> sm_;
    std::unique_ptr<catalog::Catalog> cat_;
    std::unique_ptr<executor::ExecutorContext> ctx_;

    Schema users_{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR},
                   ColumnDef{"age", ColumnType::INT32}}};

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("axo_delete_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);
        Open(/*batch=*/1024);
        cat_->CreateTable("users", users_);
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

    int64_t ExecuteDML(const std::string& sql) {
        auto rows = Run(sql);
        EXPECT_EQ(rows.size(), 1u) << "DML must emit exactly one rows_affected row";
        return rows.empty() ? -1 : rows[0][0].AsInt64();
    }
};

// ---- P4.md gate tests --------------------------------------------------------

TEST_F(DeleteTest, ReportsAffectedRowCount) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);"), 3);

    Query q = Prepare("DELETE FROM users WHERE age >= 20;");

    const auto& schema = q.exec->GetOutputSchema();
    ASSERT_EQ(schema.ColumnCount(), 1u);
    EXPECT_EQ(schema.columns[0].name, "rows_affected");
    EXPECT_EQ(schema.columns[0].type, ColumnType::INT64);

    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(Tuple::Deserialize(schema, batch[0].Span())[0].AsInt64(), 2);

    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_TRUE(batch.empty()) << "Next() must clear the batch even when exhausted";
}

TEST_F(DeleteTest, DeletedRowsDisappearFromScan) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'ann', 20), (2, 'bob', 30), (3, 'cat', 40);"), 3);

    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE id = 2;"), 1);

    auto rows = Run("SELECT id, name, age FROM users;");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].AsInt32(), 1);
    EXPECT_EQ(rows[0][1].AsVarchar(), "ann");
    EXPECT_EQ(rows[0][2].AsInt32(), 20);

    EXPECT_EQ(rows[1][0].AsInt32(), 3);
    EXPECT_EQ(rows[1][1].AsVarchar(), "cat");
    EXPECT_EQ(rows[1][2].AsInt32(), 40);
}

// ---- WHERE variations --------------------------------------------------------

TEST_F(DeleteTest, DeleteWithoutWhereRemovesAllRows) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);"), 3);

    ASSERT_EQ(ExecuteDML("DELETE FROM users;"), 3);

    auto rows = Run("SELECT id FROM users;");
    EXPECT_TRUE(rows.empty()) << "all rows must be deleted when WHERE is omitted";
}

TEST_F(DeleteTest, DeleteMatchingNoRowsEmitsZero) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20);"), 2);

    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE id = 999;"), 0);

    auto rows = Run("SELECT id FROM users;");
    EXPECT_EQ(rows.size(), 2u);
}

TEST_F(DeleteTest, DeleteOnEmptyTableEmitsZero) {
    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE id = 1;"), 0);
    ASSERT_EQ(ExecuteDML("DELETE FROM users;"), 0);
}

TEST_F(DeleteTest, CompoundWherePredicate) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'ann', 20), (2, 'bob', 20), (3, 'bob', 30);"), 3);

    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE name = 'bob' AND age = 20;"), 1);

    auto rows = Run("SELECT id, name FROM users;");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].AsInt32(), 1);
    EXPECT_EQ(rows[1][0].AsInt32(), 3);
}

// ---- Volcano discipline -------------------------------------------------------

TEST_F(DeleteTest, InitAloneDeletesNothing) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10);"), 1);

    Query q = Prepare("DELETE FROM users WHERE id = 1;");
    q.exec->Init();  // never call Next()

    auto rows = Run("SELECT id FROM users;");
    EXPECT_EQ(rows.size(), 1u) << "Init() alone must not cause deletion";
}

TEST_F(DeleteTest, CallingNextAfterExhaustionDoesNotDeleteAgain) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10);"), 1);

    Query q = Prepare("DELETE FROM users WHERE id = 1;");
    q.exec->Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    ASSERT_TRUE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_FALSE(q.exec->Next(&batch, rids));

    auto rows = Run("SELECT id FROM users;");
    EXPECT_TRUE(rows.empty());
}

// ---- Multi-batch execution ---------------------------------------------------

TEST_F(DeleteTest, DeletionIsIndependentOfBatchSize) {
    Close();
    Open(/*batch=*/2);

    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30), "
                         "(4, 'd', 40), (5, 'e', 50);"),
              5);

    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE age >= 30;"), 3);

    auto rows = Run("SELECT id FROM users;");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].AsInt32(), 1);
    EXPECT_EQ(rows[1][0].AsInt32(), 2);
}

// ---- Durability --------------------------------------------------------------

TEST_F(DeleteTest, DeletedRowsStayDeletedAfterFlushAndReopen) {
    ASSERT_EQ(ExecuteDML("INSERT INTO users VALUES (1, 'a', 10), (2, 'b', 20);"), 2);
    ASSERT_EQ(ExecuteDML("DELETE FROM users WHERE id = 1;"), 1);

    sm_->FlushAll();
    Close();
    Open(/*batch=*/1024);

    auto rows = Run("SELECT id, name FROM users;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 2);
    EXPECT_EQ(rows[0][1].AsVarchar(), "b");
}
