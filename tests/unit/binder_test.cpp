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
