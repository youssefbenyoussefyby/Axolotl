#pragma once

#include <parser/ast/expr.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace AxolotlDB::parser::ast {
enum class JoinType { INNER, LEFT, RIGHT, FULL, CROSS };
struct TableName {
    std::optional<std::string> schema;
    std::string name;
    uint32_t line = 0;
    uint32_t column = 0;

    TableName() = default;
    TableName(const char* n) : name(n) {}
    TableName(std::string n, uint32_t l = 0, uint32_t c = 0)
        : name(std::move(n)), line(l), column(c) {}
};
struct TableRef {
    TableName table;
    std::optional<std::string> alias;

    TableRef() = default;
    TableRef(const char* n) : table(n) {}
    TableRef(std::string n, uint32_t l = 0, uint32_t c = 0,
             std::optional<std::string> a = std::nullopt)
        : table(std::move(n), l, c), alias(std::move(a)) {}
    TableRef(std::string n, std::string a, uint32_t l = 0, uint32_t c = 0)
        : table(std::move(n), l, c), alias(std::move(a)) {}
    TableRef(TableName t, std::optional<std::string> a = std::nullopt)
        : table(std::move(t)), alias(std::move(a)) {}
};

struct SelectItem {
    ExprPtr expr;
    std::optional<std::string> alias;
    SelectItem() = default;
    SelectItem(ExprPtr e, std::optional<std::string> a = std::nullopt)
        : expr(std::move(e)), alias(std::move(a)) {}
};

struct JoinClause {
    JoinType type = JoinType::INNER;
    bool natural = false;
    TableRef table;
    ExprPtr onCondition;
    std::vector<std::string> usingColumns;
    JoinClause() = default;
    JoinClause(TableRef tbl, ExprPtr on = nullptr, JoinType jt = JoinType::INNER)
        : type(jt), table(std::move(tbl)), onCondition(std::move(on)) {}
};

struct OrderByItem {
    ExprPtr expr;
    bool isDescending = false;
    OrderByItem() = default;
    OrderByItem(ExprPtr e, bool desc = false) : expr(std::move(e)), isDescending(desc) {}
};

struct SelectStmt {
    std::vector<SelectItem> projection;
    std::optional<TableRef> fromTable;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> groupBy;
    ExprPtr having;
    std::vector<OrderByItem> orderBy;
    ExprPtr limit;
    ExprPtr offset;
    bool isDistinct = false;
};

struct InsertStmt {
    TableName table;
    std::optional<std::vector<std::string>> columns;
    std::vector<std::vector<ExprPtr>> valueRows;
};

struct UpdateStmt {
    TableName table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr where;
};

struct DeleteStmt {
    TableName table;
    ExprPtr where;
};

struct ColumnDef {
    std::string name;
    std::string typeName;
    std::optional<uint32_t> typeSize;
    bool isPrimaryKey = false;
    bool isNotNull = false;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct CreateTableStmt {
    TableName table;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    TableName table;
};

class Stmt {
   public:
    std::variant<SelectStmt, UpdateStmt, InsertStmt, DeleteStmt, CreateTableStmt, DropTableStmt>
        node;
    uint32_t line = 0;
    uint32_t column = 0;

    Stmt() = default;
    template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Stmt>>>
    Stmt(T&& val, uint32_t l = 0, uint32_t c = 0)
        : node(std::forward<T>(val)), line(l), column(c) {}
};

using Statement = Stmt;
using StatementPtr = std::unique_ptr<Stmt>;
using SelectStatement = SelectStmt;
using InsertStatement = InsertStmt;
using UpdateStatement = UpdateStmt;
using DeleteStatement = DeleteStmt;
using CreateTableStatement = CreateTableStmt;
using DropTableStatement = DropTableStmt;

}  // namespace AxolotlDB::parser::ast