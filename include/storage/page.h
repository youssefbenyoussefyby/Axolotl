#pragma once

#include <utils/errors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace AxolotlDB::storage {

using PageId = int32_t;
using SlotIdx = uint16_t;
inline constexpr PageId INVALID_PAGE_ID = -1;
inline constexpr SlotIdx INVALID_SLOT = 0xFFFF;
struct RID {
    PageId page_id = INVALID_PAGE_ID;
    SlotIdx slot_idx = INVALID_SLOT;

    [[nodiscard]] bool IsValid() const noexcept {
        return page_id != INVALID_PAGE_ID && slot_idx != INVALID_SLOT;
    }

    bool operator==(const RID& o) const noexcept {
        return page_id == o.page_id && slot_idx == o.slot_idx;
    }
    bool operator!=(const RID& o) const noexcept {
        return !(*this == o);
    }
};

inline constexpr uint32_t PAGE_SIZE = 4096;

struct PageHeader {
    PageId page_id = INVALID_PAGE_ID;
    uint16_t slot_count = 0;
    uint16_t free_ptr = PAGE_SIZE;
    uint64_t page_lsn = 0;
    uint32_t checksum = 0;
    uint32_t _pad2 = 0;
};
static_assert(sizeof(PageHeader) == 24, "PageHeader must be exactly 16 bytes");

struct SlotEntry {
    uint16_t offset = 0;
    uint16_t length = 0;
};
static_assert(sizeof(SlotEntry) == 4, "SlotEntry must be exactly 4 bytes");

inline constexpr uint16_t MAX_SLOTS =
    static_cast<uint16_t>((PAGE_SIZE - sizeof(PageHeader)) / sizeof(SlotEntry));

class Page {
   public:
    Page();
    explicit Page(const std::array<std::byte, PAGE_SIZE>& raw);

    [[nodiscard]] PageId GetPageId() const noexcept;
    void SetPageId(PageId id) noexcept;

    /// @returns The slot index of the new tuple.
    /// @throws  PageFullException if there is not enough free space.
    [[nodiscard]] SlotIdx InsertTuple(std::span<const std::byte> data);

    /// @returns A span into the page's internal buffer (zero-copy).
    /// @throws  InvalidSlotException if `slot` is out of range or deleted.
    [[nodiscard]] std::span<const std::byte> GetTuple(SlotIdx slot) const;

    /// @throws  InvalidSlotException if `slot` is out of range or already deleted.
    void DeleteTuple(SlotIdx slot);
    /// @throws  InvalidSlotException if `slot` is out of range or deleted.
    void UpdateTupleInPlace(SlotIdx slot, std::span<const std::byte> data);
    /// @throws CorruptPageException naming the invariant that was violated.
    void ValidateOrThrow() const;
    [[nodiscard]] uint32_t FreeSpace() const noexcept;

    [[nodiscard]] uint16_t SlotCount() const noexcept;
    [[nodiscard]] bool IsDirty() const noexcept;
    void MarkDirty() noexcept;
    void ClearDirty() noexcept;
    [[nodiscard]] const std::array<std::byte, PAGE_SIZE>& RawData() const noexcept;
    [[nodiscard]] std::array<std::byte, PAGE_SIZE>& RawData() noexcept;
    void SetPageLSN(uint64_t lsn) noexcept;
    [[nodiscard]] uint64_t GetPageLSN() const noexcept;
    void SetChecksum(uint32_t checksum) noexcept;
    [[nodiscard]] uint32_t GetChecksum() const noexcept;
    bool IsValidChecksum() const noexcept;
    void UpdateChecksum() noexcept;
    uint32_t ComputeChecksum() const noexcept;
    void Compact() noexcept;

   private:
    std::array<std::byte, PAGE_SIZE> data_{};
    bool isDirty_ = false;
    [[nodiscard]] PageHeader& Header() noexcept;
    [[nodiscard]] const PageHeader& Header() const noexcept;

    [[nodiscard]] SlotEntry* Slots() noexcept;
    [[nodiscard]] const SlotEntry* Slots() const noexcept;
    [[nodiscard]] uint32_t SlotArrayEnd() const noexcept;
    void EnsureHeaderSane() const;
    void EnsureSlotInBounds(SlotIdx slot, const SlotEntry& entry) const;
};

}  // namespace AxolotlDB::storage
