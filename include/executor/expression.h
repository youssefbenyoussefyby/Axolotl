#pragma once

#include <parser/ast/boundstmt.h>
#include <storage/tuple.h>

namespace AxolotlDB::executor
{
    /// Evaluate an expression against one row.
    /// NULL is std::monostate. Comparisons and AND/OR use three-valued logic.
    /// @throws std::runtime_error on division by zero, type mismatch,
    ///         integer narrowing overflow, or an aggregate call.
    [[nodiscard]] storage::Value Evaluate(const parser::ast::BoundExpr& expr,const storage::Tuple& tuple,const storage::Schema& schema);


    [[nodiscard]] storage::Value EvaluateConstant(const parser::ast::BoundExpr& expr);

    [[nodiscard]] bool IsTrue(const storage::Value& v) noexcept;
}