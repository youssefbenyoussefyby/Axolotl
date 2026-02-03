#include <storage/tuple.h>
#include <utils/errors.h>

#include <cstring>
#include <stdexcept>

namespace AxolotlDB::storage {

static constexpr std::size_t FixedSize(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::INT32:
            return 4;
        case ColumnType::INT64:
            return 8;
        case ColumnType::FLOAT32:
            return 4;
        case ColumnType::FLOAT64:
            return 8;
        case ColumnType::BOOLEAN:
            return 1;
        case ColumnType::VECTOR:
            return sizeof(VectorRef);
        case ColumnType::VARCHAR:
            return 0;
    }
    return 0;
}

static bool IsVariable(ColumnType t) noexcept {
    return t == ColumnType::VARCHAR;
}

static void RequireColumnType(const ColumnDef& column, const Value& value) {
    if (value.Type() != column.type) {
        throw AxolotlDB::utils::TypeMismatchException("column '" + column.name + "'",
                                                     ColumnTypeName(column.type),
                                                     ColumnTypeName(value.Type()));
    }
}

std::vector<std::byte> Tuple::Serialize(const Schema& schema, const std::vector<Value>& values) {
    const std::size_t N = schema.ColumnCount();
    if (values.size() != N) {
        throw std::invalid_argument("Tuple::Serialize: values count (" +
                                    std::to_string(values.size()) + ") != schema column count (" +
                                    std::to_string(N) + ")");
    }
    for (std::size_t i = 0; i < N; ++i) {
        RequireColumnType(schema.columns[i], values[i]);
    }

    const std::size_t bitmap_bytes = (N + 7) / 8;
    std::size_t fixed_bytes = 0;
    for (std::size_t i = 0; i < N; i++) {
        if (!values[i].IsNull() && !IsVariable(schema.columns[i].type)) {
            fixed_bytes += FixedSize(schema.columns[i].type);
        }
    }
    std::size_t var_col_count = 0;
    std::size_t var_data_bytes = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (IsVariable(schema.columns[i].type)) {
            var_col_count++;
            if (!values[i].IsNull()) {
                const std::size_t col_bytes = values[i].AsVarchar().size();
                if (col_bytes > MAX_VAR_COLUMN_BYTES) {
                    throw AxolotlDB::utils::ValueTooLargeException(schema.columns[i].name, col_bytes,
                                                                  MAX_VAR_COLUMN_BYTES);
                }
                var_data_bytes += col_bytes;
            }
        }
    }
    const std::size_t var_offset_bytes = var_col_count * sizeof(uint16_t);
    const std::size_t total =
        bitmap_bytes + fixed_bytes + var_offset_bytes + var_data_bytes + sizeof(uint8_t);
    std::vector<std::byte> buf(total, std::byte{0});
    buf[0] = std::byte{TUPLE_FORMAT_VERSION};
    std::byte* p = buf.data() + 1;
    for (std::size_t i = 0; i < N; i++) {
        if (values[i].IsNull()) {
            buf[1 + i / 8] |= std::byte{1u << (i % 8)};
        }
    }
    p += bitmap_bytes;

    for (std::size_t i = 0; i < N; i++) {
        if (values[i].IsNull() || IsVariable(schema.columns[i].type))
            continue;
        switch (schema.columns[i].type) {
            case ColumnType::INT32: {
                const int32_t v = values[i].AsInt32();
                std::memcpy(p, &v, 4);
                p += 4;
                break;
            }
            case ColumnType::INT64: {
                const int64_t v = values[i].AsInt64();
                std::memcpy(p, &v, 8);
                p += 8;
                break;
            }
            case ColumnType::FLOAT32: {
                const float v = values[i].AsFloat32();
                std::memcpy(p, &v, 4);
                p += 4;
                break;
            }
            case ColumnType::FLOAT64: {
                const double v = values[i].AsFloat64();
                std::memcpy(p, &v, 8);
                p += 8;
                break;
            }
            case ColumnType::BOOLEAN: {
                uint8_t b = values[i].AsBool() ? 1 : 0;
                std::memcpy(p, &b, 1);
                p += 1;
                break;
            }
            case ColumnType::VECTOR: {
                const VectorRef ref = values[i].AsVector();
                if (!ref.IsValid()) {
                    throw AxolotlDB::utils::StorageException(
                        "Tuple::Serialize: column '" + schema.columns[i].name +
                        "' holds a VectorRef with no heap id -- store the vector in its VectorHeap "
                        "first, or use NULL");
                }
                std::memcpy(p, &ref, sizeof(VectorRef));
                p += sizeof(VectorRef);
                break;
            }
            default:
                break;
        }
    }

    std::byte* offset_table = p;
    p += var_offset_bytes;
    std::byte* var_start = p;

    std::size_t var_idx = 0;
    std::size_t var_written = 0;
    for (std::size_t i = 0; i < N; i++) {
        if (!IsVariable(schema.columns[i].type))
            continue;

        uint16_t size = 0;
        if (!values[i].IsNull()) {
            const std::string& s = values[i].AsVarchar();
            std::memcpy(var_start + var_written, s.data(), s.size());
            var_written += s.size();
            size = static_cast<uint16_t>(s.size());
        }
        std::memcpy(offset_table + var_idx * sizeof(uint16_t), &size, sizeof(uint16_t));

        var_idx++;
    }
    return buf;
}

std::vector<Value> Tuple::Deserialize(const Schema& schema, std::span<const std::byte> raw) {
    const std::size_t N = schema.ColumnCount();
    const std::size_t bitmap_bytes = (N + 7) / 8;

    if (raw.size() < bitmap_bytes) {
        throw AxolotlDB::utils::StorageException(
            "Tuple::Deserialize: buffer too small for null bitmap");
    }

    std::vector<Value> values(N);
    const auto version = static_cast<uint8_t>(raw[0]);
    if (version != TUPLE_FORMAT_VERSION) {
        throw AxolotlDB::utils::StorageException("Tuple::Deserialize: unsupported format version " +
                                                std::to_string(version));
    }

    const std::byte* p = raw.data() + 1;

    std::vector<bool> is_null(N, false);
    for (std::size_t i = 0; i < N; i++) {
        is_null[i] = (static_cast<uint8_t>(p[i / 8]) & (1u << (i % 8))) != 0;
        values[i] = Value::Null(schema.columns[i].type);
    }
    p += bitmap_bytes;

    std::size_t fixed_bytes = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (!is_null[i] && !IsVariable(schema.columns[i].type)) {
            fixed_bytes += FixedSize(schema.columns[i].type);
        }
    }
    if (raw.size() < bitmap_bytes + fixed_bytes + sizeof(uint8_t)) {
        throw AxolotlDB::utils::StorageException(
            "Tuple::Deserialize: buffer too small for fixed columns");
    }
    for (std::size_t i = 0; i < N; i++) {
        if (is_null[i] || IsVariable(schema.columns[i].type))
            continue;
        switch (schema.columns[i].type) {
            case ColumnType::INT32: {
                int32_t v = 0;
                std::memcpy(&v, p, 4);
                values[i] = Value::Int32(v);
                p += 4;
                break;
            }
            case ColumnType::INT64: {
                int64_t v = 0;
                std::memcpy(&v, p, 8);
                values[i] = Value::Int64(v);
                p += 8;
                break;
            }
            case ColumnType::FLOAT32: {
                float v = 0.0f;
                std::memcpy(&v, p, 4);
                values[i] = Value::Float32(v);
                p += 4;
                break;
            }
            case ColumnType::FLOAT64: {
                double v = 0.0;
                std::memcpy(&v, p, 8);
                values[i] = Value::Float64(v);
                p += 8;
                break;
            }
            case ColumnType::BOOLEAN: {
                uint8_t b = 0;
                std::memcpy(&b, p, 1);
                values[i] = Value::Bool(b != 0);
                p += 1;
                break;
            }
            case ColumnType::VECTOR: {
                VectorRef ref{};
                std::memcpy(&ref, p, sizeof(VectorRef));
                values[i] = Value::Vector(ref);
                p += sizeof(VectorRef);
                break;
            }
            default:
                break;
        }
    }

    std::size_t var_col_count = 0;
    for (std::size_t i = 0; i < N; i++) {
        if (IsVariable(schema.columns[i].type))
            var_col_count++;
    }
    const std::size_t var_offset_bytes = var_col_count * sizeof(uint16_t);
    if (raw.size() < bitmap_bytes + fixed_bytes + var_offset_bytes + sizeof(uint8_t)) {
        throw AxolotlDB::utils::StorageException(
            "Tuple::Deserialize: buffer too small for offset table");
    }

    const std::byte* offset_table = p;
    const std::byte* var_start = p + var_offset_bytes;

    std::vector<uint16_t> lengths(var_col_count);
    for (std::size_t k = 0; k < var_col_count; k++) {
        std::memcpy(&lengths[k], offset_table + k * sizeof(uint16_t), sizeof(uint16_t));
    }
    std::size_t total_var_bytes = 0;
    for (std::size_t k = 0; k < var_col_count; ++k)
        total_var_bytes += lengths[k];

    if (raw.size() <
        bitmap_bytes + fixed_bytes + var_offset_bytes + total_var_bytes + sizeof(uint8_t)) {
        throw AxolotlDB::utils::StorageException(
            "Tuple::Deserialize: buffer too small for variable-length data");
    }

    std::size_t var_idx = 0;
    std::size_t var_written = 0;
    for (std::size_t i = 0; i < N; i++) {
        if (!IsVariable(schema.columns[i].type))
            continue;
        if (!is_null[i]) {
            values[i] =
                Value::Varchar(std::string(reinterpret_cast<const char*>(var_start + var_written),
                                           static_cast<std::size_t>(lengths[var_idx])));
        }
        var_written += lengths[var_idx];
        var_idx++;
    }

    return values;
}

}  // namespace AxolotlDB::storage
