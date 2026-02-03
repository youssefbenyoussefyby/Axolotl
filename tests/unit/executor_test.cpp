#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <gtest/gtest.h>
#include <optimizer/physical_plan.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>

#include <filesystem>
#include <memory>
#include <optional>
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

class MockExecutor : public AbstractExecutor {
public:
    explicit MockExecutor(ExecutorContext* ctx, Schema schema)
        : AbstractExecutor(ctx), schema_(std::move(schema)) {}

    void Init() override {
        cursor_ = 0;
    }

    bool Next(std::vector<Tuple>* tupleBatch, std::optional<std::vector<RID>>& ridBatch) override {
        if (!tupleBatch) {
            return false;
        }
        tupleBatch->clear();
        if (ridBatch.has_value()) {
            ridBatch->clear();
        }

        if (cursor_ >= data_.size()) {
            return false;
        }

        const std::size_t batchSize = context_ ? context_->GetBatchSize() : 1024;
        while (cursor_ < data_.size() && tupleBatch->size() < batchSize) {
            tupleBatch->push_back(data_[cursor_].first);
            if (ridBatch.has_value()) {
                ridBatch->push_back(data_[cursor_].second);
            }
            ++cursor_;
        }
        return true;
    }

    const Schema& GetOutputSchema() const override {
        return schema_;
    }

    void Add(Tuple t, RID r) {
        data_.emplace_back(std::move(t), r);
    }

private:
    Schema schema_;
    std::vector<std::pair<Tuple, RID>> data_;
    std::size_t cursor_ = 0;
};

TEST(ExecutorTest, NextWritesTupleOnlyWhenReturningTrue) {
    Schema schema{{ColumnDef{"x", ColumnType::INT32}}};
    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat, nullptr, 2);

    MockExecutor exec(&ctx, schema);
    for (int i = 0; i < 3; ++i) {
        auto bytes = Tuple::Serialize(schema, {Value::Int32(100 + i)});
        exec.Add(Tuple(std::move(bytes)), RID{1, static_cast<uint16_t>(i)});
    }
    exec.Init();

    std::vector<Tuple> tupleBatch;
    std::optional<std::vector<RID>> ridBatch = std::vector<RID>{};

    EXPECT_TRUE(exec.Next(&tupleBatch, ridBatch));
    ASSERT_EQ(tupleBatch.size(), 2);
    ASSERT_TRUE(ridBatch.has_value());
    ASSERT_EQ(ridBatch->size(), 2);
    EXPECT_EQ(Tuple::Deserialize(schema, tupleBatch[0].Span())[0].AsInt32(), 100);
    EXPECT_EQ(Tuple::Deserialize(schema, tupleBatch[1].Span())[0].AsInt32(), 101);

    EXPECT_TRUE(exec.Next(&tupleBatch, ridBatch));
    ASSERT_EQ(tupleBatch.size(), 1);
    EXPECT_EQ(Tuple::Deserialize(schema, tupleBatch[0].Span())[0].AsInt32(), 102);

    EXPECT_FALSE(exec.Next(&tupleBatch, ridBatch));
    EXPECT_TRUE(tupleBatch.empty());
    EXPECT_TRUE(ridBatch->empty());
}

TEST(ExecutorTest, FactoryBuildsMatchingTreeShape) {
    Schema schema{{ColumnDef{"id", ColumnType::INT32}}};
    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat);

    EXPECT_THROW((void)CreateExecutor(PhysicalSeqScan{}, nullptr), std::invalid_argument);

    PhysicalSeqScan scanNode;
    EXPECT_NE(CreateExecutor(scanNode, &ctx), nullptr);

    PhysicalFilter badFilter;
    EXPECT_THROW((void)CreateExecutor(badFilter, &ctx), std::logic_error);

    PhysicalFilter filterWithChild;
    filterWithChild.children.push_back(std::make_unique<PhysicalSeqScan>());
    EXPECT_NE(CreateExecutor(filterWithChild, &ctx), nullptr);

    PhysicalHashAggregate agg;
    agg.children.push_back(std::make_unique<PhysicalSeqScan>());
    EXPECT_THROW((void)CreateExecutor(agg, &ctx), std::logic_error);
}

TEST(ExecutorTest, TupleSurvivesEvictionTriggeredByPipeline) {
    auto origDir = std::filesystem::current_path();
    auto dir = std::filesystem::temp_directory_path() /
               ("axo_exec_n5_" + std::to_string(reinterpret_cast<std::uintptr_t>(&origDir)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::current_path(dir);

    {
        storage::StorageManager sm(2);
        catalog::Catalog cat(sm);

        Schema schema{{
            ColumnDef{"id", ColumnType::INT32},
            ColumnDef{"payload", ColumnType::VARCHAR}
        }};
        cat.CreateTable("t", schema);
        auto& heap = cat.GetTableHeap("t");

        auto b1 = Tuple::Serialize(schema, {Value::Int32(1), Value::Varchar("tuple-original-content")});
        auto rid1 = heap.Insert(b1);

        storage::ReadPageGuard guard;
        auto span = heap.Get(rid1, guard);
        std::vector<std::byte> copyBytes(span.begin(), span.end());
        Tuple heldTuple(std::move(copyBytes), rid1);
        guard.Drop();

        for (int i = 0; i < 20; ++i) {
            std::string filler(1500, static_cast<char>('A' + (i % 26)));
            auto b = Tuple::Serialize(schema, {Value::Int32(100 + i), Value::Varchar(filler)});
            (void)heap.Insert(b);
        }

        auto readVals = Tuple::Deserialize(schema, heldTuple.Span());
        EXPECT_EQ(readVals[0].AsInt32(), 1);
        EXPECT_EQ(readVals[1].AsVarchar(), "tuple-original-content");
    }

    std::filesystem::current_path(origDir);
    std::filesystem::remove_all(dir);
}
