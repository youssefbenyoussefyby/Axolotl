#pragma once

#include <catalog/catalog.h>
#include <parser/ast/expr.h>
#include <parser/ast/stmt.h>
#include <storage/tuple.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace AxolotlDB::parser::ast {
class BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundLiteral {
    storage::ColumnType type = storage::ColumnType::INT32;
    std::variant<int64_t, double, std::string, bool, std::nullptr_t> value;

    BoundLiteral() = default;
    BoundLiteral(storage::ColumnType t,
                 std::variant<int64_t, double, std::string, bool, std::nullptr_t> v)
        : type(t), value(std::move(v)) {}
};

struct BoundColumnRef {
    catalog::TableId tableId = 0;
    uint32_t columnIndex = 0;
    std::string columnName;
    storage::ColumnType type = storage::ColumnType::INT32;
    std::optional<std::string> tableAlias;

    BoundColumnRef() = default;
    BoundColumnRef(catalog::TableId tid, uint32_t colIdx, std::string colName,
                   storage::ColumnType t, std::optional<std::string> alias = std::nullopt)
        : tableId(tid),
          columnIndex(colIdx),
          columnName(std::move(colName)),
          type(t),
          tableAlias(std::move(alias)) {}
};

struct BoundBinaryOp {
    BinaryOperator biop;
    BoundExprPtr left;
    BoundExprPtr right;
    storage::ColumnType resultType = storage::ColumnType::BOOLEAN;

    BoundBinaryOp(BinaryOperator op, BoundExprPtr l, BoundExprPtr r, storage::ColumnType resType)
        : biop(op), left(std::move(l)), right(std::move(r)), resultType(resType) {}
};

struct BoundUnaryOp {
    UnaryOperator uop;
    BoundExprPtr expr;
    storage::ColumnType resultType = storage::ColumnType::BOOLEAN;

    BoundUnaryOp(UnaryOperator op, BoundExprPtr e, storage::ColumnType resType)
        : uop(op), expr(std::move(e)), resultType(resType) {}
};

struct BoundFunctionCall {
    std::string name;
    bool isAggregate = false;
    std::vector<BoundExprPtr> args;
    storage::ColumnType resultType = storage::ColumnType::INT64;

    BoundFunctionCall(std::string n, bool isAgg, std::vector<BoundExprPtr> a,
                      storage::ColumnType resType)
        : name(std::move(n)), isAggregate(isAgg), args(std::move(a)), resultType(resType) {}
};

class BoundExpr {
   public:
    std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp, BoundUnaryOp, BoundFunctionCall> node;
    storage::ColumnType type = storage::ColumnType::INT32;
    uint16_t line = 0;
    uint16_t column = 0;

    BoundExpr() = default;
    template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, BoundExpr>>>
    BoundExpr(T&& val, storage::ColumnType t, uint16_t l = 0, uint16_t c = 0)
        : node(std::forward<T>(val)), type(t), line(l), column(c) {}
};

struct BoundTableRef {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::optional<std::string> alias;
    storage::Schema schema;

    BoundTableRef() = default;
    BoundTableRef(catalog::TableId tid, std::string name, std::optional<std::string> a,
                  storage::Schema s)
        : tableId(tid), tableName(std::move(name)), alias(std::move(a)), schema(std::move(s)) {}
};

struct BoundJoinClause {
    JoinType type = JoinType::INNER;
    bool natural = false;
    BoundTableRef table;
    BoundExprPtr onCondition;
    std::vector<std::string> usingColumns;
    BoundJoinClause() = default;
    BoundJoinClause(BoundTableRef tbl, BoundExprPtr on = nullptr, JoinType jt = JoinType::INNER,
                    bool nat = false, std::vector<std::string> usingCols = {})
        : type(jt),
          natural(nat),
          table(std::move(tbl)),
          onCondition(std::move(on)),
          usingColumns(std::move(usingCols)) {}
};

struct BoundSelectItem {
    BoundExprPtr expr;
    std::string name;
    std::optional<std::string> alias;
    BoundSelectItem() = default;
    BoundSelectItem(BoundExprPtr e, std::string n, std::optional<std::string> a = std::nullopt)
        : expr(std::move(e)), name(std::move(n)), alias(std::move(a)) {}
};

struct BoundOrderByItem {
    BoundExprPtr expr;
    bool isDescending = false;

    BoundOrderByItem() = default;
    BoundOrderByItem(BoundExprPtr e, bool desc = false) : expr(std::move(e)), isDescending(desc) {}
};

struct BoundSelectStmt {
    std::vector<BoundSelectItem> projection;
    std::optional<BoundTableRef> fromTable;
    std::vector<BoundJoinClause> joins;
    BoundExprPtr where;
    std::vector<BoundExprPtr> groupBy;
    BoundExprPtr having;
    std::vector<BoundOrderByItem> orderBy;
    BoundExprPtr limit;
    BoundExprPtr offset;
    bool isDistinct = false;
};

struct BoundInsertStmt {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<uint32_t> columnIndices;
    std::vector<storage::ColumnDef> targetColumns;
    std::vector<std::vector<BoundExprPtr>> valueRows;
};

struct BoundUpdateStmt {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<std::pair<uint32_t, BoundExprPtr>> assignments;
    BoundExprPtr where;
};

struct BoundDeleteStmt {
    catalog::TableId tableId = 0;
    std::string tableName;
    BoundExprPtr where;
};

struct BoundCreateTableStmt {
    std::string tableName;
    storage::Schema schema;
};

struct BoundDropTableStmt {
    catalog::TableId tableId = 0;
    std::string tableName;
};

class BoundStmt {
   public:
    std::variant<BoundSelectStmt, BoundInsertStmt, BoundUpdateStmt, BoundDeleteStmt,
                 BoundCreateTableStmt, BoundDropTableStmt>
        node;
    uint32_t line = 0;
    uint32_t column = 0;

    BoundStmt() = default;
    template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, BoundStmt>>>
    BoundStmt(T&& val, uint32_t l = 0, uint32_t c = 0)
        : node(std::forward<T>(val)), line(l), column(c) {}
};

using BoundStatement = BoundStmt;
using BoundStatementPtr = std::unique_ptr<BoundStmt>;
using BoundSelectStatement = BoundSelectStmt;
using BoundInsertStatement = BoundInsertStmt;
using BoundUpdateStatement = BoundUpdateStmt;
using BoundDeleteStatement = BoundDeleteStmt;
using BoundCreateTableStatement = BoundCreateTableStmt;
using BoundDropTableStatement = BoundDropTableStmt;

BoundExprPtr CloneExpr(const BoundExprPtr& expr);
BoundExprPtr CloneExpr(const BoundExpr* expr);

}  // namespace AxolotlDB::parser::ast
