#include <catalog/catalog.h>
#include <executor/executor_context.h>
#include <executor/operators/scan.h>
#include <gtest/gtest.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB;
using namespace AxolotlDB::executor;
using namespace AxolotlDB::optimizer;
using storage::ColumnDef;
using storage::ColumnType;
using storage::RID;
using storage::Schema;
using storage::Tuple;
using storage::Value;

class SeqScanTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;
    std::unique_ptr<storage::StorageManager> sm_;
    std::unique_ptr<catalog::Catalog> cat_;
    std::unique_ptr<ExecutorContext> ctx_;
    Schema schema_{{
        ColumnDef{"id", ColumnType::INT32},
        ColumnDef{"name", ColumnType::VARCHAR},
        ColumnDef{"val", ColumnType::INT64}
    }};

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("seq_scan_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);

        sm_ = std::make_unique<storage::StorageManager>(16);
        cat_ = std::make_unique<catalog::Catalog>(*sm_);
        ctx_ = std::make_unique<ExecutorContext>(*sm_, *cat_, nullptr, 2);
    }

    void TearDown() override {
        ctx_.reset();
        cat_.reset();
        sm_.reset();
        std::error_code ec;
        std::filesystem::current_path(orig_dir_, ec);
        std::filesystem::remove_all(dir_, ec);
    }
};

TEST_F(SeqScanTest, ReturnsEveryLiveRow) {
    cat_->CreateTable("users", schema_);
    auto& heap = cat_->GetTableHeap("users");

    for (int i = 0; i < 5; ++i) {
        auto b = Tuple::Serialize(schema_, {Value::Int32(i), Value::Varchar("user_" + std::to_string(i)), Value::Int64(i * 10)});
        (void)heap.Insert(b);
    }

    PhysicalSeqScan plan;
    plan.tableName = "users";
    plan.schema = schema_;

    SeqScanExecutor scan(ctx_.get(), plan);
    scan.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::vector<RID>{};
    std::vector<int32_t> ids;

    while (scan.Next(&batch, rids)) {
        for (const auto& t : batch) {
            auto vals = Tuple::Deserialize(schema_, t.Span());
            ids.push_back(vals[0].AsInt32());
        }
    }

    EXPECT_EQ(ids.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(ids[i], i);
    }
}

TEST_F(SeqScanTest, SkipsTombstones) {
    cat_->CreateTable("users", schema_);
    auto& heap = cat_->GetTableHeap("users");

    std::vector<RID> insertedRids;
    for (int i = 0; i < 6; ++i) {
        auto b = Tuple::Serialize(schema_, {Value::Int32(i), Value::Varchar("user_" + std::to_string(i)), Value::Int64(i * 10)});
        insertedRids.push_back(heap.Insert(b));
    }

    heap.Delete(insertedRids[1]);
    heap.Delete(insertedRids[3]);

    PhysicalSeqScan plan;
    plan.tableName = "users";
    plan.schema = schema_;

    SeqScanExecutor scan(ctx_.get(), plan);
    scan.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::vector<RID>{};
    std::vector<int32_t> ids;

    while (scan.Next(&batch, rids)) {
        for (const auto& t : batch) {
            auto vals = Tuple::Deserialize(schema_, t.Span());
            ids.push_back(vals[0].AsInt32());
        }
    }

    ASSERT_EQ(ids.size(), 4);
    EXPECT_EQ(ids[0], 0);
    EXPECT_EQ(ids[1], 2);
    EXPECT_EQ(ids[2], 4);
    EXPECT_EQ(ids[3], 5);
}

TEST_F(SeqScanTest, EmptyTableTerminatesImmediately) {
    cat_->CreateTable("empty_tbl", schema_);

    PhysicalSeqScan plan;
    plan.tableName = "empty_tbl";
    plan.schema = schema_;

    SeqScanExecutor scan(ctx_.get(), plan);
    scan.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::vector<RID>{};

    EXPECT_FALSE(scan.Next(&batch, rids));
    EXPECT_TRUE(batch.empty());
}
