#pragma once

#include <catalog/catalog.h>
#include <parser/ast.h>
#include <parser/ast/boundstmt.h>
#include <utils/diagnosticsink.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AxolotlDB::binder {
struct TableBinding {
    catalog::TableId tableId = 0;
    std::string name;
    std::optional<std::string> alias;
    storage::Schema schema;
    std::unordered_map<std::string, uint32_t> columnIndex;

    TableBinding() = default;
    TableBinding(catalog::TableId id, std::string n, std::optional<std::string> a,
                 storage::Schema s)
        : tableId(id), name(std::move(n)), alias(std::move(a)), schema(std::move(s)) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(schema.columns.size()); i++) {
            columnIndex[schema.columns[i].name] = i;
        }
    }
};

class BinderContext {
   private:
    std::vector<TableBinding> bindings_;
    std::unordered_map<std::string, std::size_t> qualifiers_;

   public:
    BinderContext() = default;

    void AddTable(catalog::TableId id, const std::string& name,
                  const std::optional<std::string>& alias, const storage::Schema& schema);
    [[nodiscard]] std::vector<std::pair<const TableBinding*, uint32_t>> FindAllMatches(
        const std::string& columnName) const;
    [[nodiscard]] std::vector<std::pair<const TableBinding*, uint32_t>> FindAllMatches(
        const std::optional<std::string>& tableQualifier, const std::string& columnName) const;
    [[nodiscard]] std::optional<std::pair<const TableBinding*, uint32_t>> ResolveColumn(
        const std::optional<std::string>& tableQualifier, const std::string& columnName) const;
    [[nodiscard]] bool IsAmbiguous(const std::string& columnName) const;
    [[nodiscard]] const TableBinding* LookupQualifier(const std::string& qualifier) const;
    [[nodiscard]] const std::vector<TableBinding>& GetTables() const;
};

class Binder {
   private:
    catalog::Catalog& catalog_;
    utils::DiagnosticSink* sink_ = nullptr;
    BinderContext context_;

    void Error(const std::string& message, uint32_t line = 0, uint32_t column = 0,
               const std::string& token = "");

    std::pair<catalog::TableId, storage::Schema> ResolveTable(const parser::ast::TableName& tbl);
    parser::ast::BoundTableRef BindTableRefInContext(const parser::ast::TableRef& tbl,
                                                     BinderContext& ctx);
    parser::ast::BoundJoinClause BindJoinClause(const parser::ast::JoinClause& join,
                                                BinderContext& ctx);
    parser::ast::BoundSelectItem BindSelectItem(const parser::ast::SelectItem& item,
                                                BinderContext& ctx);
    parser::ast::BoundOrderByItem BindOrderByItem(const parser::ast::OrderByItem& item,
                                                  BinderContext& ctx);
    std::vector<parser::ast::BoundSelectItem> BindAllColumns(BinderContext& ctx);
    std::vector<parser::ast::BoundSelectItem> BindTableColumns(const std::string& qualifier,
                                                               BinderContext& ctx);

    std::unique_ptr<parser::ast::BoundExpr> BindExprInContext(const parser::ast::ExprPtr& expr,
                                                              BinderContext& ctx,
                                                              bool allowAggregate);

    /// Make one INSERT value fit its target column, or fail with a positioned error.
    ///  - NULL into a NOT NULL column is rejected.
    ///  - Literals are retyped to the column type (INT64 literal -> INT32 with a range
    ///    check, integer -> FLOAT32/FLOAT64, FLOAT64 -> FLOAT32), so EvaluateConstant
    ///    produces exactly the Value that Tuple::Serialize expects. `-<number>` is folded
    ///    into a literal first.
    ///  - Non-literal expressions only get a type-family check (numeric / VARCHAR /
    ///    BOOLEAN); a numeric width difference is left to the executor.
    ///  - VECTOR columns accept only NULL until the grammar has a vector literal.
    void CoerceInsertValue(parser::ast::BoundExpr& value, const storage::ColumnDef& target);

   public:
    explicit Binder(catalog::Catalog& catalog, utils::DiagnosticSink* sink = nullptr);

    std::unique_ptr<parser::ast::BoundStatement> Bind(const parser::ast::Statement& stmt);

    parser::ast::BoundSelectStatement BindSelect(const parser::ast::SelectStatement& stmt);
    parser::ast::BoundInsertStatement BindInsert(const parser::ast::InsertStatement& stmt);
    parser::ast::BoundUpdateStatement BindUpdate(const parser::ast::UpdateStatement& stmt);
    parser::ast::BoundDeleteStatement BindDelete(const parser::ast::DeleteStatement& stmt);
    parser::ast::BoundCreateTableStatement BindCreateTable(
        const parser::ast::CreateTableStatement& stmt);
    parser::ast::BoundDropTableStatement BindDrop(const parser::ast::DropTableStatement& stmt);

    parser::ast::BoundTableRef BindTableRef(const parser::ast::TableRef& tbl);
    std::unique_ptr<parser::ast::BoundExpr> BindExpression(const parser::ast::ExprPtr& expr,
                                                           bool allowAggregate = true);
};
}  // namespace AxolotlDB::binder

namespace AxolotlDB::parser {
using ast::BoundBinaryOp;
using ast::BoundColumnRef;
using ast::BoundCreateTableStatement;
using ast::BoundDeleteStatement;
using ast::BoundDropTableStatement;
using ast::BoundExpr;
using ast::BoundExprPtr;
using ast::BoundFunctionCall;
using ast::BoundInsertStatement;
using ast::BoundJoinClause;
using ast::BoundLiteral;
using ast::BoundOrderByItem;
using ast::BoundSelectItem;
using ast::BoundSelectStatement;
using ast::BoundStatement;
using ast::BoundStatementPtr;
using ast::BoundTableRef;
using ast::BoundUnaryOp;
using ast::BoundUpdateStatement;
using binder::Binder;
using binder::BinderContext;
using binder::TableBinding;
}  // namespace AxolotlDB::parser
