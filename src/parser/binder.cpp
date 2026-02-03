#include <parser/binder.h>
#include <utils/errors.h>

namespace AxolotlDB::binder
{
    namespace
    {
        template <typename... Ts>
        struct overloaded : Ts... { using Ts::operator()...; };
        template <typename... Ts>
        overloaded(Ts...) -> overloaded<Ts...>;

        bool HasAggregate(const parser::ast::BoundExpr* expr)
        {
            if (!expr) return false;
            return std::visit(overloaded{
                [](const parser::ast::BoundLiteral&) { return false; },
                [](const parser::ast::BoundColumnRef&) { return false; },
                [](const parser::ast::BoundBinaryOp& b) {
                    return HasAggregate(b.left.get()) || HasAggregate(b.right.get());
                },
                [](const parser::ast::BoundUnaryOp& u) {
                    return HasAggregate(u.expr.get());
                },
                [](const parser::ast::BoundFunctionCall& f) {
                    if (f.isAggregate) return true;
                    for (const auto& arg : f.args) {
                        if (HasAggregate(arg.get())) return true;
                    }
                    return false;
                }
            }, expr->node);
        }

        void CollectUngroupedColumnRefs(const parser::ast::BoundExpr* expr, std::vector<const parser::ast::BoundColumnRef*>& out)
        {
            if (!expr) return;
            std::visit(overloaded{
                [](const parser::ast::BoundLiteral&) {},
                [&](const parser::ast::BoundColumnRef& c) {
                    out.push_back(&c);
                },
                [&](const parser::ast::BoundBinaryOp& b) {
                    CollectUngroupedColumnRefs(b.left.get(), out);
                    CollectUngroupedColumnRefs(b.right.get(), out);
                },
                [&](const parser::ast::BoundUnaryOp& u) {
                    CollectUngroupedColumnRefs(u.expr.get(), out);
                },
                [&](const parser::ast::BoundFunctionCall& f) {
                    if (f.isAggregate) {
                        return;
                    }
                    for (const auto& arg : f.args) {
                        CollectUngroupedColumnRefs(arg.get(), out);
                    }
                }
            }, expr->node);
        }

        bool IsInGroupBy(const parser::ast::BoundColumnRef& col, const std::vector<parser::ast::BoundExprPtr>& groupBy)
        {
            for (const auto& g : groupBy) {
                if (g && std::holds_alternative<parser::ast::BoundColumnRef>(g->node)) {
                    const auto& gcol = std::get<parser::ast::BoundColumnRef>(g->node);
                    if (gcol.tableId == col.tableId && gcol.columnIndex == col.columnIndex) {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    Binder::Binder(catalog::Catalog& catalog, utils::DiagnosticSink* sink): catalog_(catalog), sink_(sink), context_(){}

    void Binder::Error(const std::string& message, uint32_t line, uint32_t column, const std::string& token)
    {
        if (sink_) {
            sink_->addDiagnostic(utils::Diagnostic{message, static_cast<uint16_t>(line), static_cast<uint16_t>(column), token});
        }
        throw utils::SemanticException(message, static_cast<uint16_t>(line), static_cast<uint16_t>(column), token);
    }

    void BinderContext::AddTable(catalog::TableId id, const std::string& name, const std::optional<std::string>& alias, const storage::Schema& schema)
    {
        std::size_t idx = bindings_.size();
        bindings_.emplace_back(id, name, alias, schema);
        qualifiers_[name] = idx;
        if (alias.has_value()) {
            qualifiers_[*alias] = idx;
        }
    }

    const TableBinding* BinderContext::LookupQualifier(const std::string& qualifier) const
    {
        auto it = qualifiers_.find(qualifier);
        if (it == qualifiers_.end()) {
            return nullptr;
        }
        return &bindings_[it->second];
    }

    std::vector<std::pair<const TableBinding*, uint32_t>> BinderContext::FindAllMatches(const std::string& columnName) const
    {
        std::vector<std::pair<const TableBinding*, uint32_t>> matches;
        for (const auto& tbl : bindings_) {
            auto it = tbl.columnIndex.find(columnName);
            if (it != tbl.columnIndex.end()) {
                matches.emplace_back(&tbl, it->second);
            }
        }
        return matches;
    }

    std::vector<std::pair<const TableBinding*, uint32_t>> BinderContext::FindAllMatches(const std::optional<std::string>& tableQualifier, const std::string& columnName) const
    {
        if (!tableQualifier.has_value()) {
            return FindAllMatches(columnName);
        }
        const TableBinding* tbl = LookupQualifier(*tableQualifier);
        if (!tbl) {
            return {};
        }
        auto it = tbl->columnIndex.find(columnName);
        if (it == tbl->columnIndex.end()) {
            return {};
        }
        return { {tbl, it->second} };
    }

    std::optional<std::pair<const TableBinding*, uint32_t>> BinderContext::ResolveColumn(const std::optional<std::string>& tableQualifier, const std::string& columnName) const
    {
        auto matches = FindAllMatches(tableQualifier, columnName);
        if (matches.size() == 1) {
            return matches[0];
        }
        return std::nullopt;
    }

    bool BinderContext::IsAmbiguous(const std::string& columnName) const
    {
        return FindAllMatches(columnName).size() > 1;
    }

    const std::vector<TableBinding>& BinderContext::GetTables() const
    {
        return bindings_;
    }

    std::unique_ptr<parser::ast::BoundStatement> Binder::Bind(const parser::ast::Statement& stmt)
    {
        context_ = BinderContext();
        uint32_t line = stmt.line;
        uint32_t col = stmt.column;

        return std::visit(overloaded{
            [&](const parser::ast::SelectStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindSelect(s), line, col);
            },
            [&](const parser::ast::InsertStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindInsert(s), line, col);
            },
            [&](const parser::ast::UpdateStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindUpdate(s), line, col);
            },
            [&](const parser::ast::DeleteStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindDelete(s), line, col);
            },
            [&](const parser::ast::CreateTableStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindCreateTable(s), line, col);
            },
            [&](const parser::ast::DropTableStmt& s) -> std::unique_ptr<parser::ast::BoundStatement> {
                return std::make_unique<parser::ast::BoundStatement>(BindDrop(s), line, col);
            }
        }, stmt.node);
    }

    std::pair<catalog::TableId, storage::Schema> Binder::ResolveTable(const parser::ast::TableName& tbl)
    {
        try {
            catalog::TableId id = catalog_.GetTableId(tbl.name);
            const storage::Schema& schema = catalog_.GetSchema(tbl.name);
            return {id, schema};
        } catch (const utils::StorageException&) {
            Error("Table '" + tbl.name + "' does not exist", tbl.line, tbl.column, tbl.name);
            return {0, {}};
        }
    }

    parser::ast::BoundTableRef Binder::BindTableRef(const parser::ast::TableRef& tbl)
    {
        return BindTableRefInContext(tbl, context_);
    }

    parser::ast::BoundTableRef Binder::BindTableRefInContext(const parser::ast::TableRef& tbl, BinderContext& ctx)
    {
        auto [id, schema] = ResolveTable(tbl.table);
        ctx.AddTable(id, tbl.table.name, tbl.alias, schema);
        return parser::ast::BoundTableRef(id, tbl.table.name, tbl.alias, schema);
    }

    parser::ast::BoundJoinClause Binder::BindJoinClause(const parser::ast::JoinClause& join, BinderContext& ctx)
    {
        if (join.natural && (join.onCondition || !join.usingColumns.empty())) {
            Error("NATURAL join cannot have ON or USING clause", join.table.table.line, join.table.table.column, join.table.table.name);
        }

        if (join.onCondition && !join.usingColumns.empty()) {
            Error("JOIN cannot have both ON and USING", join.table.table.line, join.table.table.column, join.table.table.name);
        }

        auto [rightId, rightSchema] = ResolveTable(join.table.table);
        const std::string& rightName = join.table.table.name;
        TableBinding rightBinding(rightId, rightName, join.table.alias, rightSchema);

        for (const auto& colName : join.usingColumns) {
            if (rightBinding.columnIndex.find(colName) == rightBinding.columnIndex.end()) {
                Error("column not found in right table", join.table.table.line, join.table.table.column, colName);
            }
            auto leftMatches = ctx.FindAllMatches(colName);
            if (leftMatches.empty()) {
                Error("column not found in left side", join.table.table.line, join.table.table.column, colName);
            }
            if (leftMatches.size() > 1) {
                Error("column is ambiguous on the left side", join.table.table.line, join.table.table.column, colName);
            }
        }

        parser::ast::BoundExprPtr on;
        std::vector<std::string> naturalCols;

        if (join.natural) {
            for (const auto& rcol : rightSchema.columns) {
                auto leftMatches = ctx.FindAllMatches(rcol.name);
                if (leftMatches.size() > 1) {
                    Error("column is ambiguous on the left side", join.table.table.line, join.table.table.column, rcol.name);
                }
                if (leftMatches.size() == 1) {
                    naturalCols.push_back(rcol.name);
                    const auto* leftTbl = leftMatches[0].first;
                    uint32_t leftIdx = leftMatches[0].second;
                    auto leftQual = leftTbl->alias.has_value() ? leftTbl->alias : std::optional<std::string>(leftTbl->name);
                    auto rightQual = join.table.alias.has_value() ? join.table.alias : std::optional<std::string>(rightName);

                    auto leftExpr = std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundColumnRef(leftTbl->tableId, leftIdx, rcol.name, rcol.type, leftQual),rcol.type, join.table.table.line, join.table.table.column);
                    auto rightExpr = std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundColumnRef(rightId, rightBinding.columnIndex.at(rcol.name), rcol.name, rcol.type, rightQual),rcol.type, join.table.table.line, join.table.table.column);

                    auto eqExpr = std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundBinaryOp(parser::ast::BinaryOperator::EQUAL, std::move(leftExpr), std::move(rightExpr), storage::ColumnType::BOOLEAN),storage::ColumnType::BOOLEAN, join.table.table.line, join.table.table.column);

                    if (!on) {
                        on = std::move(eqExpr);
                    } else {
                        on = std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundBinaryOp(parser::ast::BinaryOperator::AND, std::move(on), std::move(eqExpr), storage::ColumnType::BOOLEAN),storage::ColumnType::BOOLEAN, join.table.table.line, join.table.table.column);
                    }
                }
            }
        }

        ctx.AddTable(rightId, rightName, join.table.alias, rightSchema);
        parser::ast::BoundTableRef tbl(rightId, rightName, join.table.alias, rightSchema);

        if (join.onCondition) {
            on = BindExprInContext(join.onCondition, ctx, false);
        }

        const auto& finalUsing = join.natural ? naturalCols : join.usingColumns;
        return parser::ast::BoundJoinClause(std::move(tbl), std::move(on), join.type, join.natural, finalUsing);
    }

    parser::ast::BoundSelectItem Binder::BindSelectItem(const parser::ast::SelectItem& item, BinderContext& ctx)
    {
        parser::ast::BoundExprPtr expr = BindExprInContext(item.expr, ctx, true);
        std::string name;
        
        if (item.alias.has_value()) {
            name = *item.alias;
        } else if (std::holds_alternative<parser::ast::ColumnRef>(item.expr->node)) {
            name = std::get<parser::ast::ColumnRef>(item.expr->node).column;
        } else if (std::holds_alternative<parser::ast::FunctionCall>(item.expr->node)) {
            name = std::get<parser::ast::FunctionCall>(item.expr->node).name;
        } else {
            name = "?";
        }
        
        return parser::ast::BoundSelectItem(std::move(expr), name, item.alias);
    }

    std::vector<parser::ast::BoundSelectItem> Binder::BindAllColumns(BinderContext& ctx)
    {
        std::vector<parser::ast::BoundSelectItem> items;
        for (const auto& tbl : ctx.GetTables()) {
            std::optional<std::string> qual = tbl.alias.has_value() ? tbl.alias : std::optional<std::string>(tbl.name);
            for (uint32_t i = 0; i < static_cast<uint32_t>(tbl.schema.columns.size()); ++i) {
                const auto& col = tbl.schema.columns[i];
                auto colRef = std::make_unique<parser::ast::BoundExpr>(
                    parser::ast::BoundColumnRef(tbl.tableId, i, col.name, col.type, qual),
                    col.type, 0, 0);
                items.emplace_back(std::move(colRef), col.name, std::nullopt);
            }
        }
        return items;
    }

    std::vector<parser::ast::BoundSelectItem> Binder::BindTableColumns(const std::string& qualifier, BinderContext& ctx)
    {
        const auto* tbl = ctx.LookupQualifier(qualifier);
        if (!tbl) {
            Error("Unknown table qualifier: " + qualifier, 0, 0, qualifier);
        }
        std::vector<parser::ast::BoundSelectItem> items;
        std::optional<std::string> qual = tbl->alias.has_value() ? tbl->alias : std::optional<std::string>(tbl->name);
        for (uint32_t i = 0; i < static_cast<uint32_t>(tbl->schema.columns.size()); ++i) {
            const auto& col = tbl->schema.columns[i];
            auto colRef = std::make_unique<parser::ast::BoundExpr>(
                parser::ast::BoundColumnRef(tbl->tableId, i, col.name, col.type, qual),
                col.type, 0, 0);
            items.emplace_back(std::move(colRef), col.name, std::nullopt);
        }
        return items;
    }

    parser::ast::BoundOrderByItem Binder::BindOrderByItem(const parser::ast::OrderByItem& item, BinderContext& ctx)
    {
        parser::ast::BoundExprPtr expr = BindExprInContext(item.expr, ctx, false);
        return parser::ast::BoundOrderByItem(std::move(expr), item.isDescending);
    }

    parser::ast::BoundSelectStatement Binder::BindSelect(const parser::ast::SelectStatement& stmt)
    {
        BinderContext ctx;

        parser::ast::BoundSelectStmt bound;
        bound.isDistinct = stmt.isDistinct;

        if (stmt.fromTable.has_value()) {
            bound.fromTable = BindTableRefInContext(*stmt.fromTable, ctx);
        }

        for (const auto& join : stmt.joins) {
            bound.joins.push_back(BindJoinClause(join, ctx));
        }

        if (stmt.where) {
            bound.where = BindExprInContext(stmt.where, ctx, false);
        }

        for (const auto& g : stmt.groupBy) {
            bound.groupBy.push_back(BindExprInContext(g, ctx, false));
        }

        if (stmt.having) {
            bound.having = BindExprInContext(stmt.having, ctx, true);
        }

        for (const auto& item : stmt.projection) {
            if (item.expr && std::holds_alternative<parser::ast::Star>(item.expr->node)) {
                const auto& star = std::get<parser::ast::Star>(item.expr->node);
                if (star.qualifier.has_value()) {
                    auto qualCols = BindTableColumns(*star.qualifier, ctx);
                    for (auto& col : qualCols) {
                        bound.projection.push_back(std::move(col));
                    }
                } else {
                    auto allCols = BindAllColumns(ctx);
                    for (auto& col : allCols) {
                        bound.projection.push_back(std::move(col));
                    }
                }
                continue;
            }
            if (item.alias.has_value() && *item.alias == "*") {
                auto allCols = BindAllColumns(ctx);
                for (auto& col : allCols) {
                    bound.projection.push_back(std::move(col));
                }
                continue;
            }
            bound.projection.push_back(BindSelectItem(item, ctx));
        }

        bool hasGroupBy = !bound.groupBy.empty();
        bool hasAggInProj = false;
        for (const auto& p : bound.projection) {
            if (HasAggregate(p.expr.get())) {
                hasAggInProj = true;
                break;
            }
        }
        if (hasGroupBy || hasAggInProj || bound.having != nullptr) {
            for (const auto& p : bound.projection) {
                std::vector<const parser::ast::BoundColumnRef*> cols;
                CollectUngroupedColumnRefs(p.expr.get(), cols);
                for (const auto* col : cols) {
                    if (!IsInGroupBy(*col, bound.groupBy)) {
                        Error("Column '" + col->columnName + "' must appear in the GROUP BY clause or be used in an aggregate function",
                              p.expr ? p.expr->line : 0, p.expr ? p.expr->column : 0, col->columnName);
                    }
                }
            }
        }

        for (const auto& ob : stmt.orderBy) {
            bound.orderBy.push_back(BindOrderByItem(ob, ctx));
        }

        if (stmt.limit) {
            bound.limit = BindExprInContext(stmt.limit, ctx, false);
        }

        if (stmt.offset) {
            bound.offset = BindExprInContext(stmt.offset, ctx, false);
        }

        return bound;
    }

    parser::ast::BoundInsertStatement Binder::BindInsert(const parser::ast::InsertStatement& stmt)
    {
        auto [id, schema] = ResolveTable(stmt.table);
        const std::string& name = stmt.table.name;

        parser::ast::BoundInsertStmt bound;
        bound.tableId = id;
        bound.tableName = name;

        if (stmt.columns.has_value()) {
            for (const auto& colName : *stmt.columns) {
                auto it = std::find_if(schema.columns.begin(), schema.columns.end(),
                    [&](const storage::ColumnDef& c){ return c.name == colName; });
                if (it == schema.columns.end()) {
                    Error("Unknown column '" + colName + "' in table '" + name + "'",
                          stmt.table.line, stmt.table.column, colName);
                }
                bound.columnIndices.push_back(static_cast<uint32_t>(std::distance(schema.columns.begin(), it)));
                bound.targetColumns.push_back(*it);
            }
        } else {
            for (uint32_t i = 0; i < static_cast<uint32_t>(schema.columns.size()); ++i) {
                bound.columnIndices.push_back(i);
                bound.targetColumns.push_back(schema.columns[i]);
            }
        }

        BinderContext ctx;
        ctx.AddTable(id, name, std::nullopt, schema);

        for (const auto& row : stmt.valueRows) {
            if (row.size() != bound.targetColumns.size()) {
                Error("INSERT arity mismatch: expected " + std::to_string(bound.targetColumns.size()) +
                      " values, got " + std::to_string(row.size()),
                      stmt.table.line, stmt.table.column, name);
            }
            std::vector<parser::ast::BoundExprPtr> boundRow;
            for (const auto& val : row) {
                boundRow.push_back(BindExprInContext(val, ctx, false));
            }
            bound.valueRows.push_back(std::move(boundRow));
        }

        return bound;
    }

    parser::ast::BoundUpdateStatement Binder::BindUpdate(const parser::ast::UpdateStatement& stmt)
    {
        auto [id, schema] = ResolveTable(stmt.table);
        const std::string& name = stmt.table.name;

        BinderContext ctx;
        ctx.AddTable(id, name, std::nullopt, schema);
        const TableBinding* tbl = ctx.LookupQualifier(name);

        parser::ast::BoundUpdateStmt bound;
        bound.tableId = id;
        bound.tableName = name;

        for (const auto& [colName, valExpr] : stmt.assignments) {
            auto col_it = tbl->columnIndex.find(colName);
            if (col_it == tbl->columnIndex.end()) {
                Error("Unknown column '" + colName + "' in table '" + name + "'",
                      stmt.table.line, stmt.table.column, colName);
            }
            parser::ast::BoundExprPtr val = BindExprInContext(valExpr, ctx, false);
            bound.assignments.emplace_back(col_it->second, std::move(val));
        }

        if (stmt.where) {
            bound.where = BindExprInContext(stmt.where, ctx, false);
        }

        return bound;
    }

    parser::ast::BoundDeleteStatement Binder::BindDelete(const parser::ast::DeleteStatement& stmt)
    {
        auto [id, schema] = ResolveTable(stmt.table);
        const std::string& name = stmt.table.name;

        BinderContext ctx;
        ctx.AddTable(id, name, std::nullopt, schema);

        parser::ast::BoundDeleteStmt bound;
        bound.tableId = id;
        bound.tableName = name;

        if (stmt.where) {
            bound.where = BindExprInContext(stmt.where, ctx, false);
        }

        return bound;
    }

    parser::ast::BoundCreateTableStatement Binder::BindCreateTable(const parser::ast::CreateTableStatement& stmt)
    {
        try {
            (void)catalog_.GetTableId(stmt.table.name);
            Error("Table '" + stmt.table.name + "' already exists", stmt.table.line, stmt.table.column, stmt.table.name);
        } catch (const utils::StorageException&) {
        }

        static const std::unordered_map<std::string, storage::ColumnType> kTypeMap = {
            {"INT32",   storage::ColumnType::INT32},
            {"INT64",   storage::ColumnType::INT64},
            {"FLOAT32", storage::ColumnType::FLOAT32},
            {"FLOAT64", storage::ColumnType::FLOAT64},
            {"BOOLEAN", storage::ColumnType::BOOLEAN},
            {"VARCHAR", storage::ColumnType::VARCHAR},
            {"VECTOR",  storage::ColumnType::VECTOR},
        };

        parser::ast::BoundCreateTableStmt bound;
        bound.tableName = stmt.table.name;

        for (const auto& col : stmt.columns) {
            auto it = kTypeMap.find(col.typeName);
            if (it == kTypeMap.end()) {
                Error("Unknown type '" + col.typeName + "'", col.line, col.column, col.typeName);
            }
            storage::ColumnDef def;
            def.name = col.name;
            def.type = it->second;
            def.nullable = !col.isNotNull;
            def.dimension = col.typeSize.value_or(0);
            bound.schema.columns.push_back(def);
        }

        return bound;
    }

    parser::ast::BoundDropTableStatement Binder::BindDrop(const parser::ast::DropTableStatement& stmt)
    {
        auto [id, schema] = ResolveTable(stmt.table);
        const std::string& name = stmt.table.name;

        parser::ast::BoundDropTableStmt bound;
        bound.tableId = id;
        bound.tableName = name;
        return bound;
    }

    static const std::unordered_map<std::string, bool> kAggregates = {
        {"COUNT", true}, {"SUM", true}, {"AVG", true}, {"MIN", true}, {"MAX", true}
    };

    std::unique_ptr<parser::ast::BoundExpr> Binder::BindExpression(const parser::ast::ExprPtr& expr, bool allowAggregate)
    {
        return BindExprInContext(expr, context_, allowAggregate);
    }

    std::unique_ptr<parser::ast::BoundExpr> Binder::BindExprInContext(const parser::ast::ExprPtr& expr, BinderContext& ctx, bool allowAggregate)
    {
        if (!expr) return nullptr;
        uint16_t line = expr->line;
        uint16_t col = expr->column;

        return std::visit(overloaded{
            [&](const parser::ast::Literal& lit) -> std::unique_ptr<parser::ast::BoundExpr> {
                storage::ColumnType t = storage::ColumnType::INT32;
                std::visit(overloaded{
                    [&](int64_t)            { t = storage::ColumnType::INT64; },
                    [&](double)             { t = storage::ColumnType::FLOAT64; },
                    [&](const std::string&) { t = storage::ColumnType::VARCHAR; },
                    [&](bool)               { t = storage::ColumnType::BOOLEAN; },
                    [&](std::nullptr_t)     { t = storage::ColumnType::INT32; }
                }, lit.value);
                return std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundLiteral(t, lit.value), t, line, col);
            },
            [&](const parser::ast::ColumnRef& ref) -> std::unique_ptr<parser::ast::BoundExpr> {
                auto matches = ctx.FindAllMatches(ref.table, ref.column);
                if (matches.empty()) {
                    std::string msg = ref.table.has_value() ? "Unknown column '" + *ref.table + "." + ref.column + "'" : "Unknown column '" + ref.column + "'";
                    Error(msg, line, col, ref.column);
                }
                if (matches.size() > 1) {
                    Error("Ambiguous column '" + ref.column + "'", line, col, ref.column);
                }
                const TableBinding* tbl = matches[0].first;
                uint32_t idx = matches[0].second;
                storage::ColumnType t = tbl->schema.columns[idx].type;
                return std::make_unique<parser::ast::BoundExpr>(
                    parser::ast::BoundColumnRef(tbl->tableId, idx, ref.column, t, ref.table), t, line, col);
            },
            [&](const parser::ast::BinaryOp& op) -> std::unique_ptr<parser::ast::BoundExpr> {
                auto left  = BindExprInContext(op.left,  ctx, allowAggregate);
                auto right = BindExprInContext(op.right, ctx, allowAggregate);
                storage::ColumnType resType = storage::ColumnType::BOOLEAN;
                if (op.biop == parser::ast::BinaryOperator::ADD  || op.biop == parser::ast::BinaryOperator::SUBTRACT || op.biop == parser::ast::BinaryOperator::MULTIPLY || op.biop == parser::ast::BinaryOperator::DIVIDE) {
                    resType = left->type;
                }
                return std::make_unique<parser::ast::BoundExpr>( parser::ast::BoundBinaryOp(op.biop, std::move(left), std::move(right), resType), resType, line, col);
            },
            [&](const parser::ast::UnaryOp& op) -> std::unique_ptr<parser::ast::BoundExpr> {
                auto sub = BindExprInContext(op.expr, ctx, allowAggregate);
                storage::ColumnType resType = op.uop == parser::ast::UnaryOperator::NOT ? storage::ColumnType::BOOLEAN : sub->type;
                return std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundUnaryOp(op.uop, std::move(sub), resType), resType, line, col);
            },
            [&](const parser::ast::FunctionCall& fn) -> std::unique_ptr<parser::ast::BoundExpr> {
                bool isAgg = kAggregates.count(fn.name) > 0;
                if (isAgg && !allowAggregate) {
                    Error("Aggregate function '" + fn.name + "' is not allowed here", line, col, fn.name);
                }
                std::vector<parser::ast::BoundExprPtr> boundArgs;
                for (const auto& arg : fn.args) {
                    boundArgs.push_back(BindExprInContext(arg, ctx, false));
                }
                storage::ColumnType resType = storage::ColumnType::INT64;
                return std::make_unique<parser::ast::BoundExpr>(
                    parser::ast::BoundFunctionCall(fn.name, isAgg, std::move(boundArgs), resType), resType, line, col);
            },
            [&](const parser::ast::Star&) -> std::unique_ptr<parser::ast::BoundExpr> {
                return std::make_unique<parser::ast::BoundExpr>(parser::ast::BoundLiteral(storage::ColumnType::INT32, static_cast<int64_t>(0)),storage::ColumnType::INT32, line, col);
            }
        }, expr->node);
    }
}
