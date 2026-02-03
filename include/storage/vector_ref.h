#pragma once

#include <cstdint>
#include <type_traits>

namespace AxolotlDB::storage {

using HeapId = uint32_t;

inline constexpr HeapId INVALID_HEAP_ID = 0;

struct VectorRef {
    HeapId heap_id = INVALID_HEAP_ID;
    uint32_t index = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return heap_id != INVALID_HEAP_ID;
    }

    friend bool operator==(const VectorRef&, const VectorRef&) noexcept = default;
};

static_assert(sizeof(VectorRef) == 8,
              "VectorRef must stay 8 bytes: it is a fixed-width column payload");
static_assert(std::is_trivially_copyable_v<VectorRef>, "VectorRef is memcpy'd into tuple bytes");

}  // namespace AxolotlDB::storage
