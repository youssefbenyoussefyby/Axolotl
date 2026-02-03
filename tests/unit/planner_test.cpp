#include <catalog/catalog.h>
#include <gtest/gtest.h>
#include <optimizer/logical_plan.h>
#include <optimizer/planner.h>
#include <parser/ast/boundstmt.h>
#include <parser/binder.h>
#include <parser/parser.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace AxolotlDB;
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::Schema;

struct PlannerTest : ::testing::Test {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    std::unique_ptr<storage::StorageManager> sm;
    std::unique_ptr<catalog::Catalog> cat;
    utils::DiagnosticSink sink;

    void SetUp() override {
        orig_dir = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
              ("axo_plan_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);

        sm = std::make_unique<storage::StorageManager>(32);
        cat = std::make_unique<catalog::Catalog>(*sm);

        cat->CreateTable("users", Schema{{
            ColumnDef{"id", ColumnType::INT32},
            ColumnDef{"name", ColumnType::VARCHAR},
            ColumnDef{"age", ColumnType::INT32}
        }});

        cat->CreateTable("orders", Schema{{
            ColumnDef{"order_id", ColumnType::INT32},
            ColumnDef{"user_id", ColumnType::INT32},
            ColumnDef{"amount", ColumnType::FLOAT64}
        }});

        cat->CreateTable("products", Schema{{
            ColumnDef{"id", ColumnType::INT32},
            ColumnDef{"name", ColumnType::VARCHAR},
            ColumnDef{"price", ColumnType::FLOAT64}
        }});
    }

    void TearDown() override {
        cat.reset();
        sm.reset();
        std::error_code ec;
        std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }

    optimizer::LogicalNodePtr Plan(const std::string& sql) {
        sink = utils::DiagnosticSink();
        parser::Parser p(sql, sink);
        auto stmt = p.Parse();
        binder::Binder b(*cat, &sink);
        auto bound = b.Bind(stmt);
        optimizer::Planner planner;
        return planner.Plan(*bound);
    }
};

TEST_F(PlannerTest, SimpleSelectProducesProjectOverScan) {
    auto plan = Plan("SELECT id, name FROM users;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    ASSERT_EQ(plan->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Scan);

    const auto& scan = static_cast<optimizer::LogicalScan&>(*plan->children[0]);
    EXPECT_EQ(scan.tableName, "users");
}

TEST_F(PlannerTest, SelectStarOutputSchemaMatchesTableColumns) {
    auto plan = Plan("SELECT * FROM users;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    auto schema = plan->OutputSchema();
    ASSERT_EQ(schema.size(), 3u);
    EXPECT_EQ(schema[0].name, "id");
    EXPECT_EQ(schema[1].name, "name");
    EXPECT_EQ(schema[2].name, "age");
}

TEST_F(PlannerTest, WhereClauseProducesFilterBetweenScanAndProject) {
    auto plan = Plan("SELECT id FROM users WHERE age > 18;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    ASSERT_EQ(plan->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Filter);

    ASSERT_EQ(plan->children[0]->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->children[0]->Type(), optimizer::LogicalNodeType::Scan);
}

TEST_F(PlannerTest, FilterPredicateIsSet) {
    auto plan = Plan("SELECT id FROM users WHERE age > 18;");
    ASSERT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Filter);

    const auto& filter = static_cast<optimizer::LogicalFilter&>(*plan->children[0]);
    EXPECT_NE(filter.predicate, nullptr);
}

TEST_F(PlannerTest, InnerJoinProducesNestedLoopJoinNode) {
    auto plan = Plan("SELECT users.id FROM users JOIN orders ON users.id = orders.user_id;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    ASSERT_EQ(plan->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::NestedLoopJoin);
}

TEST_F(PlannerTest, JoinNodeHasTwoScanChildren) {
    auto plan = Plan("SELECT users.id FROM users JOIN orders ON users.id = orders.user_id;");
    const auto& join = *plan->children[0];

    ASSERT_EQ(join.children.size(), 2u);
    EXPECT_EQ(join.children[0]->Type(), optimizer::LogicalNodeType::Scan);
    EXPECT_EQ(join.children[1]->Type(), optimizer::LogicalNodeType::Scan);

    const auto& left = static_cast<optimizer::LogicalScan&>(*join.children[0]);
    const auto& right = static_cast<optimizer::LogicalScan&>(*join.children[1]);
    EXPECT_EQ(left.tableName, "users");
    EXPECT_EQ(right.tableName, "orders");
}

TEST_F(PlannerTest, JoinOnConditionIsSet) {
    auto plan = Plan("SELECT users.id FROM users JOIN orders ON users.id = orders.user_id;");
    const auto& join = static_cast<optimizer::LogicalNestedLoopJoin&>(*plan->children[0]);
    EXPECT_NE(join.condition, nullptr);
}

TEST_F(PlannerTest, JoinWithWhereClauseSetsFilter) {
    auto plan = Plan("SELECT users.id FROM users JOIN orders ON users.id = orders.user_id WHERE orders.amount > 100;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Filter);
    EXPECT_EQ(plan->children[0]->children[0]->Type(), optimizer::LogicalNodeType::NestedLoopJoin);
}

TEST_F(PlannerTest, AggregateQueryProducesAggregateNode) {
    auto plan = Plan("SELECT COUNT(id) FROM users;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Aggregate);
}

TEST_F(PlannerTest, GroupByProducesAggregateNode) {
    auto plan = Plan("SELECT age, COUNT(id) FROM users GROUP BY age;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Aggregate);

    const auto& agg = static_cast<optimizer::LogicalAggregate&>(*plan);
    EXPECT_EQ(agg.groupBy.size(), 1u);
    EXPECT_EQ(agg.aggregates.size(), 2u);
}

TEST_F(PlannerTest, HavingClauseIsStoredInAggregateNode) {
    auto plan = Plan("SELECT age, COUNT(id) FROM users GROUP BY age HAVING COUNT(id) > 5;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Aggregate);

    const auto& agg = static_cast<optimizer::LogicalAggregate&>(*plan);
    EXPECT_NE(agg.having, nullptr);
}

TEST_F(PlannerTest, OrderByProducesSortAboveProject) {
    auto plan = Plan("SELECT id FROM users ORDER BY age DESC;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Sort);

    const auto& sort = static_cast<optimizer::LogicalSort&>(*plan);
    ASSERT_EQ(sort.keys.size(), 1u);
    EXPECT_TRUE(sort.keys[0].descending);

    EXPECT_EQ(sort.children[0]->Type(), optimizer::LogicalNodeType::Project);
}

TEST_F(PlannerTest, LimitProducesLimitNode) {
    auto plan = Plan("SELECT id FROM users LIMIT 10;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    const auto& lim = static_cast<optimizer::LogicalLimit&>(*plan);
    EXPECT_NE(lim.limit, nullptr);
    EXPECT_EQ(lim.offset, nullptr);
}

TEST_F(PlannerTest, LimitOffsetBothSet) {
    auto plan = Plan("SELECT id FROM users LIMIT 10 OFFSET 20;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    const auto& lim = static_cast<optimizer::LogicalLimit&>(*plan);
    EXPECT_NE(lim.limit, nullptr);
    EXPECT_NE(lim.offset, nullptr);
}

TEST_F(PlannerTest, OrderByThenLimitOrderIsCorrect) {
    auto plan = Plan("SELECT id FROM users ORDER BY age LIMIT 5;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Sort);
    EXPECT_EQ(plan->children[0]->children[0]->Type(), optimizer::LogicalNodeType::Project);
}

TEST_F(PlannerTest, InsertProducesInsertNode) {
    auto plan = Plan("INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);");
    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Insert);

    const auto& ins = static_cast<optimizer::LogicalInsert&>(*plan);
    EXPECT_EQ(ins.tableName, "users");
    EXPECT_EQ(ins.rows.size(), 1u);
    EXPECT_EQ(ins.rows[0].values.size(), 3u);
}

TEST_F(PlannerTest, InsertMultipleRowsPreservedInPlan) {
    auto plan = Plan("INSERT INTO users VALUES (1, 'A', 20), (2, 'B', 25), (3, 'C', 30);");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Insert);

    const auto& ins = static_cast<optimizer::LogicalInsert&>(*plan);
    EXPECT_EQ(ins.rows.size(), 3u);
}

TEST_F(PlannerTest, InsertOutputSchemaIsRowsAffected) {
    auto plan = Plan("INSERT INTO users VALUES (1, 'A', 20);");
    auto schema = plan->OutputSchema();
    ASSERT_EQ(schema.size(), 1u);
    EXPECT_EQ(schema[0].name, "rows_affected");
}

TEST_F(PlannerTest, UpdateProducesUpdateWithFilteredScan) {
    auto plan = Plan("UPDATE users SET name = 'Bob' WHERE id = 1;");
    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Update);

    const auto& upd = static_cast<optimizer::LogicalUpdate&>(*plan);
    EXPECT_EQ(upd.tableName, "users");
    ASSERT_EQ(upd.assignments.size(), 1u);
    EXPECT_EQ(upd.assignments[0].columnIndex, 1u);

    ASSERT_EQ(upd.children.size(), 1u);
    EXPECT_EQ(upd.children[0]->Type(), optimizer::LogicalNodeType::Filter);
    EXPECT_EQ(upd.children[0]->children[0]->Type(), optimizer::LogicalNodeType::Scan);
}

TEST_F(PlannerTest, UpdateWithoutWhereHasNoFilter) {
    auto plan = Plan("UPDATE users SET age = 0;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Update);

    ASSERT_EQ(plan->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Scan);
}

TEST_F(PlannerTest, DeleteProducesDeleteWithFilteredScan) {
    auto plan = Plan("DELETE FROM users WHERE id = 5;");
    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Delete);

    const auto& del = static_cast<optimizer::LogicalDelete&>(*plan);
    EXPECT_EQ(del.tableName, "users");

    ASSERT_EQ(del.children.size(), 1u);
    EXPECT_EQ(del.children[0]->Type(), optimizer::LogicalNodeType::Filter);
}

TEST_F(PlannerTest, DeleteWithoutWhereHasNoFilter) {
    auto plan = Plan("DELETE FROM users;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Delete);

    ASSERT_EQ(plan->children.size(), 1u);
    EXPECT_EQ(plan->children[0]->Type(), optimizer::LogicalNodeType::Scan);
}

TEST_F(PlannerTest, CreateTableProducesCreateNode) {
    auto plan = Plan("CREATE TABLE items (id INT32 NOT NULL, title VARCHAR);");
    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::CreateTable);

    const auto& create = static_cast<optimizer::LogicalCreateTable&>(*plan);
    EXPECT_EQ(create.tableName, "items");
    ASSERT_EQ(create.schema.columns.size(), 2u);
    EXPECT_EQ(create.schema.columns[0].type, ColumnType::INT32);
    EXPECT_EQ(create.schema.columns[1].type, ColumnType::VARCHAR);
}

TEST_F(PlannerTest, DropTableProducesDropNode) {
    auto plan = Plan("DROP TABLE users;");
    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::DropTable);

    const auto& drop = static_cast<optimizer::LogicalDropTable&>(*plan);
    EXPECT_EQ(drop.tableName, "users");
    EXPECT_NE(drop.tableId, 0u);
}

TEST_F(PlannerTest, ScanOutputSchemaMatchesCatalogSchema) {
    auto plan = Plan("SELECT * FROM orders;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    const auto& scanNode = *plan->children[0];
    ASSERT_EQ(scanNode.Type(), optimizer::LogicalNodeType::Scan);
    auto schema = scanNode.OutputSchema();
    ASSERT_EQ(schema.size(), 3u);
    EXPECT_EQ(schema[0].name, "order_id");
    EXPECT_EQ(schema[1].name, "user_id");
    EXPECT_EQ(schema[2].name, "amount");
    EXPECT_EQ(schema[2].type, ColumnType::FLOAT64);
}

TEST_F(PlannerTest, JoinOutputSchemaIsCombinedColumns) {
    auto plan = Plan("SELECT * FROM users JOIN orders ON users.id = orders.user_id;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    const auto& joinNode = *plan->children[0];
    ASSERT_EQ(joinNode.Type(), optimizer::LogicalNodeType::NestedLoopJoin);
    auto schema = joinNode.OutputSchema();
    EXPECT_EQ(schema.size(), 6u);
}

TEST_F(PlannerTest, ArithmeticExprInProjection) {
    auto plan = Plan("SELECT id + age FROM users;");
    ASSERT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    auto schema = plan->OutputSchema();
    ASSERT_EQ(schema.size(), 1u);
    EXPECT_EQ(schema[0].type, ColumnType::INT32);
}

TEST_F(PlannerTest, ThreeTableJoinProducesNestedJoins) {
    auto plan = Plan("SELECT users.id FROM users JOIN orders ON users.id = orders.user_id JOIN products ON orders.order_id = products.id;");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    const auto& outerJoin = *plan->children[0];
    EXPECT_EQ(outerJoin.Type(), optimizer::LogicalNodeType::NestedLoopJoin);

    const auto& innerJoin = *outerJoin.children[0];
    EXPECT_EQ(innerJoin.Type(), optimizer::LogicalNodeType::NestedLoopJoin);
}

TEST_F(PlannerTest, DistinctSelectPreservesInPlan) {
    EXPECT_NO_THROW({
        auto plan = Plan("SELECT DISTINCT age FROM users;");
        EXPECT_NE(plan, nullptr);
    });
}
