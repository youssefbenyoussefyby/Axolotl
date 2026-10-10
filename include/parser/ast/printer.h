#pragma once

#include <parser/ast/expr.h>
#include <parser/ast/stmt.h>

#include <string>

namespace AxolotlDB::parser::ast {
std::string ToString(BinaryOperator bio);
std::string ToString(UnaryOperator uao);
std::string ToString(JoinType jt);
std::string ToString(const Expr& expr);
std::string ToString(const SelectStmt& stmt);
std::string ToString(const InsertStmt& stmt);
std::string ToString(const UpdateStmt& stmt);
std::string ToString(const DeleteStmt& stmt);
std::string ToString(const CreateTableStmt& stmt);
std::string ToString(const DropTableStmt& stmt);
std::string ToString(const Stmt& stmt);
}  // namespace AxolotlDB::parser::ast
