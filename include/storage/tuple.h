#pragma once

#include <storage/page.h>
#include <storage/vector_ref.h>
#include <utils/errors.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace AxolotlDB::storage {

inline constexpr uint8_t TUPLE_FORMAT_VERSION = 1;

enum class ColumnType : uint8_t {
    INT32 = 0,
    INT64 = 1,
    FLOAT32 = 2,
    FLOAT64 = 3,
    VARCHAR = 4,
    BOOLEAN = 5,
    VECTOR = 6,
};

struct ColumnDef {
    std::string name;
    ColumnType type = ColumnType::INT32;
    bool nullable = false;
    uint32_t dimension = 0;

    ColumnDef() = default;
    ColumnDef(std::string n, ColumnType t)
        : name(std::move(n)), type(t), nullable(false), dimension(0) {}
    ColumnDef(std::string n, ColumnType t, bool null)
        : name(std::move(n)), type(t), nullable(null), dimension(0) {}
    ColumnDef(std::string n, ColumnType t, bool null, uint32_t dim)
        : name(std::move(n)), type(t), nullable(null), dimension(dim) {}
    ColumnDef(std::string n, ColumnType t, uint32_t dim)
        : name(std::move(n)), type(t), nullable(false), dimension(dim) {}
    ColumnDef(std::string n, ColumnType t, int dim)
        : name(std::move(n)), type(t), nullable(false), dimension(static_cast<uint32_t>(dim)) {}
};

struct Schema {
    std::vector<ColumnDef> columns;

    Schema() = default;
    Schema(std::vector<ColumnDef> cols) : columns(std::move(cols)) {}
    Schema(std::initializer_list<ColumnDef> cols) : columns(cols) {}

    [[nodiscard]] std::size_t ColumnCount() const noexcept {
        return columns.size();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return columns.size();
    }
};

inline constexpr std::size_t MAX_VAR_COLUMN_BYTES = 65535;

[[nodiscard]] constexpr const char* ColumnTypeName(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::INT32:
            return "INT32";
        case ColumnType::INT64:
            return "INT64";
        case ColumnType::FLOAT32:
            return "FLOAT32";
        case ColumnType::FLOAT64:
            return "FLOAT64";
        case ColumnType::VARCHAR:
            return "VARCHAR";
        case ColumnType::BOOLEAN:
            return "BOOLEAN";
        case ColumnType::VECTOR:
            return "VECTOR";
    }
    return "UNKNOWN";
}

struct NullValue {
    ColumnType type = ColumnType::INT32;
    friend bool operator==(const NullValue&, const NullValue&) noexcept = default;
};

class Value {
   public:
    using Payload =
        std::variant<NullValue, int32_t, int64_t, float, double, bool, std::string, VectorRef>;

    Value() = default;

    static Value Int32(int32_t v) {
        return Value(Payload(v));
    }
    static Value Int64(int64_t v) {
        return Value(Payload(v));
    }
    static Value Float32(float v) {
        return Value(Payload(v));
    }
    static Value Float64(double v) {
        return Value(Payload(v));
    }
    static Value Bool(bool v) {
        return Value(Payload(v));
    }
    static Value Varchar(std::string v) {
        return Value(Payload(std::move(v)));
    }

    static Value Vector(VectorRef ref) {
        return Value(Payload(ref));
    }

    static Value Null(ColumnType t) {
        return Value(Payload(NullValue{t}));
    }

    [[nodiscard]] ColumnType Type() const noexcept {
        if (const NullValue* n = std::get_if<NullValue>(&payload_))
            return n->type;
        switch (payload_.index()) {
            case 1:
                return ColumnType::INT32;
            case 2:
                return ColumnType::INT64;
            case 3:
                return ColumnType::FLOAT32;
            case 4:
                return ColumnType::FLOAT64;
            case 5:
                return ColumnType::BOOLEAN;
            case 6:
                return ColumnType::VARCHAR;
            case 7:
                return ColumnType::VECTOR;
            default:
                return ColumnType::INT32;
        }
    }

    [[nodiscard]] bool IsNull() const noexcept {
        return std::holds_alternative<NullValue>(payload_);
    }

    /// @throws AxolotlDB::utils::TypeMismatchException if a different
    ///         alternative is held (NULL included).
    [[nodiscard]] int32_t AsInt32() const {
        return Get<int32_t>("INT32");
    }
    [[nodiscard]] int64_t AsInt64() const {
        return Get<int64_t>("INT64");
    }
    [[nodiscard]] float AsFloat32() const {
        return Get<float>("FLOAT32");
    }
    [[nodiscard]] double AsFloat64() const {
        return Get<double>("FLOAT64");
    }
    [[nodiscard]] bool AsBool() const {
        return Get<bool>("BOOLEAN");
    }
    [[nodiscard]] const std::string& AsVarchar() const {
        return Get<std::string>("VARCHAR");
    }
    [[nodiscard]] VectorRef AsVector() const {
        return Get<VectorRef>("VECTOR");
    }

    [[nodiscard]] const Payload& Raw() const noexcept {
        return payload_;
    }

    [[nodiscard]] const char* PayloadName() const noexcept {
        return IsNull() ? "NULL" : ColumnTypeName(Type());
    }

    friend bool operator==(const Value&, const Value&) = default;

   private:
    explicit Value(Payload p) : payload_(std::move(p)) {}

    template <typename T>
    const T& Get(const char* expected) const {
        if (const T* p = std::get_if<T>(&payload_))
            return *p;
        throw AxolotlDB::utils::TypeMismatchException("Value", expected, PayloadName());
    }

    Payload payload_{};
};

class Tuple {
   public:
    Tuple() = default;

    /// @throws std::invalid_argument if values.size() != schema.ColumnCount()
    static std::vector<std::byte> Serialize(const Schema& schema, const std::vector<Value>& values);

    /// @throws AxolotlDB::utils::StorageException on corrupt data.
    static std::vector<Value> Deserialize(const Schema& schema, std::span<const std::byte> data);

    [[nodiscard]] RID GetRID() const noexcept {
        return rid_;
    }
    void SetRID(RID r) noexcept {
        rid_ = r;
    }

    [[nodiscard]] const std::vector<std::byte>& Data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::span<const std::byte> Span() const noexcept {
        return data_;
    }

    explicit Tuple(std::vector<std::byte> data, RID rid = {}) : data_(std::move(data)), rid_(rid) {}

   private:
    std::vector<std::byte> data_;
    RID rid_;
};

}  // namespace AxolotlDB::storage
