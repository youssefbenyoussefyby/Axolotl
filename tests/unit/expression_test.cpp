#include <executor/expression.h>
#include <gtest/gtest.h>
#include <parser/ast/boundstmt.h>
#include <storage/tuple.h>

#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB;
using namespace AxolotlDB::executor;
using namespace AxolotlDB::parser::ast;
using storage::ColumnType;
using storage::Schema;
using storage::Tuple;
using storage::Value;

static BoundExpr MakeInt(int64_t v, ColumnType t = ColumnType::INT32) {
    return BoundExpr(BoundLiteral(t, v), t);
}

static BoundExpr MakeDouble(double v, ColumnType t = ColumnType::FLOAT64) {
    return BoundExpr(BoundLiteral(t, v), t);
}

static BoundExpr MakeStr(const std::string& v) {
    return BoundExpr(BoundLiteral(ColumnType::VARCHAR, v), ColumnType::VARCHAR);
}

static BoundExpr MakeBool(bool v) {
    return BoundExpr(BoundLiteral(ColumnType::BOOLEAN, v), ColumnType::BOOLEAN);
}

static BoundExpr MakeNull(ColumnType t = ColumnType::INT32) {
    return BoundExpr(BoundLiteral(t, nullptr), t);
}

static BoundExpr MakeBinOp(BinaryOperator op, BoundExpr left, BoundExpr right, ColumnType resType) {
    return BoundExpr(BoundBinaryOp(op, std::make_unique<BoundExpr>(std::move(left)),
                                   std::make_unique<BoundExpr>(std::move(right)), resType),
                     resType);
}

static BoundExpr MakeUnaryOp(UnaryOperator op, BoundExpr operand, ColumnType resType) {
    return BoundExpr(BoundUnaryOp(op, std::make_unique<BoundExpr>(std::move(operand)), resType),
                     resType);
}

static BoundExpr MakeColRef(uint32_t colIdx, ColumnType t) {
    return BoundExpr(BoundColumnRef(1, colIdx, "col" + std::to_string(colIdx), t), t);
}

TEST(ExpressionTest, ArithmeticAndComparison) {
    {
        auto add = MakeBinOp(BinaryOperator::ADD, MakeInt(10), MakeInt(25), ColumnType::INT32);
        auto val = EvaluateConstant(add);
        EXPECT_EQ(val.AsInt32(), 35);
    }
    {
        auto sub = MakeBinOp(BinaryOperator::SUBTRACT, MakeInt(50LL, ColumnType::INT64),
                             MakeInt(20LL, ColumnType::INT64), ColumnType::INT64);
        auto val = EvaluateConstant(sub);
        EXPECT_EQ(val.AsInt64(), 30);
    }
    {
        auto mul = MakeBinOp(BinaryOperator::MULTIPLY, MakeInt(6), MakeInt(7), ColumnType::INT32);
        auto val = EvaluateConstant(mul);
        EXPECT_EQ(val.AsInt32(), 42);
    }
    {
        auto div = MakeBinOp(BinaryOperator::DIVIDE, MakeInt(100), MakeInt(4), ColumnType::INT32);
        auto val = EvaluateConstant(div);
        EXPECT_EQ(val.AsInt32(), 25);
    }
    {
        auto divZero =
            MakeBinOp(BinaryOperator::DIVIDE, MakeInt(10), MakeInt(0), ColumnType::INT32);
        EXPECT_THROW((void)EvaluateConstant(divZero), std::runtime_error);
    }
    {
        auto overflow = MakeBinOp(BinaryOperator::ADD, MakeInt(2147483647LL, ColumnType::INT64),
                                  MakeInt(1LL, ColumnType::INT64), ColumnType::INT32);
        EXPECT_THROW((void)EvaluateConstant(overflow), std::runtime_error);
    }
    {
        auto fadd =
            MakeBinOp(BinaryOperator::ADD, MakeDouble(1.5), MakeDouble(2.5), ColumnType::FLOAT64);
        auto val = EvaluateConstant(fadd);
        EXPECT_DOUBLE_EQ(val.AsFloat64(), 4.0);
    }
    {
        auto fdivZero = MakeBinOp(BinaryOperator::DIVIDE, MakeDouble(1.0), MakeDouble(0.0),
                                  ColumnType::FLOAT64);
        EXPECT_THROW((void)EvaluateConstant(fdivZero), std::runtime_error);
    }
    {
        auto eq = MakeBinOp(BinaryOperator::EQUAL, MakeInt(5), MakeInt(5), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(eq).AsBool());

        auto ne = MakeBinOp(BinaryOperator::NOT_EQUAL, MakeInt(5), MakeInt(6), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(ne).AsBool());

        auto lt = MakeBinOp(BinaryOperator::LESS, MakeInt(4), MakeInt(5), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(lt).AsBool());

        auto le =
            MakeBinOp(BinaryOperator::LESS_EQUAL, MakeInt(5), MakeInt(5), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(le).AsBool());

        auto gt = MakeBinOp(BinaryOperator::GREATER, MakeInt(6), MakeInt(5), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(gt).AsBool());

        auto ge =
            MakeBinOp(BinaryOperator::GREATER_EQUAL, MakeInt(5), MakeInt(5), ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(ge).AsBool());
    }
    {
        auto strEq = MakeBinOp(BinaryOperator::EQUAL, MakeStr("axolotl"), MakeStr("axolotl"),
                               ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(strEq).AsBool());

        auto strLt = MakeBinOp(BinaryOperator::LESS, MakeStr("apple"), MakeStr("banana"),
                               ColumnType::BOOLEAN);
        EXPECT_TRUE(EvaluateConstant(strLt).AsBool());
    }
    {
        auto neg = MakeUnaryOp(UnaryOperator::NEGATE, MakeInt(12), ColumnType::INT32);
        EXPECT_EQ(EvaluateConstant(neg).AsInt32(), -12);

        auto notOp = MakeUnaryOp(UnaryOperator::NOT, MakeBool(true), ColumnType::BOOLEAN);
        EXPECT_FALSE(EvaluateConstant(notOp).AsBool());
    }
}

TEST(ExpressionTest, NullPropagatesThroughComparison) {
    {
        auto cmp = MakeBinOp(BinaryOperator::EQUAL, MakeNull(), MakeInt(10), ColumnType::BOOLEAN);
        auto val = EvaluateConstant(cmp);
        EXPECT_TRUE(val.IsNull());
        EXPECT_FALSE(IsTrue(val));
    }
    {
        auto cmp = MakeBinOp(BinaryOperator::EQUAL, MakeNull(), MakeNull(), ColumnType::BOOLEAN);
        auto val = EvaluateConstant(cmp);
        EXPECT_TRUE(val.IsNull());
        EXPECT_FALSE(IsTrue(val));
    }
    {
        auto cmp = MakeBinOp(BinaryOperator::LESS, MakeInt(5), MakeNull(), ColumnType::BOOLEAN);
        auto val = EvaluateConstant(cmp);
        EXPECT_TRUE(val.IsNull());
    }
    {
        auto notNull =
            MakeUnaryOp(UnaryOperator::NOT, MakeNull(ColumnType::BOOLEAN), ColumnType::BOOLEAN);
        auto val = EvaluateConstant(notNull);
        EXPECT_TRUE(val.IsNull());
    }
    {
        auto andFalse = MakeBinOp(BinaryOperator::AND, MakeNull(ColumnType::BOOLEAN),
                                  MakeBool(false), ColumnType::BOOLEAN);
        auto val = EvaluateConstant(andFalse);
        EXPECT_FALSE(val.IsNull());
        EXPECT_FALSE(val.AsBool());
    }
    {
        auto andTrue = MakeBinOp(BinaryOperator::AND, MakeNull(ColumnType::BOOLEAN), MakeBool(true),
                                 ColumnType::BOOLEAN);
        auto val = EvaluateConstant(andTrue);
        EXPECT_TRUE(val.IsNull());
    }
    {
        auto orTrue = MakeBinOp(BinaryOperator::OR, MakeNull(ColumnType::BOOLEAN), MakeBool(true),
                                ColumnType::BOOLEAN);
        auto val = EvaluateConstant(orTrue);
        EXPECT_FALSE(val.IsNull());
        EXPECT_TRUE(val.AsBool());
    }
    {
        auto orFalse = MakeBinOp(BinaryOperator::OR, MakeNull(ColumnType::BOOLEAN), MakeBool(false),
                                 ColumnType::BOOLEAN);
        auto val = EvaluateConstant(orFalse);
        EXPECT_TRUE(val.IsNull());
    }
    {
        auto nullArithmetic =
            MakeBinOp(BinaryOperator::ADD, MakeNull(), MakeInt(5), ColumnType::INT32);
        auto val = EvaluateConstant(nullArithmetic);
        EXPECT_TRUE(val.IsNull());
    }
}

TEST(ExpressionTest, TupleEvaluation) {
    Schema schema{{storage::ColumnDef{"id", ColumnType::INT32},
                   storage::ColumnDef{"name", ColumnType::VARCHAR},
                   storage::ColumnDef{"score", ColumnType::FLOAT64}}};

    std::vector<Value> row{Value::Int32(42), Value::Varchar("bob"), Value::Float64(98.5)};
    auto bytes = Tuple::Serialize(schema, row);
    Tuple tuple(std::move(bytes));

    auto col0 = MakeColRef(0, ColumnType::INT32);
    auto col1 = MakeColRef(1, ColumnType::VARCHAR);
    auto col2 = MakeColRef(2, ColumnType::FLOAT64);

    EXPECT_EQ(Evaluate(col0, tuple, schema).AsInt32(), 42);
    EXPECT_EQ(Evaluate(col1, tuple, schema).AsVarchar(), "bob");
    EXPECT_DOUBLE_EQ(Evaluate(col2, tuple, schema).AsFloat64(), 98.5);

    auto pred = MakeBinOp(BinaryOperator::GREATER, MakeColRef(0, ColumnType::INT32), MakeInt(40),
                          ColumnType::BOOLEAN);
    auto predVal = Evaluate(pred, tuple, schema);
    EXPECT_TRUE(IsTrue(predVal));

    EXPECT_THROW((void)EvaluateConstant(col0), std::runtime_error);
}
