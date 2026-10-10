#pragma once

#include <storage/page.h>
#include <storage/page_guard.h>
#include <utils/errors.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace AxolotlDB::storage {

class TableHeap {
   public:
    /// @param sm     The shared StorageManager. TableHeap does not own it.
    /// @param file   FileId this heap's pages live in. Must already be open via
    ///               StorageManager::OpenFile.
    /// @param schema The row schema. Stored by value; TableHeap does not
    ///               interpret tuple bytes itself (that is Tuple::Deserialize's
    ///               job) but needs it to size-check inserts against PAGE_SIZE.
    explicit TableHeap(StorageManager& sm, FileId fid);
    TableHeap(const TableHeap&) = delete;
    TableHeap& operator=(const TableHeap&) = delete;
    TableHeap(TableHeap&&) = delete;
    TableHeap& operator=(TableHeap&&) = delete;
    ~TableHeap() = default;

    /// @returns The RID of the stored row. Visible to a subsequent Get()
    ///          immediately; durable only after the containing file is flushed
    ///          (see StorageManager::FlushFile / FlushAll).
    /// @throws  ValueTooLargeException if `tuple` cannot fit on any page --
    ///          i.e. it exceeds PAGE_SIZE - sizeof(PageHeader) - sizeof(SlotEntry)
    ///          even on an empty page. Thrown before any page is touched.
    [[nodiscard]] RID Insert(std::span<const std::byte> tuple);

    /// @param rid   The RID of the tuple to retrieve.
    /// @returns A mutable span view over the tuple's bytes. The view remains valid
    ///          as long as the page is pinned (see PageGuard). Modifying the
    ///          contents invalidates the tuple, which must be re-serialized and
    ///          re-inserted.
    /// @throws  InvalidPageException if `rid` does not correspond to a currently-
    ///          stored (i.e. non-deleted) tuple. Throws before pinning.
    [[nodiscard]] std::span<const std::byte> Get(RID rid, ReadPageGuard& out) const;

    /// @throws InvalidSlotException if `rid` does not resolve to a live row.
    void Delete(RID rid);

    /// @throws ValueTooLargeException under the same condition as Insert.
    /// @throws InvalidSlotException if `rid` does not resolve to a live row.
    [[nodiscard]] RID Update(RID rid, std::span<const std::byte> new_tuple);

    class Iterator {
       public:
        Iterator() = default;  // the end() sentinel

        [[nodiscard]] RID operator*() const;
        Iterator& operator++();
        Iterator operator++(int);
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept;
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept;

       private:
        friend class TableHeap;
        Iterator(const TableHeap* heap, PageId page_id, SlotIdx slot);
        void SeekNextLive();

        const TableHeap* heap_ = nullptr;
        PageId page_id_ = INVALID_PAGE_ID;
        SlotIdx slot_ = 0;
    };

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

   private:
    [[nodiscard]] PageRef FindOrAllocatePageFor(std::size_t payload_len);
    void RefreshFreeSpaceEntry(PageRef page, std::size_t new_size, std::size_t old_size);
    StorageManager& storage_;
    FileId file_;
    PageId first_page_ = INVALID_PAGE_ID;
    std::set<std::pair<uint32_t, PageId>> free_space_map_;
};

}  // namespace AxolotlDB::storage