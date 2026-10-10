#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>

namespace AxolotlDB::storage {
using value_type = std::pair<RID, std::span<const std::byte>>;

TableHeap::TableHeap(StorageManager& sm, FileId file) : storage_(sm), file_(file) {
    if (file_ == INVALID_FILE_ID)
        throw std::invalid_argument(
            "TableHeap: file must be a FileId returned by StorageManager::OpenFile,got "
            "INVALID_FILE_ID");

    if (storage_.PageCount(file_) > 0) {
        first_page_ = 0;
        for (uint32_t page_id = 0; page_id < storage_.PageCount(file_); page_id++) {
            auto guard = storage_.FetchRead(PageRef{file_, static_cast<PageId>(page_id)});
            free_space_map_.insert({guard->FreeSpace(), page_id});
        }
    }

    else {
        first_page_ = INVALID_PAGE_ID;
    }
}

PageRef TableHeap::FindOrAllocatePageFor(std::size_t payload_len) {
    auto it = free_space_map_.lower_bound({payload_len, 0});

    constexpr std::size_t max_payload = PAGE_SIZE - sizeof(PageHeader) - sizeof(SlotEntry);

    if (payload_len > max_payload) {
        throw utils::ValueTooLargeException("", payload_len, max_payload);
    }

    if (it == free_space_map_.end()) {
        if (first_page_ == INVALID_PAGE_ID)
            first_page_ = 0;
        PageRef page_ref;
        WritePageGuard new_page = storage_.NewPage(file_, page_ref);
        free_space_map_.insert({new_page->FreeSpace(), new_page->GetPageId()});
        return page_ref;
    }
    PageId page_id = it->second;
    free_space_map_.erase(it);
    return PageRef{file_, page_id};
}

void TableHeap::RefreshFreeSpaceEntry(PageRef page, std::size_t new_size,
                                      std::size_t old_free_space) {
    free_space_map_.erase({old_free_space, page.page_no});
    free_space_map_.insert({new_size, page.page_no});
}
RID TableHeap::Insert(std::span<const std::byte> tuple) {
    PageRef page = FindOrAllocatePageFor(tuple.size());
    WritePageGuard guard = storage_.FetchWrite(page);
    std::size_t old_free_space = guard->FreeSpace();

    SlotIdx slot = guard->InsertTuple(tuple);
    std::size_t new_size = guard->FreeSpace();
    RefreshFreeSpaceEntry(page, new_size, old_free_space);
    return RID{page.page_no, slot};
}

std::span<const std::byte> TableHeap::Get(RID rid, ReadPageGuard& out) const {
    PageRef ref{file_, rid.page_id};
    out = storage_.FetchRead(ref);
    return out->GetTuple(rid.slot_idx);
}
void TableHeap::Delete(RID rid) {
    PageRef ref{file_, rid.page_id};
    WritePageGuard guard = storage_.FetchWrite(ref);
    std::size_t old_size = guard->FreeSpace();
    guard->DeleteTuple(rid.slot_idx);
    guard->Compact();
    std::size_t new_size = guard->FreeSpace();
    RefreshFreeSpaceEntry(ref, new_size, old_size);
}

RID TableHeap::Update(RID rid, std::span<const std::byte> tuple) {
    PageRef ref{file_, rid.page_id};
    WritePageGuard guard = storage_.FetchWrite(ref);
    std::span<const std::byte> old_data = guard->GetTuple(rid.slot_idx);
    if (tuple.size() <= old_data.size()) {
        guard->UpdateTupleInPlace(rid.slot_idx, tuple);
        return rid;
    }
    guard.Drop();
    Delete(rid);
    return Insert(tuple);
}

TableHeap::Iterator TableHeap::begin() const {
    return Iterator{this, first_page_, 0};
}

TableHeap::Iterator TableHeap::end() const {
    return Iterator{};
}

TableHeap::Iterator::Iterator(const TableHeap* heap, PageId page_id, SlotIdx slot)
    : heap_(heap), page_id_(page_id), slot_(slot) {
    SeekNextLive();
}

void TableHeap::Iterator::SeekNextLive() {
    if (heap_ == nullptr || page_id_ == INVALID_PAGE_ID)
        return;

    while (page_id_ < heap_->storage_.PageCount(heap_->file_)) {
        PageRef ref{heap_->file_, page_id_};
        ReadPageGuard guard = heap_->storage_.FetchRead(ref);

        while (slot_ < guard->SlotCount()) {
            try {
                guard->GetTuple(slot_);
                return;
            } catch (const utils::InvalidSlotException&) {
                slot_++;
            }
        }
        page_id_++;
        slot_ = 0;
    }

    page_id_ = INVALID_PAGE_ID;
    heap_ = nullptr;
}

RID TableHeap::Iterator::operator*() const {
    if (heap_ == nullptr || page_id_ == INVALID_PAGE_ID ||
        slot_ >= heap_->storage_.FetchRead({heap_->file_, page_id_})->SlotCount())
        throw std::runtime_error("Iterator is not on a valid tuple");
    return RID{page_id_, slot_};
}

TableHeap::Iterator& TableHeap::Iterator::operator++() {
    slot_++;
    SeekNextLive();
    return *this;
}
TableHeap::Iterator TableHeap::Iterator::operator++(int) {
    Iterator copy = (*this);
    ++(*this);
    return copy;
}

bool TableHeap::Iterator::operator==(const Iterator& other) const noexcept {
    const bool is_end = (page_id_ == INVALID_PAGE_ID || heap_ == nullptr);
    const bool other_is_end = (other.page_id_ == INVALID_PAGE_ID || other.heap_ == nullptr);
    if (is_end && other_is_end)
        return true;
    if (is_end || other_is_end)
        return false;
    return heap_ == other.heap_ && page_id_ == other.page_id_ && slot_ == other.slot_;
}

bool TableHeap::Iterator::operator!=(const Iterator& other) const noexcept {
    return !(*this == other);
}

}  // namespace AxolotlDB::storage