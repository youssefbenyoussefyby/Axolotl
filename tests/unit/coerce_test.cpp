// CoerceToColumn: the one place INSERT and UPDATE turn an evaluated Value into
// the exact type a column stores.
#include <executor/coerce.h>
#include <gtest/gtest.h>
#include <storage/tuple.h>
#include <utils/errors.h>

using namespace AxolotlDB;
using executor::CoerceToColumn;
using storage::ColumnDef;
using storage::ColumnType;
using storage::Value;

namespace {
const ColumnDef kInt32{"i32", ColumnType::INT32};
const ColumnDef kInt64{"i64", ColumnType::INT64};
const ColumnDef kFloat32{"f32", ColumnType::FLOAT32};
const ColumnDef kFloat64{"f64", ColumnType::FLOAT64};
const ColumnDef kVarchar{"txt", ColumnType::VARCHAR};
const ColumnDef kBool{"flag", ColumnType::BOOLEAN};
const ColumnDef kNullableInt{"maybe", ColumnType::INT32, true};
}  // namespace

// ---- unchanged / widening ---------------------------------------------------

TEST(CoerceTest, SameTypeIsReturnedUnchanged) {
    EXPECT_EQ(CoerceToColumn(Value::Int32(7), kInt32), Value::Int32(7));
    EXPECT_EQ(CoerceToColumn(Value::Varchar("a"), kVarchar), Value::Varchar("a"));
    EXPECT_EQ(CoerceToColumn(Value::Bool(true), kBool), Value::Bool(true));
}

TEST(CoerceTest, Int32WidensToInt64) {
    EXPECT_EQ(CoerceToColumn(Value::Int32(-3), kInt64), Value::Int64(-3));
}

TEST(CoerceTest, IntegersWidenToFloats) {
    EXPECT_EQ(CoerceToColumn(Value::Int64(5), kFloat64), Value::Float64(5.0));
    EXPECT_EQ(CoerceToColumn(Value::Int32(2), kFloat32), Value::Float32(2.0f));
}

TEST(CoerceTest, Float64NarrowsToFloat32) {
    EXPECT_EQ(CoerceToColumn(Value::Float64(1.5), kFloat32), Value::Float32(1.5f));
}

// ---- narrowing with a range check ------------------------------------------

TEST(CoerceTest, Int64InRangeNarrowsToInt32) {
    EXPECT_EQ(CoerceToColumn(Value::Int64(2147483647), kInt32), Value::Int32(2147483647));
    EXPECT_EQ(CoerceToColumn(Value::Int64(-2147483648LL), kInt32), Value::Int32(INT32_MIN));
}

TEST(CoerceTest, Int64OutOfRangeForInt32Throws) {
    EXPECT_THROW((void)CoerceToColumn(Value::Int64(2147483648LL), kInt32), utils::AxolotException);
    EXPECT_THROW((void)CoerceToColumn(Value::Int64(-2147483649LL), kInt32), utils::AxolotException);
}

// ---- non-numeric into numeric, and the other way ------------------------------

TEST(CoerceTest, StringIntoNumericColumnThrowsTypeMismatch) {
    EXPECT_THROW((void)CoerceToColumn(Value::Varchar("12"), kInt32), utils::TypeMismatchException);
    EXPECT_THROW((void)CoerceToColumn(Value::Varchar("1.5"), kFloat64),
                 utils::TypeMismatchException);
}

TEST(CoerceTest, NumberIntoVarcharColumnThrowsTypeMismatch) {
    EXPECT_THROW((void)CoerceToColumn(Value::Int64(12), kVarchar), utils::TypeMismatchException);
    EXPECT_THROW((void)CoerceToColumn(Value::Float64(1.5), kVarchar), utils::TypeMismatchException);
}

TEST(CoerceTest, BooleanAndNumbersDoNotMix) {
    EXPECT_THROW((void)CoerceToColumn(Value::Bool(true), kInt32), utils::TypeMismatchException);
    EXPECT_THROW((void)CoerceToColumn(Value::Int32(1), kBool), utils::TypeMismatchException);
}

TEST(CoerceTest, FloatIntoIntegerColumnThrowsRatherThanTruncating) {
    EXPECT_THROW((void)CoerceToColumn(Value::Float64(2.7), kInt32), utils::TypeMismatchException);
    EXPECT_THROW((void)CoerceToColumn(Value::Float32(2.0f), kInt64), utils::TypeMismatchException);
}

TEST(CoerceTest, MismatchMessageNamesColumnAndBothTypes) {
    try {
        (void)CoerceToColumn(Value::Varchar("x"), kInt32);
        FAIL() << "expected TypeMismatchException";
    } catch (const utils::TypeMismatchException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("i32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("INT32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("VARCHAR"), std::string::npos) << msg;
    }
}

// ---- NULL ----------------------------------------------------------------------

TEST(CoerceTest, NullIntoNullableColumnTakesTheColumnType) {
    const Value out = CoerceToColumn(Value::Null(ColumnType::VARCHAR), kNullableInt);
    EXPECT_TRUE(out.IsNull());
    EXPECT_EQ(out.Type(), ColumnType::INT32);
}

TEST(CoerceTest, NullIntoNotNullColumnThrows) {
    EXPECT_THROW((void)CoerceToColumn(Value::Null(ColumnType::INT32), kInt32),
                 utils::AxolotException);
}
