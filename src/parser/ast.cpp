#include <parser/ast/boundstmt.h>

#include <stdexcept>

namespace AxolotlDB::parser::ast {
namespace {
template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
}  // namespace

BoundExprPtr CloneExpr(const BoundExpr* expr) {
    if (!expr)
        return nullptr;

    auto node = std::visit(
        overloaded{
            [](const BoundLiteral& lit) -> std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp,
                                                        BoundUnaryOp, BoundFunctionCall> {
                return BoundLiteral(lit.type, lit.value);
            },
            [](const BoundColumnRef& ref)
                -> std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp, BoundUnaryOp,
                                BoundFunctionCall> {
                return BoundColumnRef(ref.tableId, ref.columnIndex, ref.columnName, ref.type,
                                      ref.tableAlias);
            },
            [](const BoundBinaryOp& op) -> std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp,
                                                        BoundUnaryOp, BoundFunctionCall> {
                return BoundBinaryOp(op.biop, CloneExpr(op.left.get()), CloneExpr(op.right.get()),
                                     op.resultType);
            },
            [](const BoundUnaryOp& op) -> std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp,
                                                       BoundUnaryOp, BoundFunctionCall> {
                return BoundUnaryOp(op.uop, CloneExpr(op.expr.get()), op.resultType);
            },
            [](const BoundFunctionCall& fn)
                -> std::variant<BoundLiteral, BoundColumnRef, BoundBinaryOp, BoundUnaryOp,
                                BoundFunctionCall> {
                std::vector<BoundExprPtr> clonedArgs;
                clonedArgs.reserve(fn.args.size());
                for (const auto& arg : fn.args) {
                    clonedArgs.push_back(CloneExpr(arg.get()));
                }
                return BoundFunctionCall(fn.name, fn.isAggregate, std::move(clonedArgs),
                                         fn.resultType);
            }},
        expr->node);

    auto result = std::make_unique<BoundExpr>();
    result->node = std::move(node);
    result->type = expr->type;
    result->line = expr->line;
    result->column = expr->column;
    return result;
}

BoundExprPtr CloneExpr(const BoundExprPtr& expr) {
    return CloneExpr(expr.get());
}
}  // namespace AxolotlDB::parser::ast

namespace AxolotlDB::parser {}