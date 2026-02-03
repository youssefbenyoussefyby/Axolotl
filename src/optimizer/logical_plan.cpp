#include <optimizer/planner.h>
#include <parser/ast/boundstmt.h>
#include <utils/errors.h>

namespace AxolotlDB::optimizer
{
    namespace
    {
        template <typename... Ts>
        struct overloaded : Ts... { using Ts::operator()...; };
        template <typename... Ts>
        overloaded(Ts...) -> overloaded<Ts...>;

        bool ExprContainsAggregate(const parser::ast::BoundExpr* expr) {
            if (!expr) return false;
            return std::visit(overloaded{
                [](const parser::ast::BoundLiteral&)   { return false; },
                [](const parser::ast::BoundColumnRef&) { return false; },
                [](const parser::ast::BoundFunctionCall& fn) { return fn.isAggregate; },
                [](const parser::ast::BoundBinaryOp& op) {
                    return ExprContainsAggregate(op.left.get()) || ExprContainsAggregate(op.right.get());
                },
                [](const parser::ast::BoundUnaryOp& op) {
                    return ExprContainsAggregate(op.expr.get());
                }
            }, expr->node);
        }
    }

    bool Planner::HasAggregate(const parser::ast::BoundSelectStmt& stmt) {
        for (const auto& item : stmt.projection) {
            if (ExprContainsAggregate(item.expr.get())) return true;
        }
        if (ExprContainsAggregate(stmt.having.get())) return true;
        return !stmt.groupBy.empty();
    }

    LogicalNodePtr Planner::Plan(const parser::ast::BoundStatement& stmt) {
        return std::visit(overloaded{
            [&](const parser::ast::BoundSelectStmt& s)      -> LogicalNodePtr { return PlanSelect(s); },
            [&](const parser::ast::BoundInsertStmt& s)      -> LogicalNodePtr { return PlanInsert(s); },
            [&](const parser::ast::BoundUpdateStmt& s)      -> LogicalNodePtr { return PlanUpdate(s); },
            [&](const parser::ast::BoundDeleteStmt& s)      -> LogicalNodePtr { return PlanDelete(s); },
            [&](const parser::ast::BoundCreateTableStmt& s) -> LogicalNodePtr { return PlanCreateTable(s); },
            [&](const parser::ast::BoundDropTableStmt& s)   -> LogicalNodePtr { return PlanDropTable(s); }
        }, stmt.node);
    }

    LogicalNodePtr Planner::BuildFromAndJoins(const parser::ast::BoundSelectStmt& stmt) {
        if (!stmt.fromTable.has_value()) {
            return nullptr;
        }

        const auto& from = *stmt.fromTable;
        LogicalNodePtr root = std::make_unique<LogicalScan>(from.tableId, from.tableName, from.alias, from.schema);

        for (const auto& join : stmt.joins) {
            auto rightScan = std::make_unique<LogicalScan>(
                join.table.tableId, join.table.tableName, join.table.alias, join.table.schema);

            auto joinNode = std::make_unique<LogicalNestedLoopJoin>(
                join.type,
                parser::ast::CloneExpr(join.onCondition));

            joinNode->children.push_back(std::move(root));
            joinNode->children.push_back(std::move(rightScan));
            root = std::move(joinNode);
        }

        return root;
    }

    LogicalNodePtr Planner::PlanSelect(const parser::ast::BoundSelectStmt& stmt) {
        LogicalNodePtr root = BuildFromAndJoins(stmt);

        if (stmt.where) {
            auto filter = std::make_unique<LogicalFilter>(parser::ast::CloneExpr(stmt.where));
            if (root) filter->children.push_back(std::move(root));
            root = std::move(filter);
        }

        if (HasAggregate(stmt)) {
            auto agg = std::make_unique<LogicalAggregate>();

            for (const auto& g : stmt.groupBy) {
                agg->groupBy.push_back(parser::ast::CloneExpr(g));
            }

            for (const auto& item : stmt.projection) {
                AggregateColumn col;
                col.expr = parser::ast::CloneExpr(item.expr);
                col.name = item.name;
                col.isAggregate = ExprContainsAggregate(item.expr.get());
                agg->aggregates.push_back(std::move(col));
            }

            if (stmt.having) {
                agg->having = parser::ast::CloneExpr(stmt.having);
            }

            if (root) agg->children.push_back(std::move(root));
            root = std::move(agg);
        } else {
            std::vector<ProjectColumn> projections;
            for (const auto& item : stmt.projection) {
                projections.push_back({parser::ast::CloneExpr(item.expr), item.name});
            }
            auto project = std::make_unique<LogicalProject>(std::move(projections));
            if (root) project->children.push_back(std::move(root));
            root = std::move(project);
        }

        if (!stmt.orderBy.empty()) {
            std::vector<SortKey> keys;
            for (const auto& ob : stmt.orderBy) {
                keys.push_back({parser::ast::CloneExpr(ob.expr), ob.isDescending});
            }
            auto sort = std::make_unique<LogicalSort>(std::move(keys));
            if (root) sort->children.push_back(std::move(root));
            root = std::move(sort);
        }

        if (stmt.limit || stmt.offset) {
            auto limit = std::make_unique<LogicalLimit>(
                parser::ast::CloneExpr(stmt.limit),
                parser::ast::CloneExpr(stmt.offset));
            if (root) limit->children.push_back(std::move(root));
            root = std::move(limit);
        }

        return root;
    }

    LogicalNodePtr Planner::PlanInsert(const parser::ast::BoundInsertStmt& stmt) {
        auto node = std::make_unique<LogicalInsert>();
        node->tableId = stmt.tableId;
        node->tableName = stmt.tableName;
        node->columnIndices = stmt.columnIndices;
        node->targetColumns = stmt.targetColumns;

        for (const auto& row : stmt.valueRows) {
            InsertRow ir;
            for (const auto& val : row) {
                ir.values.push_back(parser::ast::CloneExpr(val));
            }
            node->rows.push_back(std::move(ir));
        }

        return node;
    }

    LogicalNodePtr Planner::PlanUpdate(const parser::ast::BoundUpdateStmt& stmt) {
        auto node = std::make_unique<LogicalUpdate>();
        node->tableId = stmt.tableId;
        node->tableName = stmt.tableName;

        for (const auto& [idx, expr] : stmt.assignments) {
            node->assignments.push_back({idx, parser::ast::CloneExpr(expr)});
        }

        if (stmt.where) {
            auto filter = std::make_unique<LogicalFilter>(parser::ast::CloneExpr(stmt.where));
            auto scan = std::make_unique<LogicalScan>(stmt.tableId, stmt.tableName, std::nullopt, storage::Schema{});
            filter->children.push_back(std::move(scan));
            node->children.push_back(std::move(filter));
        } else {
            auto scan = std::make_unique<LogicalScan>(stmt.tableId, stmt.tableName, std::nullopt, storage::Schema{});
            node->children.push_back(std::move(scan));
        }

        return node;
    }

    LogicalNodePtr Planner::PlanDelete(const parser::ast::BoundDeleteStmt& stmt) {
        auto node = std::make_unique<LogicalDelete>();
        node->tableId = stmt.tableId;
        node->tableName = stmt.tableName;

        if (stmt.where) {
            auto filter = std::make_unique<LogicalFilter>(parser::ast::CloneExpr(stmt.where));
            auto scan = std::make_unique<LogicalScan>(stmt.tableId, stmt.tableName, std::nullopt, storage::Schema{});
            filter->children.push_back(std::move(scan));
            node->children.push_back(std::move(filter));
        } else {
            auto scan = std::make_unique<LogicalScan>(stmt.tableId, stmt.tableName, std::nullopt, storage::Schema{});
            node->children.push_back(std::move(scan));
        }

        return node;
    }

    LogicalNodePtr Planner::PlanCreateTable(const parser::ast::BoundCreateTableStmt& stmt) {
        auto node = std::make_unique<LogicalCreateTable>();
        node->tableName = stmt.tableName;
        node->schema = stmt.schema;
        return node;
    }

    LogicalNodePtr Planner::PlanDropTable(const parser::ast::BoundDropTableStmt& stmt) {
        auto node = std::make_unique<LogicalDropTable>();
        node->tableId = stmt.tableId;
        node->tableName = stmt.tableName;
        return node;
    }
}