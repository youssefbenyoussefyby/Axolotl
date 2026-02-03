#include <gtest/gtest.h>
#include <parser/parser.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>

using namespace AxolotlDB;
using namespace AxolotlDB::parser;
using namespace AxolotlDB::parser::ast;

namespace {

Statement Parse(std::string_view sql)
{
    utils::DiagnosticSink sink;
    Parser parser(sql, sink);
    return parser.Parse();
}

}

TEST(ParserTest, CreateTableWithAllColumnTypes)
{
    std::string sql = "CREATE TABLE t (id INT32 PRIMARY KEY, name VARCHAR(64), active BOOLEAN, price FLOAT64 NOT NULL);";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<CreateTableStatement>(stmt.node));
    const auto& create = std::get<CreateTableStatement>(stmt.node);
    EXPECT_EQ(create.table.name, "t");
    ASSERT_EQ(create.columns.size(), 4u);

    EXPECT_EQ(create.columns[0].name, "id");
    EXPECT_EQ(create.columns[0].typeName, "INT32");
    EXPECT_TRUE(create.columns[0].isPrimaryKey);

    EXPECT_EQ(create.columns[1].name, "name");
    EXPECT_EQ(create.columns[1].typeName, "VARCHAR");
    ASSERT_TRUE(create.columns[1].typeSize.has_value());
    EXPECT_EQ(*create.columns[1].typeSize, 64u);

    EXPECT_EQ(create.columns[2].name, "active");
    EXPECT_EQ(create.columns[2].typeName, "BOOLEAN");

    EXPECT_EQ(create.columns[3].name, "price");
    EXPECT_EQ(create.columns[3].typeName, "FLOAT64");
    EXPECT_TRUE(create.columns[3].isNotNull);
}

TEST(ParserTest, VectorColumnCarriesDimension)
{
    std::string sql = "CREATE TABLE items (emb VECTOR(768));";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<CreateTableStatement>(stmt.node));
    const auto& create = std::get<CreateTableStatement>(stmt.node);
    ASSERT_EQ(create.columns.size(), 1u);
    EXPECT_EQ(create.columns[0].name, "emb");
    EXPECT_EQ(create.columns[0].typeName, "VECTOR");
    ASSERT_TRUE(create.columns[0].typeSize.has_value());
    EXPECT_EQ(*create.columns[0].typeSize, 768u);
}

TEST(ParserTest, InsertMultipleValueTuples)
{
    std::string sql = "INSERT INTO t (id, name) VALUES (1, 'a'), (2, 'b');";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<InsertStatement>(stmt.node));
    const auto& insert = std::get<InsertStatement>(stmt.node);
    EXPECT_EQ(insert.table.name, "t");
    ASSERT_TRUE(insert.columns.has_value());
    ASSERT_EQ(insert.columns->size(), 2u);
    EXPECT_EQ((*insert.columns)[0], "id");
    EXPECT_EQ((*insert.columns)[1], "name");

    ASSERT_EQ(insert.valueRows.size(), 2u);
    EXPECT_EQ(insert.valueRows[0].size(), 2u);
    EXPECT_EQ(insert.valueRows[1].size(), 2u);
}

TEST(ParserTest, InsertArityMismatchIsParsedNotRejected)
{
    std::string sql = "INSERT INTO t (id, name) VALUES (1);";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<InsertStatement>(stmt.node));
    const auto& insert = std::get<InsertStatement>(stmt.node);
    ASSERT_TRUE(insert.columns.has_value());
    EXPECT_EQ(insert.columns->size(), 2u);
    ASSERT_EQ(insert.valueRows.size(), 1u);
    EXPECT_EQ(insert.valueRows[0].size(), 1u);
}

TEST(ParserTest, QualifiedColumnReference)
{
    std::string sql = "SELECT a.id, b.name FROM a;";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 2u);

    ASSERT_TRUE(std::holds_alternative<ColumnRef>(select.projection[0].expr->node));
    const auto& col1 = std::get<ColumnRef>(select.projection[0].expr->node);
    ASSERT_TRUE(col1.table.has_value());
    EXPECT_EQ(*col1.table, "a");
    EXPECT_EQ(col1.column, "id");

    ASSERT_TRUE(std::holds_alternative<ColumnRef>(select.projection[1].expr->node));
    const auto& col2 = std::get<ColumnRef>(select.projection[1].expr->node);
    ASSERT_TRUE(col2.table.has_value());
    EXPECT_EQ(*col2.table, "b");
    EXPECT_EQ(col2.column, "name");
}

TEST(ParserTest, TableAlias)
{
    std::string sql = "SELECT u.id FROM users AS u;";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_TRUE(select.fromTable.has_value());
    EXPECT_EQ(select.fromTable->table.name, "users");
    ASSERT_TRUE(select.fromTable->alias.has_value());
    EXPECT_EQ(*select.fromTable->alias, "u");
}

TEST(ParserTest, InnerJoinWithOnCondition)
{
    std::string sql = "SELECT a.id FROM a JOIN b ON a.id = b.a_id;";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.joins.size(), 1u);
    EXPECT_EQ(select.joins[0].table.table.name, "b");
    EXPECT_EQ(select.joins[0].type, JoinType::INNER);
    ASSERT_NE(select.joins[0].onCondition, nullptr);
    EXPECT_TRUE(std::holds_alternative<BinaryOp>(select.joins[0].onCondition->node));
}

TEST(ParserTest, FullSelectWithEveryClause)
{
    std::string sql =
        "SELECT a.id, COUNT(*) FROM a JOIN b ON a.id = b.a_id "
        "WHERE a.x > 5 AND b.y = 'z' "
        "GROUP BY a.id HAVING COUNT(*) > 2 "
        "ORDER BY a.id DESC LIMIT 10;";
    Statement stmt = Parse(sql);

    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_TRUE(select.fromTable.has_value());
    EXPECT_EQ(select.fromTable->table.name, "a");
    ASSERT_EQ(select.joins.size(), 1u);
    EXPECT_EQ(select.joins[0].table.table.name, "b");
    ASSERT_NE(select.where, nullptr);
    ASSERT_EQ(select.groupBy.size(), 1u);
    ASSERT_NE(select.having, nullptr);
    ASSERT_EQ(select.orderBy.size(), 1u);
    EXPECT_TRUE(select.orderBy[0].isDescending);
    ASSERT_NE(select.limit, nullptr);
}

TEST(ParserTest, ClausesOutOfOrderIsASyntaxError)
{
    std::string sql = "SELECT a.id FROM a LIMIT 10 WHERE a.x > 5;";
    utils::DiagnosticSink sink;
    Parser parser(sql, sink);
    EXPECT_THROW(parser.Parse(), utils::AxolotException);
    EXPECT_TRUE(sink.hasError());
}

TEST(ParserTest, AndBindsTighterThanOr)
{
    std::string sql = "SELECT a OR b AND c FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::OR);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(root.right->node));
    const auto& right = std::get<BinaryOp>(root.right->node);
    EXPECT_EQ(right.biop, BinaryOperator::AND);
}

TEST(ParserTest, ComparisonBindsTighterThanAnd)
{
    std::string sql = "SELECT a AND b = c FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::AND);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(root.right->node));
    const auto& right = std::get<BinaryOp>(root.right->node);
    EXPECT_EQ(right.biop, BinaryOperator::EQUAL);
}

TEST(ParserTest, MultiplicationBindsTighterThanAddition)
{
    std::string sql = "SELECT a + b * c FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::ADD);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(root.right->node));
    const auto& right = std::get<BinaryOp>(root.right->node);
    EXPECT_EQ(right.biop, BinaryOperator::MULTIPLY);
}

TEST(ParserTest, SubtractionIsLeftAssociative)
{
    std::string sql = "SELECT 1 - 2 - 3 FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::SUBTRACT);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(root.left->node));
    const auto& left = std::get<BinaryOp>(root.left->node);
    EXPECT_EQ(left.biop, BinaryOperator::SUBTRACT);
    ASSERT_TRUE(std::holds_alternative<Literal>(root.right->node));
    EXPECT_EQ(std::get<int64_t>(std::get<Literal>(root.right->node).value), 3);
}

TEST(ParserTest, ParenthesesOverridePrecedence)
{
    std::string sql = "SELECT (a + b) * c FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::MULTIPLY);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(root.left->node));
    const auto& left = std::get<BinaryOp>(root.left->node);
    EXPECT_EQ(left.biop, BinaryOperator::ADD);
}

TEST(ParserTest, UnaryMinusBindsTighterThanMultiplication)
{
    std::string sql = "SELECT -a * b FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<BinaryOp>(select.projection[0].expr->node));
    const auto& root = std::get<BinaryOp>(select.projection[0].expr->node);
    EXPECT_EQ(root.biop, BinaryOperator::MULTIPLY);
    ASSERT_TRUE(std::holds_alternative<UnaryOp>(root.left->node));
    const auto& left = std::get<UnaryOp>(root.left->node);
    EXPECT_EQ(left.uop, UnaryOperator::NEGATE);
}

TEST(ParserTest, CountStarIsDistinctFromCountColumn)
{
    std::string sql = "SELECT COUNT(*), COUNT(x) FROM t;";
    Statement stmt = Parse(sql);
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmt.node));
    const auto& select = std::get<SelectStatement>(stmt.node);
    ASSERT_EQ(select.projection.size(), 2u);

    ASSERT_TRUE(std::holds_alternative<FunctionCall>(select.projection[0].expr->node));
    const auto& fn1 = std::get<FunctionCall>(select.projection[0].expr->node);
    EXPECT_EQ(fn1.name, "COUNT");
    ASSERT_EQ(fn1.args.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<Star>(fn1.args[0]->node));

    ASSERT_TRUE(std::holds_alternative<FunctionCall>(select.projection[1].expr->node));
    const auto& fn2 = std::get<FunctionCall>(select.projection[1].expr->node);
    EXPECT_EQ(fn2.name, "COUNT");
    ASSERT_EQ(fn2.args.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ColumnRef>(fn2.args[0]->node));
}

TEST(ParserTest, MissingFromReportsExpectedFrom)
{
    std::string sql = "SELECT a.id WHERE a.x > 5;";
    utils::DiagnosticSink sink;
    Parser parser(sql, sink);
    try {
        (void)parser.Parse();
        FAIL() << "Expected SyntaxException";
    } catch (const utils::SyntaxException& e) {
        EXPECT_NE(std::string(e.what()).find("Expected FROM"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("WHERE"), std::string::npos);
        EXPECT_EQ(e.GetLine(), 1u);
        EXPECT_EQ(e.GetColumn(), 13u);
        EXPECT_EQ(e.GetToken(), "WHERE");
    }
    EXPECT_TRUE(sink.hasError());
    const auto& diags = sink.getDiagnostics();
    ASSERT_FALSE(diags.empty());
    EXPECT_NE(diags[0].message.find("Expected FROM"), std::string::npos);
    EXPECT_NE(diags[0].message.find("WHERE"), std::string::npos);
    EXPECT_EQ(diags[0].line, 1u);
    EXPECT_EQ(diags[0].column, 13u);
    EXPECT_EQ(diags[0].lexem, "WHERE");
}

TEST(ParserTest, ErrorPositionPointsAtOffendingToken)
{
    {
        std::string sql = "SELECT a.id WHERE a.x > 5;";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        try {
            (void)parser.Parse();
            FAIL() << "Expected SyntaxException";
        } catch (const utils::SyntaxException& e) {
            EXPECT_EQ(e.GetLine(), 1u);
            EXPECT_EQ(e.GetColumn(), 13u);
            EXPECT_EQ(e.GetToken(), "WHERE");
        }
        ASSERT_TRUE(sink.hasError());
        EXPECT_EQ(sink.getDiagnostics()[0].line, 1u);
        EXPECT_EQ(sink.getDiagnostics()[0].column, 13u);
        EXPECT_EQ(sink.getDiagnostics()[0].lexem, "WHERE");
    }
    {
        std::string sql = "INSERT t VALUES (1);";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        try {
            (void)parser.Parse();
            FAIL() << "Expected SyntaxException";
        } catch (const utils::SyntaxException& e) {
            EXPECT_EQ(e.GetLine(), 1u);
            EXPECT_EQ(e.GetColumn(), 8u);
            EXPECT_EQ(e.GetToken(), "t");
        }
        ASSERT_TRUE(sink.hasError());
        EXPECT_EQ(sink.getDiagnostics()[0].line, 1u);
        EXPECT_EQ(sink.getDiagnostics()[0].column, 8u);
        EXPECT_EQ(sink.getDiagnostics()[0].lexem, "t");
    }
    {
        std::string sql = "UPDATE t WHERE id = 1;";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        try {
            (void)parser.Parse();
            FAIL() << "Expected SyntaxException";
        } catch (const utils::SyntaxException& e) {
            EXPECT_EQ(e.GetLine(), 1u);
            EXPECT_EQ(e.GetColumn(), 10u);
            EXPECT_EQ(e.GetToken(), "WHERE");
        }
        ASSERT_TRUE(sink.hasError());
        EXPECT_EQ(sink.getDiagnostics()[0].line, 1u);
        EXPECT_EQ(sink.getDiagnostics()[0].column, 10u);
        EXPECT_EQ(sink.getDiagnostics()[0].lexem, "WHERE");
    }
    {
        std::string sql = "DROP t;";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        try {
            (void)parser.Parse();
            FAIL() << "Expected SyntaxException";
        } catch (const utils::SyntaxException& e) {
            EXPECT_EQ(e.GetLine(), 1u);
            EXPECT_EQ(e.GetColumn(), 6u);
            EXPECT_EQ(e.GetToken(), "t");
        }
        ASSERT_TRUE(sink.hasError());
        EXPECT_EQ(sink.getDiagnostics()[0].line, 1u);
        EXPECT_EQ(sink.getDiagnostics()[0].column, 6u);
        EXPECT_EQ(sink.getDiagnostics()[0].lexem, "t");
    }
    {
        std::string sql = "CREATE TABLE t (id);";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        try {
            (void)parser.Parse();
            FAIL() << "Expected SyntaxException";
        } catch (const utils::SyntaxException& e) {
            EXPECT_EQ(e.GetLine(), 1u);
            EXPECT_EQ(e.GetColumn(), 19u);
            EXPECT_EQ(e.GetToken(), ")");
        }
        ASSERT_TRUE(sink.hasError());
        EXPECT_EQ(sink.getDiagnostics()[0].line, 1u);
        EXPECT_EQ(sink.getDiagnostics()[0].column, 19u);
        EXPECT_EQ(sink.getDiagnostics()[0].lexem, ")");
    }
}

TEST(ParserTest, RecoversAtStatementBoundary)
{
    {
        std::string sql = "SELECT a.id WHERE a.x > 5; SELECT b.id FROM b;";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        auto stmts = parser.ParseStatements();
        ASSERT_EQ(stmts.size(), 1u);
        ASSERT_TRUE(std::holds_alternative<SelectStatement>(stmts[0].node));
        const auto& select = std::get<SelectStatement>(stmts[0].node);
        ASSERT_TRUE(select.fromTable.has_value());
        EXPECT_EQ(select.fromTable->table.name, "b");

        EXPECT_TRUE(sink.hasError());
        const auto& diags = sink.getDiagnostics();
        ASSERT_EQ(diags.size(), 1u);
        EXPECT_EQ(diags[0].line, 1u);
        EXPECT_EQ(diags[0].column, 13u);
        EXPECT_EQ(diags[0].lexem, "WHERE");
    }
    {
        std::string sql = "SELECT a.id WHERE a.x > 5; DROP t;";
        utils::DiagnosticSink sink;
        Parser parser(sql, sink);
        auto stmts = parser.ParseStatements();
        EXPECT_TRUE(stmts.empty());
        EXPECT_TRUE(sink.hasError());
        const auto& diags = sink.getDiagnostics();
        ASSERT_EQ(diags.size(), 2u);
        EXPECT_EQ(diags[0].line, 1u);
        EXPECT_EQ(diags[0].column, 13u);
        EXPECT_EQ(diags[0].lexem, "WHERE");
        EXPECT_EQ(diags[1].line, 1u);
        EXPECT_EQ(diags[1].column, 33u);
        EXPECT_EQ(diags[1].lexem, "t");
    }
}
