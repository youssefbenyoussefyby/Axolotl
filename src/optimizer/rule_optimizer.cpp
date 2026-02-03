#include <optimizer/physical_plan.h>

namespace AxolotlDB::optimizer {

namespace {

bool IsEquiCondition(const parser::ast::BoundExpr* expr) {
    if (!expr) {
        return false;
    }
    if (const auto* bin = std::get_if<parser::ast::BoundBinaryOp>(&expr->node)) {
        if (bin->biop == parser::ast::BinaryOperator::EQUAL) {
            return true;
        }
        if (bin->biop == parser::ast::BinaryOperator::AND) {
            return IsEquiCondition(bin->left.get()) && IsEquiCondition(bin->right.get());
        }
    }
    return false;
}

} // namespace

JoinAlgorithm SelectJoinAlgorithm(const LogicalNestedLoopJoin& join) {
    if (IsEquiCondition(join.condition.get())) {
        return JoinAlgorithm::Hash;
    }
    return JoinAlgorithm::NestedLoop;
}

} // namespace AxolotlDB::optimizer