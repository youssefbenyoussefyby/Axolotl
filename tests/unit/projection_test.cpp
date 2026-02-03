#include <catalog/catalog.h>
#include <executor/executor_context.h>
#include <executor/operators/project.h>
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

class MockChildExecutorProj : public AbstractExecutor {
public:
    MockChildExecutorProj(ExecutorContext* ctx, Schema schema)
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

TEST(ProjectionTest, OutputSchemaMatchesProjectionList) {
    Schema inputSchema{{
        ColumnDef{"a", ColumnType::INT32},
        ColumnDef{"b", ColumnType::VARCHAR},
        ColumnDef{"c", ColumnType::FLOAT64}
    }};

    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat);

    auto child = std::make_unique<MockChildExecutorProj>(&ctx, inputSchema);

    PhysicalProject plan;
    plan.projections.push_back(ProjectColumn{
        std::make_unique<BoundExpr>(BoundColumnRef(1, 1, "b", ColumnType::VARCHAR), ColumnType::VARCHAR),
        "user_name"
    });
    plan.projections.push_back(ProjectColumn{
        std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "a", ColumnType::INT32), ColumnType::INT32),
        "user_id"
    });

    ProjectionExecutor proj(&ctx, plan, std::move(child));
    const auto& outSchema = proj.GetOutputSchema();

    ASSERT_EQ(outSchema.ColumnCount(), 2);
    EXPECT_EQ(outSchema.columns[0].name, "user_name");
    EXPECT_EQ(outSchema.columns[0].type, ColumnType::VARCHAR);
    EXPECT_EQ(outSchema.columns[1].name, "user_id");
    EXPECT_EQ(outSchema.columns[1].type, ColumnType::INT32);
}

TEST(ProjectionTest, ComputedExpressionColumn) {
    Schema inputSchema{{
        ColumnDef{"price", ColumnType::INT32},
        ColumnDef{"tax", ColumnType::INT32}
    }};

    storage::StorageManager sm(8);
    catalog::Catalog cat(sm);
    ExecutorContext ctx(sm, cat);

    auto child = std::make_unique<MockChildExecutorProj>(&ctx, inputSchema);
    child->Add(Tuple(Tuple::Serialize(inputSchema, {Value::Int32(100), Value::Int32(15)})), RID{1, 1});
    child->Add(Tuple(Tuple::Serialize(inputSchema, {Value::Int32(200), Value::Int32(30)})), RID{1, 2});

    PhysicalProject plan;
    auto left = std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "price", ColumnType::INT32), ColumnType::INT32);
    auto right = std::make_unique<BoundExpr>(BoundColumnRef(1, 1, "tax", ColumnType::INT32), ColumnType::INT32);
    auto totalExpr = std::make_unique<BoundExpr>(
        BoundBinaryOp(BinaryOperator::ADD, std::move(left), std::move(right), ColumnType::INT32),
        ColumnType::INT32);

    plan.projections.push_back(ProjectColumn{std::move(totalExpr), "total"});

    ProjectionExecutor proj(&ctx, plan, std::move(child));
    proj.Init();

    std::vector<Tuple> batch;
    std::optional<std::vector<RID>> rids = std::nullopt;
    ASSERT_TRUE(proj.Next(&batch, rids));
    ASSERT_EQ(batch.size(), 2);

    auto row0 = Tuple::Deserialize(proj.GetOutputSchema(), batch[0].Span());
    EXPECT_EQ(row0[0].AsInt32(), 115);

    auto row1 = Tuple::Deserialize(proj.GetOutputSchema(), batch[1].Span());
    EXPECT_EQ(row1[0].AsInt32(), 230);
}
