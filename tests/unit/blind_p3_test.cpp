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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB;
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::Schema;

struct BlindP3Test : public ::testing::Test {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    std::unique_ptr<storage::StorageManager> sm;
    std::unique_ptr<catalog::Catalog> cat;
    utils::DiagnosticSink sink;

    void SetUp() override {
        orig_dir = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
              ("blind_p3_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));

        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);

        sm = std::make_unique<storage::StorageManager>(32);
        cat = std::make_unique<catalog::Catalog>(*sm);

        cat->CreateTable(
            "users",
            Schema{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR},
                    ColumnDef{"age", ColumnType::INT32}, ColumnDef{"score", ColumnType::FLOAT64},
                    ColumnDef{"emb", ColumnType::VECTOR, /*nullable=*/true, 4u}}});

        cat->CreateTable("orders", Schema{{ColumnDef{"order_id", ColumnType::INT32},
                                           ColumnDef{"user_id", ColumnType::INT32},
                                           ColumnDef{"amount", ColumnType::FLOAT64}}});

        cat->CreateTable("items", Schema{{ColumnDef{"item_id", ColumnType::INT32},
                                          ColumnDef{"order_id", ColumnType::INT32},
                                          ColumnDef{"price", ColumnType::FLOAT64}}});
    }

    void TearDown() override {
        cat.reset();
        sm.reset();

        std::error_code ec;
        std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }

    parser::ast::Statement Parse(const std::string& sql) {
        sink = utils::DiagnosticSink();
        parser::Parser p(sql, sink);
        return p.Parse();
    }

    std::unique_ptr<parser::ast::BoundStatement> Bind(const std::string& sql) {
        auto stmt = Parse(sql);
        binder::Binder b(*cat, &sink);
        return b.Bind(stmt);
    }

    optimizer::LogicalNodePtr Plan(const std::string& sql) {
        auto bound = Bind(sql);
        optimizer::Planner planner;
        return planner.Plan(*bound);
    }

    static std::size_t CountNodesOfType(const optimizer::LogicalNode* node,
                                        optimizer::LogicalNodeType type) {
        if (node == nullptr) {
            return 0;
        }

        std::size_t count = (node->Type() == type) ? 1 : 0;
        for (const auto& child : node->children) {
            count += CountNodesOfType(child.get(), type);
        }
        return count;
    }

    static void ExpectSyntaxErrorWithLocation(BlindP3Test* self, const std::string& sql) {
        try {
            (void)self->Parse(sql);
            FAIL() << "Expected SyntaxException for SQL: " << sql;
        } catch (const utils::SyntaxException& e) {
            ASSERT_GT(e.GetLine(), 0);
            ASSERT_GT(e.GetColumn(), 0);
        } catch (...) {
            FAIL() << "Expected utils::SyntaxException";
        }
    }
};

// -----------------------------------------------------------------------------
// Parser / Lexer / Pratt parser
// -----------------------------------------------------------------------------

TEST_F(BlindP3Test, ParserAcceptsCanonicalSelectClauseOrder) {
    ASSERT_NO_THROW(
        Parse("SELECT age, COUNT(*) "
              "FROM users "
              "WHERE age > 18 "
              "GROUP BY age "
              "HAVING COUNT(*) > 1 "
              "ORDER BY age DESC "
              "LIMIT 10 OFFSET 5;"));
}

TEST_F(BlindP3Test, ParserRejectsHavingBeforeGroupBy) {
    ExpectSyntaxErrorWithLocation(this,
                                  "SELECT age FROM users "
                                  "HAVING age > 10 "
                                  "GROUP BY age;");
}

TEST_F(BlindP3Test, ParserRejectsOrderByBeforeHaving) {
    ExpectSyntaxErrorWithLocation(this,
                                  "SELECT age FROM users "
                                  "GROUP BY age "
                                  "ORDER BY age "
                                  "HAVING age > 10;");
}

TEST_F(BlindP3Test, ParserRejectsWhereAfterOrderBy) {
    ExpectSyntaxErrorWithLocation(this,
                                  "SELECT age FROM users "
                                  "ORDER BY age "
                                  "WHERE age > 10;");
}

TEST_F(BlindP3Test, ParserRejectsLimitBeforeOrderBy) {
    ExpectSyntaxErrorWithLocation(this,
                                  "SELECT age FROM users "
                                  "LIMIT 10 "
                                  "ORDER BY age;");
}

TEST_F(BlindP3Test, ParserRejectsOffsetBeforeLimit) {
    ExpectSyntaxErrorWithLocation(this,
                                  "SELECT age FROM users "
                                  "OFFSET 5 "
                                  "LIMIT 10;");
}

TEST_F(BlindP3Test, SyntaxErrorReportsNonZeroLineAndColumn) {
    try {
        (void)Parse(
            "SELECT *\n"
            "FROM users\n"
            "WHERE ;");
        FAIL() << "Expected SyntaxException";
    } catch (const utils::SyntaxException& e) {
        EXPECT_GT(e.GetLine(), 0);
        EXPECT_GT(e.GetColumn(), 0);
    } catch (...) {
        FAIL() << "Expected utils::SyntaxException";
    }
}

TEST_F(BlindP3Test, ParserAcceptsSelectDistinct) {
    ASSERT_NO_THROW(Parse("SELECT DISTINCT age FROM users;"));
    ASSERT_NO_THROW(Bind("SELECT DISTINCT age FROM users;"));
}

TEST_F(BlindP3Test, ParserAcceptsSelectAll) {
    ASSERT_NO_THROW(Parse("SELECT ALL age FROM users;"));
    ASSERT_NO_THROW(Bind("SELECT ALL age FROM users;"));
}

TEST_F(BlindP3Test, PrattParserAcceptsOrAndPrecedenceForm) {
    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE id = 1 OR age = 20 AND score > 10;"));
}

TEST_F(BlindP3Test, PrattParserRespectsNotVsComparisonThroughSemanticBinding) {
    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE NOT age = 20 AND id = 1;"));
}

TEST_F(BlindP3Test, PrattParserRespectsComparisonVsArithmeticPrecedence) {
    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE score > 1 + 2 * 3;"));
}

TEST_F(BlindP3Test, PrattParserAcceptsLeftAssociativeSubtraction) {
    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE 10 - 5 - 2 = 3;"));
}

TEST_F(BlindP3Test, PrattParserAcceptsExplicitParentheses) {
    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE (10 - 5) - 2 = 3;"));

    ASSERT_NO_THROW(
        Bind("SELECT id FROM users "
             "WHERE 10 - (5 - 2) = 7;"));
}

// -----------------------------------------------------------------------------
// Binder semantic validation
// -----------------------------------------------------------------------------

TEST_F(BlindP3Test, BinderRejectsUnknownTable) {
    EXPECT_THROW(Bind("SELECT * FROM nonexistent;"), utils::SemanticException);
}

TEST_F(BlindP3Test, BinderRejectsUnknownColumn) {
    EXPECT_THROW(Bind("SELECT nonexistent FROM users;"), utils::SemanticException);
}

TEST_F(BlindP3Test, BinderRejectsAmbiguousUnqualifiedColumn) {
    EXPECT_THROW(Bind("SELECT order_id "
                      "FROM orders "
                      "JOIN items ON orders.order_id = items.order_id;"),
                 utils::SemanticException);
}

TEST_F(BlindP3Test, BinderAcceptsQualifiedColumnToResolveAmbiguity) {
    ASSERT_NO_THROW(
        Bind("SELECT orders.order_id "
             "FROM orders "
             "JOIN items ON orders.order_id = items.order_id;"));
}

TEST_F(BlindP3Test, BinderRejectsAggregateInWhereClause) {
    EXPECT_THROW(Bind("SELECT id FROM users WHERE COUNT(*) > 1;"), utils::SemanticException);
}

TEST_F(BlindP3Test, BinderRejectsUngroupedProjectionColumn) {
    EXPECT_THROW(Bind("SELECT id, name, COUNT(*) "
                      "FROM users "
                      "GROUP BY id;"),
                 utils::SemanticException);
}

TEST_F(BlindP3Test, BinderExpandsUnqualifiedWildcardInSchemaOrder) {
    auto plan = Plan("SELECT * FROM users;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    EXPECT_EQ(plan->OutputSchema().size(), 5u);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(plan.get());
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->projections.size(), 5u);

    for (const auto& projection : project->projections) {
        ASSERT_NE(projection.expr, nullptr);
    }
}

TEST_F(BlindP3Test, BinderExpandsQualifiedWildcard) {
    auto plan = Plan("SELECT u.* FROM users u;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    EXPECT_EQ(plan->OutputSchema().size(), 5u);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(plan.get());
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->projections.size(), 5u);

    for (const auto& projection : project->projections) {
        ASSERT_NE(projection.expr, nullptr);
    }
}

TEST_F(BlindP3Test, BinderAcceptsTableAliasAndQualifiedProjection) {
    auto plan = Plan("SELECT u.id, u.name FROM users u;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(plan.get());
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->projections.size(), 2u);

    for (const auto& projection : project->projections) {
        ASSERT_NE(projection.expr, nullptr);
    }
}

TEST_F(BlindP3Test, BinderAcceptsGroupedAggregateQuery) {
    auto bound = Bind(
        "SELECT age, COUNT(*) "
        "FROM users "
        "GROUP BY age;");

    ASSERT_NE(bound, nullptr);
}

TEST_F(BlindP3Test, BinderAcceptHavingOnAggregateExpression) {
    auto bound = Bind(
        "SELECT age, COUNT(*) "
        "FROM users "
        "GROUP BY age "
        "HAVING COUNT(*) > 1;");

    ASSERT_NE(bound, nullptr);
}

// -----------------------------------------------------------------------------
// Planner topology
// -----------------------------------------------------------------------------

TEST_F(BlindP3Test, PlannerProducesProjectOverScanForSimpleSelect) {
    auto plan = Plan("SELECT id, name FROM users;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(plan.get());
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->projections.size(), 2u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(project->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->Type(), optimizer::LogicalNodeType::Scan);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerProducesProjectFilterScanForWhere) {
    auto plan = Plan("SELECT id FROM users WHERE age > 18;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* filter = dynamic_cast<optimizer::LogicalFilter*>(plan->children[0].get());
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(filter->predicate, nullptr);
    ASSERT_EQ(filter->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(filter->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerProducesLimitThenProjectFilterScan) {
    auto plan = Plan(
        "SELECT id "
        "FROM users "
        "WHERE age > 18 "
        "LIMIT 10;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    auto* limit = dynamic_cast<optimizer::LogicalLimit*>(plan.get());
    ASSERT_NE(limit, nullptr);
    ASSERT_NE(limit->limit, nullptr);
    ASSERT_EQ(limit->children.size(), 1u);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(limit->children[0].get());
    ASSERT_NE(project, nullptr);
    ASSERT_EQ(project->children.size(), 1u);

    auto* filter = dynamic_cast<optimizer::LogicalFilter*>(project->children[0].get());
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(filter->predicate, nullptr);
    ASSERT_EQ(filter->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(filter->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerProducesLimitThenSortThenProject) {
    auto plan = Plan(
        "SELECT id, score "
        "FROM users "
        "ORDER BY score DESC "
        "LIMIT 10;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    auto* limit = dynamic_cast<optimizer::LogicalLimit*>(plan.get());
    ASSERT_NE(limit, nullptr);
    ASSERT_NE(limit->limit, nullptr);
    ASSERT_EQ(limit->children.size(), 1u);

    auto* sort = dynamic_cast<optimizer::LogicalSort*>(limit->children[0].get());
    ASSERT_NE(sort, nullptr);
    EXPECT_FALSE(sort->keys.empty());
    ASSERT_EQ(sort->children.size(), 1u);

    auto* project = dynamic_cast<optimizer::LogicalProject*>(sort->children[0].get());
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->projections.size(), 2u);

    ASSERT_EQ(project->children.size(), 1u);
    auto* scan = dynamic_cast<optimizer::LogicalScan*>(project->children[0].get());
    ASSERT_NE(scan, nullptr);
}

TEST_F(BlindP3Test, PlannerSortKeyCarriesDescendingDirection) {
    auto plan = Plan(
        "SELECT id, score "
        "FROM users "
        "ORDER BY score DESC;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Sort);

    auto* sort = dynamic_cast<optimizer::LogicalSort*>(plan.get());
    ASSERT_NE(sort, nullptr);
    ASSERT_EQ(sort->keys.size(), 1u);
    EXPECT_TRUE(sort->keys[0].descending);
    ASSERT_NE(sort->keys[0].expr, nullptr);
}

TEST_F(BlindP3Test, PlannerProducesAggregateForGroupBy) {
    auto plan = Plan(
        "SELECT age, COUNT(*) "
        "FROM users "
        "GROUP BY age;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Aggregate);

    auto* aggregate = dynamic_cast<optimizer::LogicalAggregate*>(plan.get());
    ASSERT_NE(aggregate, nullptr);
    EXPECT_EQ(aggregate->groupBy.size(), 1u);
    ASSERT_FALSE(aggregate->aggregates.empty());

    bool foundAggregateExpression = false;
    for (const auto& aggregate_column : aggregate->aggregates) {
        ASSERT_NE(aggregate_column.expr, nullptr);
        if (aggregate_column.isAggregate) {
            foundAggregateExpression = true;
        }
    }

    EXPECT_TRUE(foundAggregateExpression);
    ASSERT_EQ(aggregate->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(aggregate->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerProducesLimitSortAggregateForGroupedOrderedQuery) {
    auto plan = Plan(
        "SELECT age, COUNT(*) "
        "FROM users "
        "GROUP BY age "
        "HAVING COUNT(*) > 1 "
        "ORDER BY age DESC "
        "LIMIT 10;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    auto* limit = dynamic_cast<optimizer::LogicalLimit*>(plan.get());
    ASSERT_NE(limit, nullptr);
    ASSERT_NE(limit->limit, nullptr);
    ASSERT_EQ(limit->children.size(), 1u);

    auto* sort = dynamic_cast<optimizer::LogicalSort*>(limit->children[0].get());
    ASSERT_NE(sort, nullptr);
    ASSERT_FALSE(sort->keys.empty());
    ASSERT_EQ(sort->children.size(), 1u);

    auto* aggregate = dynamic_cast<optimizer::LogicalAggregate*>(sort->children[0].get());
    ASSERT_NE(aggregate, nullptr);

    EXPECT_EQ(aggregate->groupBy.size(), 1u);
    ASSERT_NE(aggregate->having, nullptr);
    ASSERT_EQ(aggregate->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(aggregate->children[0].get());
    ASSERT_NE(scan, nullptr);
}

TEST_F(BlindP3Test, PlannerBuildsNestedLoopJoinForTwoTables) {
    auto plan = Plan(
        "SELECT orders.order_id, items.price "
        "FROM orders "
        "JOIN items ON orders.order_id = items.order_id;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* join = dynamic_cast<optimizer::LogicalNestedLoopJoin*>(plan->children[0].get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->joinType, parser::ast::JoinType::INNER);
    ASSERT_NE(join->condition, nullptr);
    ASSERT_EQ(join->children.size(), 2u);

    auto* left = dynamic_cast<optimizer::LogicalScan*>(join->children[0].get());
    auto* right = dynamic_cast<optimizer::LogicalScan*>(join->children[1].get());

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    EXPECT_EQ(left->tableName, "orders");
    EXPECT_EQ(right->tableName, "items");
}

TEST_F(BlindP3Test, PlannerBuildsTwoNestedJoinsForThreeTables) {
    auto plan = Plan(
        "SELECT users.id, orders.order_id, items.item_id "
        "FROM users "
        "JOIN orders ON users.id = orders.user_id "
        "JOIN items ON orders.order_id = items.order_id;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);

    EXPECT_EQ(CountNodesOfType(plan.get(), optimizer::LogicalNodeType::NestedLoopJoin), 2u);
}

TEST_F(BlindP3Test, PlannerPreservesLeftJoinType) {
    auto plan = Plan(
        "SELECT users.id, orders.order_id "
        "FROM users "
        "LEFT JOIN orders ON users.id = orders.user_id;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* join = dynamic_cast<optimizer::LogicalNestedLoopJoin*>(plan->children[0].get());
    ASSERT_NE(join, nullptr);

    EXPECT_EQ(join->joinType, parser::ast::JoinType::LEFT);
    ASSERT_NE(join->condition, nullptr);
}

// -----------------------------------------------------------------------------
// UPDATE / DELETE / INSERT
// -----------------------------------------------------------------------------

TEST_F(BlindP3Test, PlannerUpdateWithWhereProducesUpdateFilterScan) {
    auto plan = Plan(
        "UPDATE users "
        "SET age = 42 "
        "WHERE id = 1;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Update);

    auto* update = dynamic_cast<optimizer::LogicalUpdate*>(plan.get());
    ASSERT_NE(update, nullptr);
    EXPECT_EQ(update->tableName, "users");
    ASSERT_FALSE(update->assignments.empty());
    ASSERT_EQ(update->children.size(), 1u);

    auto* filter = dynamic_cast<optimizer::LogicalFilter*>(update->children[0].get());
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(filter->predicate, nullptr);
    ASSERT_EQ(filter->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(filter->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerUpdateWithoutWhereProducesUpdateOverScan) {
    auto plan = Plan(
        "UPDATE users "
        "SET age = 42;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Update);

    auto* update = dynamic_cast<optimizer::LogicalUpdate*>(plan.get());
    ASSERT_NE(update, nullptr);
    EXPECT_EQ(update->tableName, "users");
    ASSERT_FALSE(update->assignments.empty());

    ASSERT_EQ(update->children.size(), 1u);
    auto* scan = dynamic_cast<optimizer::LogicalScan*>(update->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerDeleteWithWhereProducesDeleteFilterScan) {
    auto plan = Plan(
        "DELETE FROM users "
        "WHERE age < 18;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Delete);

    auto* del = dynamic_cast<optimizer::LogicalDelete*>(plan.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->tableName, "users");
    ASSERT_EQ(del->children.size(), 1u);

    auto* filter = dynamic_cast<optimizer::LogicalFilter*>(del->children[0].get());
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(filter->predicate, nullptr);
    ASSERT_EQ(filter->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(filter->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerDeleteWithoutWhereProducesDeleteOverScan) {
    auto plan = Plan("DELETE FROM users;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Delete);

    auto* del = dynamic_cast<optimizer::LogicalDelete*>(plan.get());
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->tableName, "users");

    ASSERT_EQ(del->children.size(), 1u);
    auto* scan = dynamic_cast<optimizer::LogicalScan*>(del->children[0].get());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->tableName, "users");
}

TEST_F(BlindP3Test, PlannerInsertBuildsInsertNodeWithTwoRows) {
    auto plan = Plan(
        // score is listed because it is NOT NULL; emb is nullable (no vector literal yet).
        "INSERT INTO users (id, name, age, score) "
        "VALUES (1, 'alice', 25, 1.5), (2, 'bob', 30, 2.5);");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Insert);

    auto* insert = dynamic_cast<optimizer::LogicalInsert*>(plan.get());
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->tableName, "users");
    EXPECT_EQ(insert->rows.size(), 2u);

    auto output = insert->OutputSchema();
    ASSERT_EQ(output.size(), 1u);
}

TEST_F(BlindP3Test, PlannerLimitOffsetSetsBothExpressions) {
    auto plan = Plan(
        "SELECT id "
        "FROM users "
        "LIMIT 10 OFFSET 5;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Limit);

    auto* limit = dynamic_cast<optimizer::LogicalLimit*>(plan.get());
    ASSERT_NE(limit, nullptr);

    ASSERT_NE(limit->limit, nullptr);
    ASSERT_NE(limit->offset, nullptr);

    ASSERT_EQ(limit->children.size(), 1u);
    EXPECT_EQ(limit->children[0]->Type(), optimizer::LogicalNodeType::Project);
}

TEST_F(BlindP3Test, PlannerCanonicalPipelineIncludesJoinBeforeFilterAndProject) {
    auto plan = Plan(
        "SELECT users.id "
        "FROM users "
        "JOIN orders ON users.id = orders.user_id "
        "WHERE orders.amount > 100;");

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->Type(), optimizer::LogicalNodeType::Project);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* filter = dynamic_cast<optimizer::LogicalFilter*>(plan->children[0].get());
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(filter->predicate, nullptr);
    ASSERT_EQ(filter->children.size(), 1u);

    auto* join = dynamic_cast<optimizer::LogicalNestedLoopJoin*>(filter->children[0].get());
    ASSERT_NE(join, nullptr);
    ASSERT_NE(join->condition, nullptr);
    ASSERT_EQ(join->children.size(), 2u);
}

TEST_F(BlindP3Test, PlannerPreservesScanMetadata) {
    auto plan = Plan("SELECT id FROM users;");

    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->children.size(), 1u);

    auto* scan = dynamic_cast<optimizer::LogicalScan*>(plan->children[0].get());
    ASSERT_NE(scan, nullptr);

    EXPECT_EQ(scan->tableName, "users");
    EXPECT_EQ(scan->schema.size(), 5u);
    EXPECT_FALSE(scan->alias.has_value());
}
