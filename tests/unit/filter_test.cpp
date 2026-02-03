#include <catalog/catalog.h>
#include <executor/executor_context.h>
#include <executor/operators/filter.h>
#include <gtest/gtest.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>

#include <memory>
#include <vector>

using namespace AxolotlDB;
using namespace AxolotlDB::executor;
using namespace AxolotlDB::optimizer;
using parser::ast::BinaryOperator;
using parser::ast::BoundBinaryOp;
using parser::ast::BoundColumnRef;
using parser::ast::BoundExpr;
using parser::ast::BoundLiteral;
using storage::ColumnDef;
using storage::ColumnType;
using storage::RID;
using storage::Schema;
using storage::Tuple;
using storage::Value;

class MockChildExecutor : public AbstractExecutor {
public:
    MockChildExecutor(ExecutorContext* ctx, Schema schema)
        : AbstractExecutor(ctx), schema_(std::move(schema)) {}

    void Init() override {
        cursor_ = 0;
    }

    bool Next(std::vector<Tuple>* tupleBatch, std::optional<std::vector<RID>>& ridBatch) override {
        if (!tupleBatch) return false;
        tupleBatch->clear();
        if (ridBatch.has_value()) ridBatch->clear();

        if (cursor_ >= data_.size()) return false;

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

TEST(FilterTest, PassesOnlyMatchingRows) {
    Schema schema{{ColumnDef{"val", ColumnType::INT32}}};
    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat, nullptr, 2);

    auto child = std::make_unique<MockChildExecutor>(&ctx, schema);
    for (int v : {10, 25, 5, 40, 15, 50}) {
        auto bytes = Tuple::Serialize(schema, {Value::Int32(v)});
        child->Add(Tuple(std::move(bytes)), RID{1, static_cast<uint16_t>(v)});
    }

    auto colRef = std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "val", ColumnType::INT32), ColumnType::INT32);
    auto lit = std::make_unique<BoundExpr>(BoundLiteral(ColumnType::INT32, static_cast<int64_t>(20)), ColumnType::INT32);
    auto pred = std::make_unique<BoundExpr>(
        BoundBinaryOp(BinaryOperator::GREATER, std::move(colRef), std::move(lit), ColumnType::BOOLEAN),
        ColumnType::BOOLEAN);

    PhysicalFilter plan;
    plan.predicate = std::move(pred);

    FilterExecutor filter(&ctx, plan, std::move(child));
    filter.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::vector<RID>{};
    std::vector<int32_t> results;

    while (filter.Next(&batch, rids)) {
        for (const auto& t : batch) {
            auto vals = Tuple::Deserialize(schema, t.Span());
            results.push_back(vals[0].AsInt32());
        }
    }

    ASSERT_EQ(results.size(), 3);
    EXPECT_EQ(results[0], 25);
    EXPECT_EQ(results[1], 40);
    EXPECT_EQ(results[2], 50);
}

TEST(FilterTest, NullPredicateExcludesRow) {
    Schema schema{{ColumnDef{"val", ColumnType::INT32}}};
    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat, nullptr, 2);

    auto child = std::make_unique<MockChildExecutor>(&ctx, schema);
    child->Add(Tuple(Tuple::Serialize(schema, {Value::Int32(10)})), RID{1, 1});
    child->Add(Tuple(Tuple::Serialize(schema, {Value::Null(ColumnType::INT32)})), RID{1, 2});
    child->Add(Tuple(Tuple::Serialize(schema, {Value::Int32(30)})), RID{1, 3});

    auto colRef = std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "val", ColumnType::INT32), ColumnType::INT32);
    auto lit = std::make_unique<BoundExpr>(BoundLiteral(ColumnType::INT32, static_cast<int64_t>(20)), ColumnType::INT32);
    auto pred = std::make_unique<BoundExpr>(
        BoundBinaryOp(BinaryOperator::GREATER, std::move(colRef), std::move(lit), ColumnType::BOOLEAN),
        ColumnType::BOOLEAN);

    PhysicalFilter plan;
    plan.predicate = std::move(pred);

    FilterExecutor filter(&ctx, plan, std::move(child));
    filter.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::nullopt;
    std::vector<int32_t> results;

    while (filter.Next(&batch, rids)) {
        for (const auto& t : batch) {
            auto vals = Tuple::Deserialize(schema, t.Span());
            results.push_back(vals[0].AsInt32());
        }
    }

    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0], 30);
}
