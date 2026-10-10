#include <executor/expression.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace AxolotlDB::executor {

using parser::ast::BinaryOperator;
using parser::ast::BoundBinaryOp;
using parser::ast::BoundColumnRef;
using parser::ast::BoundExpr;
using parser::ast::BoundFunctionCall;
using parser::ast::BoundLiteral;
using parser::ast::BoundUnaryOp;
using parser::ast::UnaryOperator;
using storage::ColumnType;
using storage::Value;

namespace {

struct Num {
    bool isFloat = false;
    int64_t i = 0;
    double d = 0.0;
    double AsDouble() const noexcept {
        return isFloat ? d : static_cast<double>(i);
    }
};

std::optional<Num> AsNum(const Value& v) {
    if (auto* p = std::get_if<int32_t>(&v.Raw()))
        return Num{false, *p, 0.0};
    if (auto* p = std::get_if<int64_t>(&v.Raw()))
        return Num{false, *p, 0.0};
    if (auto* p = std::get_if<float>(&v.Raw()))
        return Num{true, 0, static_cast<double>(*p)};
    if (auto* p = std::get_if<double>(&v.Raw()))
        return Num{true, 0, *p};
    return std::nullopt;
}

Value Narrow(const Num& n, ColumnType t) {
    switch (t) {
        case ColumnType::INT32: {
            if (n.isFloat)
                return Value::Int32(static_cast<int32_t>(n.d));
            if (n.i < std::numeric_limits<int32_t>::min() ||
                n.i > std::numeric_limits<int32_t>::max()) {
                throw std::runtime_error("Evaluate: INT32 overflow");
            }
            return Value::Int32(static_cast<int32_t>(n.i));
        }
        case ColumnType::INT64:
            return Value::Int64(n.isFloat ? static_cast<int64_t>(n.d) : n.i);
        case ColumnType::FLOAT32:
            return Value::Float32(static_cast<float>(n.AsDouble()));
        case ColumnType::FLOAT64:
            return Value::Float64(n.AsDouble());
        default:
            throw std::runtime_error("Evaluate: arithmetic result has non-numeric type");
    }
}

Value Arithmetic(BinaryOperator op, const Value& l, const Value& r, ColumnType t) {
    if (l.IsNull() || r.IsNull())
        return Value::Null(t);

    auto a = AsNum(l);
    auto b = AsNum(r);
    if (!a || !b)
        throw std::runtime_error("Evaluate: arithmetic on a non-numeric value");

    Num out;
    if (a->isFloat || b->isFloat) {
        out.isFloat = true;
        const double x = a->AsDouble(), y = b->AsDouble();
        switch (op) {
            case BinaryOperator::ADD:
                out.d = x + y;
                break;
            case BinaryOperator::SUBTRACT:
                out.d = x - y;
                break;
            case BinaryOperator::MULTIPLY:
                out.d = x * y;
                break;
            case BinaryOperator::DIVIDE:
                if (y == 0.0)
                    throw std::runtime_error("Evaluate: division by zero");
                out.d = x / y;
                break;
            default:
                throw std::logic_error("Evaluate: not an arithmetic operator");
        }
    } else {
        const int64_t x = a->i, y = b->i;
        switch (op) {
            case BinaryOperator::ADD:
                out.i = x + y;
                break;
            case BinaryOperator::SUBTRACT:
                out.i = x - y;
                break;
            case BinaryOperator::MULTIPLY:
                out.i = x * y;
                break;
            case BinaryOperator::DIVIDE:
                if (y == 0)
                    throw std::runtime_error("Evaluate: division by zero");
                out.i = x / y;
                break;
            default:
                throw std::logic_error("Evaluate: not an arithmetic operator");
        }
    }
    return Narrow(out, t);
}

bool ApplyCmp(BinaryOperator op, int c) {
    switch (op) {
        case BinaryOperator::EQUAL:
            return c == 0;
        case BinaryOperator::NOT_EQUAL:
            return c != 0;
        case BinaryOperator::LESS:
            return c < 0;
        case BinaryOperator::LESS_EQUAL:
            return c <= 0;
        case BinaryOperator::GREATER:
            return c > 0;
        case BinaryOperator::GREATER_EQUAL:
            return c >= 0;
        default:
            throw std::logic_error("Evaluate: not a comparison operator");
    }
}

template <typename T>
int Three(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

Value Compare(BinaryOperator op, const Value& l, const Value& r) {
    if (l.IsNull() || r.IsNull())
        return Value::Null(ColumnType::BOOLEAN);

    auto a = AsNum(l);
    auto b = AsNum(r);
    if (a && b) {
        const int c =
            (a->isFloat || b->isFloat) ? Three(a->AsDouble(), b->AsDouble()) : Three(a->i, b->i);
        return Value::Bool(ApplyCmp(op, c));
    }
    if (auto* x = std::get_if<std::string>(&l.Raw())) {
        if (auto* y = std::get_if<std::string>(&r.Raw()))
            return Value::Bool(ApplyCmp(op, Three(*x, *y)));
    }
    if (auto* x = std::get_if<bool>(&l.Raw())) {
        if (auto* y = std::get_if<bool>(&r.Raw()))
            return Value::Bool(ApplyCmp(op, Three(*x, *y)));
    }
    throw std::runtime_error("Evaluate: cannot compare values of different types");
}

std::optional<bool> AsTruth(const Value& v) {
    if (v.IsNull())
        return std::nullopt;
    if (auto* b = std::get_if<bool>(&v.Raw()))
        return *b;
    throw std::runtime_error("Evaluate: expected a boolean value");
}

Value FromTruth(std::optional<bool> t) {
    return t ? Value::Bool(*t) : Value::Null(ColumnType::BOOLEAN);
}

struct Evaluator {
    const storage::Tuple* tuple;
    const storage::Schema* schema;

    Value Eval(const BoundExpr& e) const {
        return std::visit([&](const auto& n) -> Value { return (*this)(n, e.type); }, e.node);
    }

    Value operator()(const BoundLiteral& n, ColumnType) const {
        return std::visit(
            [&](const auto& v) -> Value {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    return Value::Null(n.type);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    if (n.type == ColumnType::INT32) {
                        if (v < std::numeric_limits<int32_t>::min() ||
                            v > std::numeric_limits<int32_t>::max()) {
                            throw std::runtime_error("Evaluate: INT32 overflow");
                        }
                        return Value::Int32(static_cast<int32_t>(v));
                    }
                    return Value::Int64(v);
                } else if constexpr (std::is_same_v<T, double>) {
                    if (n.type == ColumnType::FLOAT32) {
                        return Value::Float32(static_cast<float>(v));
                    }
                    return Value::Float64(v);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return Value::Varchar(v);
                } else if constexpr (std::is_same_v<T, bool>) {
                    return Value::Bool(v);
                } else {
                    throw std::runtime_error("Evaluate: unknown literal variant");
                }
            },
            n.value);
    }

    Value operator()(const BoundColumnRef& n, ColumnType) const {
        if (tuple == nullptr || schema == nullptr) {
            throw std::runtime_error("Evaluate: column reference in a constant expression");
        }
        auto values = storage::Tuple::Deserialize(*schema, tuple->Span());
        if (n.columnIndex >= values.size()) {
            throw std::runtime_error("Evaluate: column index out of range");
        }
        return values[n.columnIndex];
    }

    Value operator()(const BoundUnaryOp& n, ColumnType t) const {
        const Value v = Eval(*n.expr);
        switch (n.uop) {
            case UnaryOperator::NEGATE: {
                if (v.IsNull())
                    return Value::Null(t);
                auto x = AsNum(v);
                if (!x)
                    throw std::runtime_error("Evaluate: unary minus on a non-numeric value");
                Num out = *x;
                if (out.isFloat)
                    out.d = -out.d;
                else
                    out.i = -out.i;
                return Narrow(out, t);
            }
            case UnaryOperator::NOT: {
                auto b = AsTruth(v);
                return FromTruth(b ? std::optional<bool>(!*b) : std::nullopt);
            }
        }
        throw std::logic_error("Evaluate: unknown unary operator");
    }

    Value operator()(const BoundBinaryOp& n, ColumnType t) const {
        if (n.biop == BinaryOperator::AND) {
            auto l = AsTruth(Eval(*n.left));
            if (l && !*l)
                return Value::Bool(false);
            auto r = AsTruth(Eval(*n.right));
            if (r && !*r)
                return Value::Bool(false);
            if (!l || !r)
                return Value::Null(ColumnType::BOOLEAN);
            return Value::Bool(true);
        }
        if (n.biop == BinaryOperator::OR) {
            auto l = AsTruth(Eval(*n.left));
            if (l && *l)
                return Value::Bool(true);
            auto r = AsTruth(Eval(*n.right));
            if (r && *r)
                return Value::Bool(true);
            if (!l || !r)
                return Value::Null(ColumnType::BOOLEAN);
            return Value::Bool(false);
        }

        const Value l = Eval(*n.left);
        const Value r = Eval(*n.right);
        switch (n.biop) {
            case BinaryOperator::ADD:
            case BinaryOperator::SUBTRACT:
            case BinaryOperator::MULTIPLY:
            case BinaryOperator::DIVIDE:
                return Arithmetic(n.biop, l, r, t);
            case BinaryOperator::EQUAL:
            case BinaryOperator::NOT_EQUAL:
            case BinaryOperator::LESS:
            case BinaryOperator::LESS_EQUAL:
            case BinaryOperator::GREATER:
            case BinaryOperator::GREATER_EQUAL:
                return Compare(n.biop, l, r);
            default:
                throw std::logic_error("Evaluate: unknown binary operator");
        }
    }

    Value operator()(const BoundFunctionCall&, ColumnType) const {
        throw std::runtime_error("Evaluate: aggregate call reached the row evaluator");
    }
};

}  // namespace

Value Evaluate(const BoundExpr& expr, const storage::Tuple& tuple, const storage::Schema& schema) {
    return Evaluator{&tuple, &schema}.Eval(expr);
}

Value EvaluateConstant(const BoundExpr& expr) {
    return Evaluator{nullptr, nullptr}.Eval(expr);
}

bool IsTrue(const Value& v) noexcept {
    if (v.IsNull())
        return false;
    if (auto* b = std::get_if<bool>(&v.Raw())) {
        return *b;
    }
    return false;
}

}  // namespace AxolotlDB::executor