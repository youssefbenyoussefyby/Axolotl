#pragma once

#include <cassert>
#include <iostream>
#include <string>
#include <variant>
#include <vector>
#include <memory>
#include <optional>
#include <type_traits>

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

namespace AxolotlDB::parser::ast
{
    class Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    enum class BinaryOperator {
        // Arithmetic
        ADD,            // +
        SUBTRACT,       // -
        MULTIPLY,       // *
        DIVIDE,         // /

        // Comparison
        EQUAL,          // =
        NOT_EQUAL,      // != or <>
        LESS,           // <
        LESS_EQUAL,     // <=
        GREATER,        // >
        GREATER_EQUAL,  // >=

        // Logical
        AND,            // AND
        OR,             // OR

        // Vector distance
        VECTOR_L2,      // <->
        VECTOR_COSINE,  // <=>
    };

    enum class UnaryOperator {
        NOT,            // NOT
        NEGATE,         // unary -
    };

    struct Literal {
        std::variant<int64_t, double, std::string, bool, std::nullptr_t> value;

        Literal() = default;
        template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Literal>>>
        Literal(T&& val) : value(std::forward<T>(val)) {}
    };

    struct ColumnRef {
        std::optional<std::string> table;
        std::string column;

        ColumnRef() = default;
        ColumnRef(std::string col) : table(std::nullopt), column(std::move(col)) {}
        ColumnRef(std::string tbl, std::string col) : table(std::move(tbl)), column(std::move(col)) {}
    };

    using Identifier = ColumnRef;

    struct BinaryOp {
        BinaryOperator biop;
        ExprPtr left;
        ExprPtr right;
        BinaryOp(BinaryOperator op, ExprPtr l, ExprPtr r): biop(op), left(std::move(l)), right(std::move(r)) {}
    };

    struct UnaryOp {
        UnaryOperator uop;
        ExprPtr expr;
        UnaryOp(UnaryOperator op, ExprPtr e): uop(op), expr(std::move(e)) {}
    };

    struct FunctionCall {
        std::string name;
        std::vector<ExprPtr> args;

        FunctionCall(std::string n, std::vector<ExprPtr> a = {}) : name(std::move(n)), args(std::move(a)) {}
    };

    struct Star {
        std::optional<std::string> qualifier;
        Star() = default;
        Star(std::optional<std::string> q) : qualifier(std::move(q)) {}
    };

    class Expr {
    public:
        std::variant<Literal, ColumnRef, BinaryOp, UnaryOp, FunctionCall, Star> node;
        uint16_t line = 0;
        uint16_t column = 0;
        Expr() = default;

        template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Expr>>>
        Expr(T&& val, uint16_t l = 0, uint16_t c = 0) : node(std::forward<T>(val)), line(l), column(c) {}
    };

} // namespace AxolotlDB::parser::ast
