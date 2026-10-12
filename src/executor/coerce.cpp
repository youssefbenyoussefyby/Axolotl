#include <executor/coerce.h>
#include <utils/errors.h>

#include <cstdint>
#include <limits>
#include <string>

namespace AxolotlDB::executor {

using storage::ColumnType;
using storage::Value;

namespace {

bool IsInteger(ColumnType t) {
    return t == ColumnType::INT32 || t == ColumnType::INT64;
}

bool IsFloat(ColumnType t) {
    return t == ColumnType::FLOAT32 || t == ColumnType::FLOAT64;
}

int64_t AsInteger(const Value& v) {
    return v.Type() == ColumnType::INT32 ? v.AsInt32() : v.AsInt64();
}

double AsDouble(const Value& v) {
    switch (v.Type()) {
        case ColumnType::INT32:
            return v.AsInt32();
        case ColumnType::INT64:
            return static_cast<double>(v.AsInt64());
        case ColumnType::FLOAT32:
            return v.AsFloat32();
        default:
            return v.AsFloat64();
    }
}

[[noreturn]] void Mismatch(const storage::ColumnDef& column, const Value& value) {
    throw utils::TypeMismatchException("column '" + column.name + "'",
                                       storage::ColumnTypeName(column.type),
                                       storage::ColumnTypeName(value.Type()));
}

}  // namespace

Value CoerceToColumn(const Value& value, const storage::ColumnDef& column) {
    if (value.IsNull()) {
        if (!column.nullable) {
            throw utils::AxolotException("Column '" + column.name +
                                         "' is NOT NULL; cannot store NULL");
        }
        return Value::Null(column.type);
    }

    const ColumnType from = value.Type();
    if (from == column.type) {
        return value;
    }

    switch (column.type) {
        case ColumnType::INT32: {
            if (!IsInteger(from)) {
                Mismatch(column, value);
            }
            const int64_t i = AsInteger(value);
            if (i < std::numeric_limits<int32_t>::min() ||
                i > std::numeric_limits<int32_t>::max()) {
                throw utils::AxolotException("Value " + std::to_string(i) +
                                             " is out of range for INT32 column '" + column.name +
                                             "'");
            }
            return Value::Int32(static_cast<int32_t>(i));
        }
        case ColumnType::INT64:
            if (!IsInteger(from)) {
                Mismatch(column, value);
            }
            return Value::Int64(AsInteger(value));
        case ColumnType::FLOAT32:
            if (!IsInteger(from) && !IsFloat(from)) {
                Mismatch(column, value);
            }
            return Value::Float32(static_cast<float>(AsDouble(value)));
        case ColumnType::FLOAT64:
            if (!IsInteger(from) && !IsFloat(from)) {
                Mismatch(column, value);
            }
            return Value::Float64(AsDouble(value));
        default:
            Mismatch(column, value);
    }
}

}  // namespace AxolotlDB::executor
