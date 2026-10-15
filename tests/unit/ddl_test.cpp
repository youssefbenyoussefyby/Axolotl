// P4.3 -- CREATE TABLE / DROP TABLE through SQL.
//
// Contract: no child, an empty output schema, no output rows; the catalog is
// changed by the first Next(), never by Init(), and only once.
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
#include <vector>

using namespace AxolotlDB;
using storage::ColumnType;
using storage::Tuple;
using storage::Value;

namespace {

struct Query {
    optimizer::PhysicalNodePtr plan;  // executors hold the plan by reference
    std::unique_ptr<executor::AbstractExecutor> exec;
};

}  // namespace

class DdlTest : public ::testing::Test {
   protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;
    std::unique_ptr<storage::StorageManager> sm_;
    std::unique_ptr<catalog::Catalog> cat_;
    std::unique_ptr<executor::ExecutorContext> ctx_;

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("axo_ddl_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);
        Open();
    }

    void TearDown() override {
        Close();
        std::error_code ec;
        std::filesystem::current_path(orig_dir_, ec);
        std::filesystem::remove_all(dir_, ec);
    }

    void Open() {
        sm_ = std::make_unique<storage::StorageManager>(16);
        cat_ = std::make_unique<catalog::Catalog>(*sm_);
        ctx_ = std::make_unique<executor::ExecutorContext>(*sm_, *cat_, nullptr, 1024);
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

    bool TableExists(const std::string& name) {
        try {
            (void)cat_->GetTableId(name);
            return true;
        } catch (const utils::StorageException&) {
            return false;
        }
    }
};

// ---- CREATE TABLE ------------------------------------------------------------

TEST_F(DdlTest, CreatedTableAcceptsInsertAndSelect) {
    Run("CREATE TABLE reviews (id INT32 NOT NULL, rating INT32, body VARCHAR(200));");
    ASSERT_TRUE(TableExists("reviews"));

    Run("INSERT INTO reviews VALUES (1, 5, 'fast shipping'), (2, 1, 'box arrived crushed');");
    auto rows = Run("SELECT id, rating, body FROM reviews WHERE rating < 3;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].AsInt32(), 2);
    EXPECT_EQ(rows[0][2].AsVarchar(), "box arrived crushed");
}

TEST_F(DdlTest, CreateTableStoresTypesAndNullability) {
    Run("CREATE TABLE t (a INT32 NOT NULL, b INT64, c FLOAT32, d FLOAT64 NOT NULL, e BOOLEAN, "
        "f VARCHAR(10));");
    const auto& schema = cat_->GetSchema("t");
    ASSERT_EQ(schema.ColumnCount(), 6u);
    EXPECT_EQ(schema.columns[0].type, ColumnType::INT32);
    EXPECT_FALSE(schema.columns[0].nullable);
    EXPECT_EQ(schema.columns[1].type, ColumnType::INT64);
    EXPECT_TRUE(schema.columns[1].nullable) << "SQL columns are nullable unless NOT NULL";
    EXPECT_EQ(schema.columns[2].type, ColumnType::FLOAT32);
    EXPECT_EQ(schema.columns[3].type, ColumnType::FLOAT64);
    EXPECT_FALSE(schema.columns[3].nullable);
    EXPECT_EQ(schema.columns[4].type, ColumnType::BOOLEAN);
    EXPECT_EQ(schema.columns[5].type, ColumnType::VARCHAR);
}

TEST_F(DdlTest, NullabilityFromSqlIsEnforcedOnInsert) {
    Run("CREATE TABLE t (id INT32 NOT NULL, note VARCHAR(20));");
    Run("INSERT INTO t (id) VALUES (1);");  // note is nullable
    EXPECT_THROW((void)Run("INSERT INTO t (note) VALUES ('x');"), utils::SemanticException);
    auto rows = Run("SELECT id, note FROM t;");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0][1].IsNull());
}

TEST_F(DdlTest, CreateTableHasNoOutputAndCreatesOnce) {
    Query q = Prepare("CREATE TABLE t (id INT32);");
    EXPECT_EQ(q.exec->GetOutputSchema().ColumnCount(), 0u);
    q.exec->Init();
    std::vector<Tuple> batch;
    std::optional<std::vector<storage::RID>> rids = std::nullopt;
    EXPECT_FALSE(q.exec->Next(&batch, rids));
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(TableExists("t"));
    // A second pull must not try to create it again (which would throw).
    EXPECT_NO_THROW((void)q.exec->Next(&batch, rids));
}

TEST_F(DdlTest, InitAloneCreatesNothing) {
    Query q = Prepare("CREATE TABLE t (id INT32);");
    q.exec->Init();
    EXPECT_FALSE(TableExists("t"));
}

TEST_F(DdlTest, CreateExistingTableIsRejectedByTheBinder) {
    Run("CREATE TABLE t (id INT32);");
    EXPECT_THROW((void)Run("CREATE TABLE t (x INT32);"), utils::SemanticException);
}

TEST_F(DdlTest, VectorColumnGetsItsVectorHeap) {
    Run("CREATE TABLE docs (id INT32 NOT NULL, emb VECTOR(4));");
    EXPECT_EQ(cat_->GetVectorHeap("docs", "emb").Dimension(), 4u);
    Run("INSERT INTO docs VALUES (1, NULL);");
    EXPECT_EQ(Run("SELECT id FROM docs;").size(), 1u);
}

TEST_F(DdlTest, CreatedTableSurvivesFlushAndReopen) {
    Run("CREATE TABLE t (id INT32 NOT NULL, name VARCHAR(20));");
    Run("INSERT INTO t VALUES (1, 'a'), (2, 'b');");
    sm_->FlushAll();
    Close();
    Open();
    ASSERT_TRUE(TableExists("t"));
    EXPECT_FALSE(cat_->GetSchema("t").columns[0].nullable);
    EXPECT_EQ(Run("SELECT id FROM t;").size(), 2u);
}

// ---- DROP TABLE ----------------------------------------------------------------

TEST_F(DdlTest, DroppedTableIsGone) {
    Run("CREATE TABLE t (id INT32);");
    Run("INSERT INTO t VALUES (1);");
    Run("DROP TABLE t;");
    EXPECT_FALSE(TableExists("t"));
    EXPECT_THROW((void)Run("SELECT id FROM t;"), utils::SemanticException);
}

TEST_F(DdlTest, RecreatedTableStartsEmpty) {
    Run("CREATE TABLE t (id INT32);");
    Run("INSERT INTO t VALUES (1), (2);");
    Run("DROP TABLE t;");
    Run("CREATE TABLE t (id INT32, extra VARCHAR(5));");
    EXPECT_TRUE(Run("SELECT id FROM t;").empty());
    EXPECT_EQ(cat_->GetSchema("t").ColumnCount(), 2u);
}

TEST_F(DdlTest, DropUnknownTableIsRejectedByTheBinder) {
    EXPECT_THROW((void)Run("DROP TABLE missing;"), utils::SemanticException);
}

TEST_F(DdlTest, DropInitAloneDropsNothing) {
    Run("CREATE TABLE t (id INT32);");
    Query q = Prepare("DROP TABLE t;");
    q.exec->Init();
    EXPECT_TRUE(TableExists("t"));
}

TEST_F(DdlTest, DroppedTableStaysGoneAfterReopen) {
    Run("CREATE TABLE keep (id INT32);");
    Run("CREATE TABLE gone (id INT32);");
    Run("DROP TABLE gone;");
    sm_->FlushAll();
    Close();
    Open();
    EXPECT_TRUE(TableExists("keep"));
    EXPECT_FALSE(TableExists("gone"));
}
