#include <catalog/catalog.h>
#include <gtest/gtest.h>
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
#include <vector>

using namespace AxolotlDB;
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::Schema;

struct BinderTest : ::testing::Test {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    std::unique_ptr<storage::StorageManager> sm;
    std::unique_ptr<catalog::Catalog> cat;
    utils::DiagnosticSink sink;

    void SetUp() override {
        orig_dir = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
              ("axo_bind_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);

        sm = std::make_unique<storage::StorageManager>(32);
        cat = std::make_unique<catalog::Catalog>(*sm);

        Schema userSchema{{ColumnDef{"id", ColumnType::INT32},
                           ColumnDef{"name", ColumnType::VARCHAR},
                           ColumnDef{"age", ColumnType::INT32}}};
        cat->CreateTable("users", userSchema);

        Schema orderSchema{{ColumnDef{"order_id", ColumnType::INT32},
                            ColumnDef{"user_id", ColumnType::INT32},
                            ColumnDef{"amount", ColumnType::FLOAT64}}};
        cat->CreateTable("orders", orderSchema);

        Schema accountSchema{
            {ColumnDef{"id", ColumnType::INT32}, ColumnDef{"balance", ColumnType::FLOAT64}}};
        cat->CreateTable("accounts", accountSchema);
    }

    void TearDown() override {
        cat.reset();
        sm.reset();
        std::error_code ec;
        std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }

    parser::ast::Statement ParseSql(const std::string& sql) {
        sink = utils::DiagnosticSink();
        parser::Parser p(sql, sink);
        return p.Parse();
    }

    std::unique_ptr<parser::ast::BoundStatement> BindSql(const std::string& sql) {
        auto stmt = ParseSql(sql);
        binder::Binder b(*cat, &sink);
        return b.Bind(stmt);
    }
};

TEST_F(BinderTest, ResolveUnqualifiedColumns) {
    auto bound = BindSql("SELECT id, name FROM users;");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundSelectStmt>(bound->node));
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 2u);

    EXPECT_EQ(select.projection[0].name, "id");
    ASSERT_TRUE(
        std::holds_alternative<parser::ast::BoundColumnRef>(select.projection[0].expr->node));
    const auto& col0 = std::get<parser::ast::BoundColumnRef>(select.projection[0].expr->node);
    EXPECT_EQ(col0.columnIndex, 0u);
    EXPECT_EQ(col0.type, ColumnType::INT32);

    EXPECT_EQ(select.projection[1].name, "name");
    ASSERT_TRUE(
        std::holds_alternative<parser::ast::BoundColumnRef>(select.projection[1].expr->node));
    const auto& col1 = std::get<parser::ast::BoundColumnRef>(select.projection[1].expr->node);
    EXPECT_EQ(col1.columnIndex, 1u);
    EXPECT_EQ(col1.type, ColumnType::VARCHAR);
}

TEST_F(BinderTest, ResolveQualifiedColumnsWithAlias) {
    auto bound = BindSql("SELECT u.id, u.name FROM users AS u;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 2u);

    const auto& col0 = std::get<parser::ast::BoundColumnRef>(select.projection[0].expr->node);
    EXPECT_EQ(col0.columnIndex, 0u);
    EXPECT_EQ(col0.type, ColumnType::INT32);
    ASSERT_TRUE(col0.tableAlias.has_value());
    EXPECT_EQ(*col0.tableAlias, "u");

    const auto& col1 = std::get<parser::ast::BoundColumnRef>(select.projection[1].expr->node);
    EXPECT_EQ(col1.columnIndex, 1u);
    EXPECT_EQ(col1.type, ColumnType::VARCHAR);
    ASSERT_TRUE(col1.tableAlias.has_value());
    EXPECT_EQ(*col1.tableAlias, "u");
}

TEST_F(BinderTest, ResolveQualifiedColumnsWithTableName) {
    auto bound = BindSql("SELECT users.id, users.age FROM users;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 2u);

    const auto& col0 = std::get<parser::ast::BoundColumnRef>(select.projection[0].expr->node);
    EXPECT_EQ(col0.columnIndex, 0u);
    ASSERT_TRUE(col0.tableAlias.has_value());
    EXPECT_EQ(*col0.tableAlias, "users");

    const auto& col1 = std::get<parser::ast::BoundColumnRef>(select.projection[1].expr->node);
    EXPECT_EQ(col1.columnIndex, 2u);
    ASSERT_TRUE(col1.tableAlias.has_value());
    EXPECT_EQ(*col1.tableAlias, "users");
}

TEST_F(BinderTest, WildcardExpansionSingleTable) {
    auto bound = BindSql("SELECT * FROM users;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 3u);

    EXPECT_EQ(select.projection[0].name, "id");
    EXPECT_EQ(select.projection[1].name, "name");
    EXPECT_EQ(select.projection[2].name, "age");
}

TEST_F(BinderTest, WildcardExpansionJoinTables) {
    auto bound = BindSql("SELECT * FROM users JOIN orders ON users.id = orders.user_id;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 6u);

    EXPECT_EQ(select.projection[0].name, "id");
    EXPECT_EQ(select.projection[1].name, "name");
    EXPECT_EQ(select.projection[2].name, "age");
    EXPECT_EQ(select.projection[3].name, "order_id");
    EXPECT_EQ(select.projection[4].name, "user_id");
    EXPECT_EQ(select.projection[5].name, "amount");
}

TEST_F(BinderTest, AmbiguousColumnThrowsError) {
    EXPECT_THROW(
        { BindSql("SELECT id FROM users JOIN accounts ON users.id = accounts.id;"); },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, DisambiguatedColumnSucceeds) {
    EXPECT_NO_THROW({
        auto bound = BindSql(
            "SELECT users.id, accounts.id FROM users JOIN accounts ON users.id = accounts.id;");
        const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
        EXPECT_EQ(select.projection.size(), 2u);
    });
}

TEST_F(BinderTest, UnknownTableThrowsError) {
    EXPECT_THROW({ BindSql("SELECT id FROM nonexistent;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UnknownColumnThrowsError) {
    EXPECT_THROW({ BindSql("SELECT unknown_col FROM users;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UnknownQualifiedColumnThrowsError) {
    EXPECT_THROW({ BindSql("SELECT users.unknown_col FROM users;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UsingClauseSuccess) {
    auto bound = BindSql("SELECT * FROM users JOIN accounts USING (id);");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.joins.size(), 1u);
    ASSERT_EQ(select.joins[0].usingColumns.size(), 1u);
    EXPECT_EQ(select.joins[0].usingColumns[0], "id");
}

TEST_F(BinderTest, UsingClauseColumnNotFoundInRightTable) {
    EXPECT_THROW(
        { BindSql("SELECT * FROM users JOIN orders USING (name);"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UsingClauseColumnNotFoundInLeftSide) {
    EXPECT_THROW(
        { BindSql("SELECT * FROM users JOIN orders USING (order_id);"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UsingClauseAmbiguousOnLeftSide) {
    EXPECT_THROW(
        {
            BindSql(
                "SELECT * FROM users JOIN accounts ON users.id = accounts.id JOIN accounts AS a2 "
                "USING (id);");
        },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, NaturalJoinSynthesizesEquality) {
    auto bound = BindSql("SELECT * FROM users NATURAL JOIN accounts;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.joins.size(), 1u);
    EXPECT_TRUE(select.joins[0].natural);
    ASSERT_NE(select.joins[0].onCondition, nullptr);
    ASSERT_EQ(select.joins[0].usingColumns.size(), 1u);
    EXPECT_EQ(select.joins[0].usingColumns[0], "id");
}

TEST_F(BinderTest, NaturalJoinRejectsOnCondition) {
    EXPECT_THROW(
        { BindSql("SELECT * FROM users NATURAL JOIN accounts ON users.id = accounts.id;"); },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, AggregateAllowedInProjection) {
    EXPECT_NO_THROW({
        auto bound = BindSql("SELECT COUNT(id) FROM users;");
        const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
        ASSERT_EQ(select.projection.size(), 1u);
        ASSERT_TRUE(std::holds_alternative<parser::ast::BoundFunctionCall>(
            select.projection[0].expr->node));
        const auto& fn = std::get<parser::ast::BoundFunctionCall>(select.projection[0].expr->node);
        EXPECT_TRUE(fn.isAggregate);
    });
}

TEST_F(BinderTest, AggregateRejectedInWhereClause) {
    EXPECT_THROW({ BindSql("SELECT id FROM users WHERE COUNT(id) > 0;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, AggregateRejectedInJoinOnCondition) {
    EXPECT_THROW(
        { BindSql("SELECT users.id FROM users JOIN accounts ON COUNT(users.id) = accounts.id;"); },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, InsertWithSpecifiedColumns) {
    auto bound = BindSql("INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundInsertStmt>(bound->node));
    const auto& ins = std::get<parser::ast::BoundInsertStmt>(bound->node);
    EXPECT_EQ(ins.tableName, "users");
    EXPECT_EQ(ins.columnIndices.size(), 3u);
    EXPECT_EQ(ins.valueRows.size(), 1u);
    EXPECT_EQ(ins.valueRows[0].size(), 3u);
}

TEST_F(BinderTest, InsertWithoutColumnsExpandsAll) {
    auto bound = BindSql("INSERT INTO users VALUES (1, 'Bob', 25);");
    const auto& ins = std::get<parser::ast::BoundInsertStmt>(bound->node);
    EXPECT_EQ(ins.columnIndices.size(), 3u);
    EXPECT_EQ(ins.targetColumns.size(), 3u);
}

TEST_F(BinderTest, InsertUnknownColumnThrows) {
    EXPECT_THROW(
        { BindSql("INSERT INTO users (id, missing_col) VALUES (1, 'fail');"); },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, InsertArityMismatchThrows) {
    EXPECT_THROW(
        { BindSql("INSERT INTO users (id, name, age) VALUES (1, 'Alice');"); },
        utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, UpdateStatementBinding) {
    auto bound = BindSql("UPDATE users SET name = 'Charlie', age = 35 WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundUpdateStmt>(bound->node));
    const auto& upd = std::get<parser::ast::BoundUpdateStmt>(bound->node);
    EXPECT_EQ(upd.tableName, "users");
    ASSERT_EQ(upd.assignments.size(), 2u);
    EXPECT_EQ(upd.assignments[0].first, 1u);
    EXPECT_EQ(upd.assignments[1].first, 2u);
    EXPECT_NE(upd.where, nullptr);
}

TEST_F(BinderTest, UpdateUnknownColumnThrows) {
    EXPECT_THROW(
        { BindSql("UPDATE users SET bad_col = 100 WHERE id = 1;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, DeleteStatementBinding) {
    auto bound = BindSql("DELETE FROM users WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundDeleteStmt>(bound->node));
    const auto& del = std::get<parser::ast::BoundDeleteStmt>(bound->node);
    EXPECT_EQ(del.tableName, "users");
    EXPECT_NE(del.where, nullptr);
}

TEST_F(BinderTest, DeleteUnknownTableThrows) {
    EXPECT_THROW({ BindSql("DELETE FROM missing_table WHERE id = 1;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, CreateTableBinding) {
    auto bound = BindSql(
        "CREATE TABLE items (id INT32 NOT NULL, title VARCHAR, score FLOAT32, emb VECTOR(64));");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundCreateTableStmt>(bound->node));
    const auto& create = std::get<parser::ast::BoundCreateTableStmt>(bound->node);
    EXPECT_EQ(create.tableName, "items");
    ASSERT_EQ(create.schema.columns.size(), 4u);
    EXPECT_EQ(create.schema.columns[0].type, ColumnType::INT32);
    EXPECT_FALSE(create.schema.columns[0].nullable);
    EXPECT_EQ(create.schema.columns[1].type, ColumnType::VARCHAR);
    EXPECT_EQ(create.schema.columns[2].type, ColumnType::FLOAT32);
    EXPECT_EQ(create.schema.columns[3].type, ColumnType::VECTOR);
    EXPECT_EQ(create.schema.columns[3].dimension, 64u);
}

TEST_F(BinderTest, CreateDuplicateTableThrows) {
    EXPECT_THROW({ BindSql("CREATE TABLE users (id INT32);"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, DropTableBinding) {
    auto bound = BindSql("DROP TABLE users;");
    ASSERT_TRUE(std::holds_alternative<parser::ast::BoundDropTableStmt>(bound->node));
    const auto& drop = std::get<parser::ast::BoundDropTableStmt>(bound->node);
    EXPECT_EQ(drop.tableName, "users");
    EXPECT_NE(drop.tableId, 0u);
}

TEST_F(BinderTest, DropUnknownTableThrows) {
    EXPECT_THROW({ BindSql("DROP TABLE missing_table;"); }, utils::SyntaxException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, BinaryOperatorTypeInference) {
    auto bound = BindSql("SELECT id + age, id = age FROM users;");
    const auto& select = std::get<parser::ast::BoundSelectStmt>(bound->node);
    ASSERT_EQ(select.projection.size(), 2u);
    EXPECT_EQ(select.projection[0].expr->type, ColumnType::INT32);
    EXPECT_EQ(select.projection[1].expr->type, ColumnType::BOOLEAN);
}

// ============================================================================
// INSERT value checks: NOT NULL, literal coercion, type families, VECTOR.
//
// `users` comes from the fixture. Its ColumnDefs use the two-argument
// constructor, which makes every column NOT NULL (nullable = false).
// ============================================================================
namespace {

const parser::ast::BoundInsertStmt& AsInsert(const parser::ast::BoundStatement& s) {
    return std::get<parser::ast::BoundInsertStmt>(s.node);
}

const parser::ast::BoundLiteral& LiteralAt(const parser::ast::BoundInsertStmt& ins, size_t row,
                                           size_t k) {
    return std::get<parser::ast::BoundLiteral>(ins.valueRows[row][k]->node);
}

}  // namespace

TEST_F(BinderTest, InsertOmittingNotNullColumnThrowsNamingTheColumn) {
    try {
        (void)BindSql("INSERT INTO users (id, name) VALUES (1, 'a');");
        FAIL() << "expected SemanticException";
    } catch (const utils::SemanticException& e) {
        EXPECT_NE(std::string(e.what()).find("'age'"), std::string::npos) << e.what();
        EXPECT_EQ(e.GetToken(), "age");
    }
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, InsertOmittingNullableColumnIsAllowed) {
    cat->CreateTable("notes", Schema{{ColumnDef{"id", ColumnType::INT32, false},
                                      ColumnDef{"body", ColumnType::VARCHAR, true}}});
    auto bound = BindSql("INSERT INTO notes (id) VALUES (1);");
    const auto& ins = AsInsert(*bound);
    ASSERT_EQ(ins.columnIndices.size(), 1u);
    EXPECT_EQ(ins.columnIndices[0], 0u);
}

TEST_F(BinderTest, InsertNullLiteralIntoNotNullColumnThrowsAtTheNull) {
    try {
        (void)BindSql("INSERT INTO users VALUES (NULL, 'a', 1);");
        FAIL() << "expected SemanticException";
    } catch (const utils::SemanticException& e) {
        EXPECT_EQ(e.GetToken(), "NULL");
        EXPECT_EQ(e.GetLine(), 1u);
    }
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, InsertNullLiteralIntoNullableColumnTakesTheColumnType) {
    cat->CreateTable("notes", Schema{{ColumnDef{"id", ColumnType::INT32, false},
                                      ColumnDef{"body", ColumnType::VARCHAR, true}}});
    auto bound = BindSql("INSERT INTO notes VALUES (1, NULL);");
    const auto& ins = AsInsert(*bound);
    const auto& lit = LiteralAt(ins, 0, 1);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(lit.value));
    EXPECT_EQ(lit.type, ColumnType::VARCHAR);
    EXPECT_EQ(ins.valueRows[0][1]->type, ColumnType::VARCHAR);
}

// The binder types every integer literal INT64. Without retyping, an INT32
// column would receive Value::Int64 and Tuple::Serialize would throw.
TEST_F(BinderTest, InsertIntegerLiteralIntoInt32ColumnIsRetypedToInt32) {
    auto bound = BindSql("INSERT INTO users VALUES (7, 'a', 30);");
    const auto& ins = AsInsert(*bound);
    EXPECT_EQ(LiteralAt(ins, 0, 0).type, ColumnType::INT32);
    EXPECT_EQ(ins.valueRows[0][0]->type, ColumnType::INT32);
    EXPECT_EQ(LiteralAt(ins, 0, 2).type, ColumnType::INT32);
    EXPECT_EQ(std::get<int64_t>(LiteralAt(ins, 0, 0).value), 7);
}

TEST_F(BinderTest, InsertIntegerLiteralIntoInt64ColumnStaysInt64) {
    cat->CreateTable("big", Schema{{ColumnDef{"v", ColumnType::INT64}}});
    auto bound = BindSql("INSERT INTO big VALUES (5000000000);");
    const auto& lit = LiteralAt(AsInsert(*bound), 0, 0);
    EXPECT_EQ(lit.type, ColumnType::INT64);
    EXPECT_EQ(std::get<int64_t>(lit.value), 5000000000LL);
}

TEST_F(BinderTest, InsertIntegerOutOfInt32RangeThrows) {
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (3000000000, 'a', 1);"),
                 utils::SemanticException);
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, InsertNegativeLiteralIsFoldedAndRetyped) {
    auto bound = BindSql("INSERT INTO users VALUES (-5, 'a', 1);");
    const auto& lit = LiteralAt(AsInsert(*bound), 0, 0);  // throws if still a NEGATE node
    EXPECT_EQ(lit.type, ColumnType::INT32);
    EXPECT_EQ(std::get<int64_t>(lit.value), -5);
}

TEST_F(BinderTest, InsertNegativeOutOfInt32RangeThrows) {
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (-3000000000, 'a', 1);"),
                 utils::SemanticException);
}

TEST_F(BinderTest, InsertIntegerIntoFloat64ColumnBecomesDouble) {
    auto bound = BindSql("INSERT INTO accounts VALUES (1, 100);");
    const auto& lit = LiteralAt(AsInsert(*bound), 0, 1);
    EXPECT_EQ(lit.type, ColumnType::FLOAT64);
    ASSERT_TRUE(std::holds_alternative<double>(lit.value));
    EXPECT_DOUBLE_EQ(std::get<double>(lit.value), 100.0);
}

TEST_F(BinderTest, InsertDecimalIntoFloat32ColumnIsRetypedToFloat32) {
    cat->CreateTable("f", Schema{{ColumnDef{"x", ColumnType::FLOAT32}}});
    auto bound = BindSql("INSERT INTO f VALUES (1.5);");
    EXPECT_EQ(LiteralAt(AsInsert(*bound), 0, 0).type, ColumnType::FLOAT32);
}

TEST_F(BinderTest, InsertDecimalIntoIntColumnThrows) {
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (1.5, 'a', 1);"),
                 utils::SemanticException);
}

TEST_F(BinderTest, InsertStringIntoIntColumnThrowsNamingBothTypes) {
    try {
        (void)BindSql("INSERT INTO users VALUES ('x', 'a', 1);");
        FAIL() << "expected SemanticException";
    } catch (const utils::SemanticException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("INT32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("string"), std::string::npos) << msg;
    }
}

TEST_F(BinderTest, InsertIntegerIntoVarcharColumnThrows) {
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (1, 2, 3);"), utils::SemanticException);
}

TEST_F(BinderTest, InsertNumericExpressionIsAcceptedUnfolded) {
    auto bound = BindSql("INSERT INTO users VALUES (1 + 2, 'a', 1);");
    const auto& ins = AsInsert(*bound);
    // Width is left to the executor; the binder only checks the type family.
    EXPECT_TRUE(std::holds_alternative<parser::ast::BoundBinaryOp>(ins.valueRows[0][0]->node));
}

TEST_F(BinderTest, InsertBooleanExpressionIntoIntColumnThrows) {
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (1 = 1, 'a', 1);"),
                 utils::SemanticException);
}

TEST_F(BinderTest, InsertNonNullValueIntoVectorColumnThrows) {
    cat->CreateTable("docs", Schema{{ColumnDef{"id", ColumnType::INT32},
                                     ColumnDef{"emb", ColumnType::VECTOR, true, 4u}}});
    EXPECT_THROW((void)BindSql("INSERT INTO docs VALUES (1, 5);"), utils::SemanticException);
}

TEST_F(BinderTest, InsertNullIntoNullableVectorColumnIsAllowed) {
    cat->CreateTable("docs", Schema{{ColumnDef{"id", ColumnType::INT32},
                                     ColumnDef{"emb", ColumnType::VECTOR, true, 4u}}});
    auto bound = BindSql("INSERT INTO docs VALUES (1, NULL);");
    EXPECT_EQ(LiteralAt(AsInsert(*bound), 0, 1).type, ColumnType::VECTOR);
}

TEST_F(BinderTest, InsertEveryRowIsChecked) {
    // The first row is fine; the second violates NOT NULL.
    EXPECT_THROW((void)BindSql("INSERT INTO users VALUES (1, 'a', 1), (2, NULL, 2);"),
                 utils::SemanticException);
}

// ---- CREATE TABLE checks -------------------------------------------------------

TEST_F(BinderTest, CreateTableDuplicateColumnThrows) {
    try {
        (void)BindSql("CREATE TABLE t (id INT32, id INT64);");
        FAIL() << "expected SemanticException";
    } catch (const utils::SemanticException& e) {
        EXPECT_EQ(e.GetToken(), "id");
    }
    EXPECT_TRUE(sink.hasError());
}

TEST_F(BinderTest, CreateTableVectorWithoutDimensionThrows) {
    EXPECT_THROW((void)BindSql("CREATE TABLE docs (id INT32, emb VECTOR);"),
                 utils::SemanticException);
}

TEST_F(BinderTest, CreateTableVectorWithDimensionBinds) {
    auto bound = BindSql("CREATE TABLE docs (id INT32, emb VECTOR(8));");
    const auto& create = std::get<parser::ast::BoundCreateTableStmt>(bound->node);
    ASSERT_EQ(create.schema.ColumnCount(), 2u);
    EXPECT_EQ(create.schema.columns[1].type, ColumnType::VECTOR);
    EXPECT_EQ(create.schema.columns[1].dimension, 8u);
}
