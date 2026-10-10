#include <gtest/gtest.h>
#include <optimizer/logical_plan.h>
#include <optimizer/physical_plan.h>

using namespace AxolotlDB;
using namespace AxolotlDB::optimizer;
using parser::ast::BinaryOperator;
using parser::ast::BoundBinaryOp;
using parser::ast::BoundColumnRef;
using parser::ast::BoundExpr;
using parser::ast::BoundLiteral;
using parser::ast::JoinType;
using storage::ColumnDef;
using storage::ColumnType;
using storage::Schema;

TEST(PhysicalPlanTest, EveryLogicalNodeMapsToAPhysicalNode) {
    Schema dummySchema{{ColumnDef{"id", ColumnType::INT32}}};

    {
        auto scan = std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema);
        auto phys = ToPhysicalPlan(std::move(scan));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::SeqScan);
        EXPECT_EQ(static_cast<PhysicalSeqScan&>(*phys).tableName, "tbl");
    }
    {
        auto filter = std::make_unique<LogicalFilter>(nullptr);
        filter->children.push_back(
            std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(filter));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Filter);
        ASSERT_EQ(phys->children.size(), 1);
        EXPECT_EQ(phys->children[0]->Type(), PhysicalNodeType::SeqScan);
    }
    {
        auto proj = std::make_unique<LogicalProject>(std::vector<ProjectColumn>{});
        proj->children.push_back(
            std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(proj));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Project);
    }
    {
        auto join = std::make_unique<LogicalNestedLoopJoin>(JoinType::INNER, nullptr);
        join->children.push_back(std::make_unique<LogicalScan>(1, "t1", std::nullopt, dummySchema));
        join->children.push_back(std::make_unique<LogicalScan>(2, "t2", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(join));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::NestedLoopJoin);
        ASSERT_EQ(phys->children.size(), 2);
    }
    {
        auto agg = std::make_unique<LogicalAggregate>();
        agg->children.push_back(std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(agg));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::HashAggregate);
    }
    {
        auto sort = std::make_unique<LogicalSort>(std::vector<SortKey>{});
        sort->children.push_back(
            std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(sort));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Sort);
    }
    {
        auto limit = std::make_unique<LogicalLimit>(nullptr, nullptr);
        limit->children.push_back(
            std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(limit));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Limit);
    }
    {
        auto insert = std::make_unique<LogicalInsert>();
        insert->tableId = 1;
        insert->tableName = "tbl";
        auto phys = ToPhysicalPlan(std::move(insert));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Insert);
    }
    {
        auto update = std::make_unique<LogicalUpdate>();
        update->tableId = 1;
        update->tableName = "tbl";
        update->children.push_back(
            std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(update));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Update);
    }
    {
        auto del = std::make_unique<LogicalDelete>();
        del->tableId = 1;
        del->tableName = "tbl";
        del->children.push_back(std::make_unique<LogicalScan>(1, "tbl", std::nullopt, dummySchema));
        auto phys = ToPhysicalPlan(std::move(del));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::Delete);
    }
    {
        auto ct = std::make_unique<LogicalCreateTable>();
        ct->tableName = "tbl";
        ct->schema = dummySchema;
        auto phys = ToPhysicalPlan(std::move(ct));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::CreateTable);
    }
    {
        auto dt = std::make_unique<LogicalDropTable>();
        dt->tableId = 1;
        dt->tableName = "tbl";
        auto phys = ToPhysicalPlan(std::move(dt));
        EXPECT_EQ(phys->Type(), PhysicalNodeType::DropTable);
    }
}

TEST(PhysicalPlanTest, EquiJoinSelectsHashJoin) {
    Schema dummySchema{{ColumnDef{"id", ColumnType::INT32}}};

    auto leftCol = std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "id", ColumnType::INT32),
                                               ColumnType::INT32);
    auto rightCol = std::make_unique<BoundExpr>(BoundColumnRef(2, 0, "id", ColumnType::INT32),
                                                ColumnType::INT32);
    auto eqCond =
        std::make_unique<BoundExpr>(BoundBinaryOp(BinaryOperator::EQUAL, std::move(leftCol),
                                                  std::move(rightCol), ColumnType::BOOLEAN),
                                    ColumnType::BOOLEAN);

    auto equiJoin = std::make_unique<LogicalNestedLoopJoin>(JoinType::INNER, std::move(eqCond));
    equiJoin->children.push_back(std::make_unique<LogicalScan>(1, "t1", std::nullopt, dummySchema));
    equiJoin->children.push_back(std::make_unique<LogicalScan>(2, "t2", std::nullopt, dummySchema));

    auto physHash = ToPhysicalPlan(std::move(equiJoin));
    EXPECT_EQ(physHash->Type(), PhysicalNodeType::HashJoin);

    auto leftCol2 = std::make_unique<BoundExpr>(BoundColumnRef(1, 0, "id", ColumnType::INT32),
                                                ColumnType::INT32);
    auto rightCol2 = std::make_unique<BoundExpr>(BoundColumnRef(2, 0, "id", ColumnType::INT32),
                                                 ColumnType::INT32);
    auto ltCond =
        std::make_unique<BoundExpr>(BoundBinaryOp(BinaryOperator::LESS, std::move(leftCol2),
                                                  std::move(rightCol2), ColumnType::BOOLEAN),
                                    ColumnType::BOOLEAN);

    auto nonEquiJoin = std::make_unique<LogicalNestedLoopJoin>(JoinType::INNER, std::move(ltCond));
    nonEquiJoin->children.push_back(
        std::make_unique<LogicalScan>(1, "t1", std::nullopt, dummySchema));
    nonEquiJoin->children.push_back(
        std::make_unique<LogicalScan>(2, "t2", std::nullopt, dummySchema));

    auto physNL = ToPhysicalPlan(std::move(nonEquiJoin));
    EXPECT_EQ(physNL->Type(), PhysicalNodeType::NestedLoopJoin);
}

TEST(PhysicalPlanTest, ErrorHandlingAndPrinting) {
    EXPECT_THROW((void)ToPhysicalPlan(nullptr), std::invalid_argument);

    Schema dummySchema{{ColumnDef{"id", ColumnType::INT32}}};
    auto scan = std::make_unique<LogicalScan>(1, "users", std::nullopt, dummySchema);
    auto phys = ToPhysicalPlan(std::move(scan));
    std::string repr = PlanToString(*phys);
    EXPECT_NE(repr.find("PhysicalSeqScan(users)"), std::string::npos);
}
