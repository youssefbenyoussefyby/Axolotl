#include <gtest/gtest.h>
#include <parser/ast.h>

#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB::parser;
using namespace AxolotlDB::parser::ast;

// ===========================================================================
// 3.2 AST Tests from P3.md Checklist
// ===========================================================================

TEST(AstTest, EveryNodeCarriesPosition) {
    // 1. Statements carry source position
    SelectStmt select;
    Stmt selectStmt(std::move(select), 10, 4);
    EXPECT_EQ(selectStmt.line, 10u);
    EXPECT_EQ(selectStmt.column, 4u);

    InsertStmt insert;
    Stmt insertStmt(std::move(insert), 15, 1);
    EXPECT_EQ(insertStmt.line, 15u);
    EXPECT_EQ(insertStmt.column, 1u);

    UpdateStmt update;
    Stmt updateStmt(std::move(update), 20, 8);
    EXPECT_EQ(updateStmt.line, 20u);
    EXPECT_EQ(updateStmt.column, 8u);

    DeleteStmt del;
    Stmt deleteStmt(std::move(del), 25, 2);
    EXPECT_EQ(deleteStmt.line, 25u);
    EXPECT_EQ(deleteStmt.column, 2u);

    CreateTableStmt create;
    Stmt createStmt(std::move(create), 30, 1);
    EXPECT_EQ(createStmt.line, 30u);
    EXPECT_EQ(createStmt.column, 1u);

    DropTableStmt drop;
    Stmt dropStmt(std::move(drop), 35, 5);
    EXPECT_EQ(dropStmt.line, 35u);
    EXPECT_EQ(dropStmt.column, 5u);

    // 2. Expressions carry source position
    Expr intLit(Literal{int64_t{42}}, 1, 10);
    EXPECT_EQ(intLit.line, 1u);
    EXPECT_EQ(intLit.column, 10u);
    EXPECT_TRUE(std::holds_alternative<Literal>(intLit.node));

    Expr colRef(ColumnRef{"users", "id"}, 2, 7);
    EXPECT_EQ(colRef.line, 2u);
    EXPECT_EQ(colRef.column, 7u);
    EXPECT_TRUE(std::holds_alternative<ColumnRef>(colRef.node));

    Expr star(Star{std::string{"u"}}, 3, 12);
    EXPECT_EQ(star.line, 3u);
    EXPECT_EQ(star.column, 12u);
    EXPECT_TRUE(std::holds_alternative<Star>(star.node));

    Expr unaryOp(UnaryOp{UnaryOperator::NOT, std::make_unique<Expr>(Literal{true}, 4, 15)}, 4, 11);
    EXPECT_EQ(unaryOp.line, 4u);
    EXPECT_EQ(unaryOp.column, 11u);
    EXPECT_TRUE(std::holds_alternative<UnaryOp>(unaryOp.node));

    Expr binaryOp(BinaryOp{BinaryOperator::ADD, std::make_unique<Expr>(Literal{int64_t{1}}, 5, 1),
                           std::make_unique<Expr>(Literal{int64_t{2}}, 5, 5)},
                  5, 3);
    EXPECT_EQ(binaryOp.line, 5u);
    EXPECT_EQ(binaryOp.column, 3u);
    EXPECT_TRUE(std::holds_alternative<BinaryOp>(binaryOp.node));

    Expr funcCall(FunctionCall{"COUNT", {}}, 6, 1);
    EXPECT_EQ(funcCall.line, 6u);
    EXPECT_EQ(funcCall.column, 1u);
    EXPECT_TRUE(std::holds_alternative<FunctionCall>(funcCall.node));

    // 3. Column definitions carry source position
    ColumnDef colDef;
    colDef.name = "id";
    colDef.typeName = "INT32";
    colDef.line = 7;
    colDef.column = 5;
    EXPECT_EQ(colDef.line, 7u);
    EXPECT_EQ(colDef.column, 5u);

    // 4. Table references carry source position and identifier
    TableRef tblRef("users", 1, 15);
    EXPECT_EQ(tblRef.table.name, "users");
    EXPECT_EQ(tblRef.table.line, 1u);
    EXPECT_EQ(tblRef.table.column, 15u);
    EXPECT_FALSE(tblRef.alias.has_value());

    TableRef aliasedTbl("orders", "o", 2, 6);
    EXPECT_EQ(aliasedTbl.table.name, "orders");
    EXPECT_EQ(aliasedTbl.table.line, 2u);
    EXPECT_EQ(aliasedTbl.table.column, 6u);
    ASSERT_TRUE(aliasedTbl.alias.has_value());
    EXPECT_EQ(*aliasedTbl.alias, "o");
}

TEST(AstTest, PrinterRoundTripsSimpleSelect) {
    // Construct AST for: SELECT id, name FROM users WHERE id = 1
    SelectStmt select;
    select.projection.push_back(SelectItem(std::make_unique<Expr>(ColumnRef{"id"})));
    select.projection.push_back(SelectItem(std::make_unique<Expr>(ColumnRef{"name"})));
    select.fromTable = "users";

    auto lhs = std::make_unique<Expr>(ColumnRef{"id"});
    auto rhs = std::make_unique<Expr>(Literal{int64_t{1}});
    select.where =
        std::make_unique<Expr>(BinaryOp{BinaryOperator::EQUAL, std::move(lhs), std::move(rhs)});

    Stmt stmt(std::move(select));
    EXPECT_EQ(ToString(stmt), "SELECT id, name FROM users WHERE id = 1");
}

TEST(AstTest, PrinterRoundTripsComplexSelect) {
    // Construct Target Grammar statement from P3.md:
    // SELECT a.id, COUNT(*) FROM a JOIN b ON a.id = b.a_id
    //   WHERE a.x > 5 AND b.y = 'z'
    //   GROUP BY a.id HAVING COUNT(*) > 2
    //   ORDER BY a.id DESC LIMIT 10
    SelectStmt select;
    select.projection.push_back(SelectItem(std::make_unique<Expr>(ColumnRef{"a", "id"})));

    std::vector<ExprPtr> countArgs;
    countArgs.push_back(std::make_unique<Expr>(Star{}));
    select.projection.push_back(
        SelectItem(std::make_unique<Expr>(FunctionCall{"COUNT", std::move(countArgs)})));

    select.fromTable = "a";

    // JOIN b ON a.id = b.a_id
    auto joinLhs = std::make_unique<Expr>(ColumnRef{"a", "id"});
    auto joinRhs = std::make_unique<Expr>(ColumnRef{"b", "a_id"});
    auto onCond = std::make_unique<Expr>(
        BinaryOp{BinaryOperator::EQUAL, std::move(joinLhs), std::move(joinRhs)});
    select.joins.push_back(JoinClause("b", std::move(onCond), JoinType::INNER));

    // WHERE a.x > 5 AND b.y = 'z'
    auto cond1 = std::make_unique<Expr>(BinaryOp{BinaryOperator::GREATER,
                                                 std::make_unique<Expr>(ColumnRef{"a", "x"}),
                                                 std::make_unique<Expr>(Literal{int64_t{5}})});
    auto cond2 = std::make_unique<Expr>(
        BinaryOp{BinaryOperator::EQUAL, std::make_unique<Expr>(ColumnRef{"b", "y"}),
                 std::make_unique<Expr>(Literal{std::string{"z"}})});
    select.where =
        std::make_unique<Expr>(BinaryOp{BinaryOperator::AND, std::move(cond1), std::move(cond2)});

    // GROUP BY a.id
    select.groupBy.push_back(std::make_unique<Expr>(ColumnRef{"a", "id"}));

    // HAVING COUNT(*) > 2
    std::vector<ExprPtr> havingCountArgs;
    havingCountArgs.push_back(std::make_unique<Expr>(Star{}));
    select.having = std::make_unique<Expr>(
        BinaryOp{BinaryOperator::GREATER,
                 std::make_unique<Expr>(FunctionCall{"COUNT", std::move(havingCountArgs)}),
                 std::make_unique<Expr>(Literal{int64_t{2}})});

    // ORDER BY a.id DESC
    select.orderBy.push_back(OrderByItem(std::make_unique<Expr>(ColumnRef{"a", "id"}), true));

    // LIMIT 10
    select.limit = std::make_unique<Expr>(Literal{int64_t{10}});

    Stmt stmt(std::move(select));
    std::string expected =
        "SELECT a.id, COUNT(*) FROM a JOIN b ON a.id = b.a_id "
        "WHERE a.x > 5 AND b.y = 'z' "
        "GROUP BY a.id HAVING COUNT(*) > 2 "
        "ORDER BY a.id DESC LIMIT 10";

    EXPECT_EQ(ToString(stmt), expected);
}

TEST(AstTest, PrinterRoundTripsInsertStatement) {
    InsertStmt insert;
    insert.table = "t";
    insert.columns = std::vector<std::string>{"id", "name"};

    std::vector<ExprPtr> row1;
    row1.push_back(std::make_unique<Expr>(Literal{int64_t{1}}));
    row1.push_back(std::make_unique<Expr>(Literal{std::string{"a"}}));

    std::vector<ExprPtr> row2;
    row2.push_back(std::make_unique<Expr>(Literal{int64_t{2}}));
    row2.push_back(std::make_unique<Expr>(Literal{std::string{"b"}}));

    insert.valueRows.push_back(std::move(row1));
    insert.valueRows.push_back(std::move(row2));

    Stmt stmt(std::move(insert));
    EXPECT_EQ(ToString(stmt), "INSERT INTO t (id, name) VALUES (1, 'a'), (2, 'b')");
}

TEST(AstTest, PrinterRoundTripsUpdateAndDeleteStatements) {
    // UPDATE t SET name = 'c' WHERE id = 1
    UpdateStmt update;
    update.table = "t";
    update.assignments.push_back({"name", std::make_unique<Expr>(Literal{std::string{"c"}})});
    update.where = std::make_unique<Expr>(BinaryOp{BinaryOperator::EQUAL,
                                                   std::make_unique<Expr>(ColumnRef{"id"}),
                                                   std::make_unique<Expr>(Literal{int64_t{1}})});

    Stmt updateStmt(std::move(update));
    EXPECT_EQ(ToString(updateStmt), "UPDATE t SET name = 'c' WHERE id = 1");

    // DELETE FROM t WHERE id > 5
    DeleteStmt del;
    del.table = "t";
    del.where = std::make_unique<Expr>(BinaryOp{BinaryOperator::GREATER,
                                                std::make_unique<Expr>(ColumnRef{"id"}),
                                                std::make_unique<Expr>(Literal{int64_t{5}})});

    Stmt delStmt(std::move(del));
    EXPECT_EQ(ToString(delStmt), "DELETE FROM t WHERE id > 5");
}

TEST(AstTest, PrinterRoundTripsCreateTableAndDropTable) {
    // CREATE TABLE t (id INT32 PRIMARY KEY, name VARCHAR(64), emb VECTOR(768) NOT NULL)
    CreateTableStmt create;
    create.table = "t";

    ColumnDef c1;
    c1.name = "id";
    c1.typeName = "INT32";
    c1.isPrimaryKey = true;
    create.columns.push_back(c1);

    ColumnDef c2;
    c2.name = "name";
    c2.typeName = "VARCHAR";
    c2.typeSize = 64;
    create.columns.push_back(c2);

    ColumnDef c3;
    c3.name = "emb";
    c3.typeName = "VECTOR";
    c3.typeSize = 768;
    c3.isNotNull = true;
    create.columns.push_back(c3);

    Stmt createStmt(std::move(create));
    EXPECT_EQ(ToString(createStmt),
              "CREATE TABLE t (id INT32 PRIMARY KEY, name VARCHAR(64), emb VECTOR(768) NOT NULL)");

    // DROP TABLE t
    DropTableStmt drop;
    drop.table = "t";
    Stmt dropStmt(std::move(drop));
    EXPECT_EQ(ToString(dropStmt), "DROP TABLE t");
}

TEST(AstTest, ExpressionsAndOperators) {
    // Unary prefix: NOT active
    Expr notExpr(UnaryOp{UnaryOperator::NOT, std::make_unique<Expr>(ColumnRef{"active"})});
    EXPECT_EQ(ToString(notExpr), "NOT active");

    // Unary minus: -x
    Expr negExpr(UnaryOp{UnaryOperator::NEGATE, std::make_unique<Expr>(ColumnRef{"x"})});
    EXPECT_EQ(ToString(negExpr), "-x");

    // Vector L2 distance: vec1 <-> vec2
    Expr l2Expr(BinaryOp{BinaryOperator::VECTOR_L2, std::make_unique<Expr>(ColumnRef{"vec1"}),
                         std::make_unique<Expr>(ColumnRef{"vec2"})});
    EXPECT_EQ(ToString(l2Expr), "vec1 <-> vec2");

    // Vector cosine distance: vec1 <=> vec2
    Expr cosExpr(BinaryOp{BinaryOperator::VECTOR_COSINE, std::make_unique<Expr>(ColumnRef{"vec1"}),
                          std::make_unique<Expr>(ColumnRef{"vec2"})});
    EXPECT_EQ(ToString(cosExpr), "vec1 <=> vec2");

    // Qualified star: t.*
    Expr starExpr(Star{std::string{"t"}});
    EXPECT_EQ(ToString(starExpr), "t.*");

    // Literal null and boolean
    Expr nullExpr(Literal{nullptr});
    EXPECT_EQ(ToString(nullExpr), "NULL");

    Expr boolExpr(Literal{true});
    EXPECT_EQ(ToString(boolExpr), "true");
}
