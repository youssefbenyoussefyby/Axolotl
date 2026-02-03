#pragma once

#include <optimizer/logical_plan.h>
#include <parser/ast/boundstmt.h>

#include <memory>

namespace AxolotlDB::optimizer
{
    class Planner {
    public:
        Planner() = default;

        [[nodiscard]] LogicalNodePtr Plan(const parser::ast::BoundStatement& stmt);

    private:
        [[nodiscard]] LogicalNodePtr PlanSelect(const parser::ast::BoundSelectStmt& stmt);
        [[nodiscard]] LogicalNodePtr PlanInsert(const parser::ast::BoundInsertStmt& stmt);
        [[nodiscard]] LogicalNodePtr PlanUpdate(const parser::ast::BoundUpdateStmt& stmt);
        [[nodiscard]] LogicalNodePtr PlanDelete(const parser::ast::BoundDeleteStmt& stmt);
        [[nodiscard]] LogicalNodePtr PlanCreateTable(const parser::ast::BoundCreateTableStmt& stmt);
        [[nodiscard]] LogicalNodePtr PlanDropTable(const parser::ast::BoundDropTableStmt& stmt);

        [[nodiscard]] LogicalNodePtr BuildFromAndJoins(const parser::ast::BoundSelectStmt& stmt);

        static bool HasAggregate(const parser::ast::BoundSelectStmt& stmt);
    };
}
