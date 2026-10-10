#include <storage/vector_heap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
const std::string& RequireArgs(uint32_t dimension, AxolotlDB::storage::HeapId heap_id,
                               const std::string& path) {
    if (dimension == 0) {
        throw std::invalid_argument("VectorHeap: dimension must be > 0");
    }
    if (dimension > AxolotlDB::storage::MAX_VECTOR_DIMENSION) {
        throw std::invalid_argument("VectorHeap: dimension " + std::to_string(dimension) +
                                    " exceeds MAX_VECTOR_DIMENSION (" +
                                    std::to_string(AxolotlDB::storage::MAX_VECTOR_DIMENSION) +
                                    "); its page span would overflow the uint16_t that holds it");
    }
    if (heap_id == AxolotlDB::storage::INVALID_HEAP_ID) {
        throw std::invalid_argument("VectorHeap: heap_id must not be INVALID_HEAP_ID (0)");
    }
    return path;
}
}  // namespace

namespace AxolotlDB::storage {

Page* VectorHeap::FetchPage(PageId page_id) const {
    PageRef ref{heap_id_, page_id};
    auto r = buffer_pool_.Pin(ref);
    if (r.evicted_dirty && r.evicted_ref.file_id != INVALID_FILE_ID) {
        file_manager_.WritePage(r.evicted_ref.page_no, *r.page);
    }
    if (!r.was_in_pool) {
        try {
            file_manager_.ReadPage(page_id, *r.page);
            r.page->ClearDirty();
        } catch (...) {
            buffer_pool_.UnpinAndDrop(ref);
            throw;
        }
    }
    return r.page;
}

Page* VectorHeap::NewPage(PageId& out_page_id) const {
    out_page_id = file_manager_.AllocatePage();
    PageRef ref{heap_id_, out_page_id};
    auto r = buffer_pool_.Pin(ref);
    if (r.evicted_dirty && r.evicted_ref.file_id != INVALID_FILE_ID) {
        file_manager_.WritePage(r.evicted_ref.page_no, *r.page);
    }
    *r.page = Page();
    r.page->SetPageId(out_page_id);
    r.page->MarkDirty();
    return r.page;
}

void VectorHeap::UnpinPage(PageId page_id, bool is_dirty) const {
    buffer_pool_.Unpin(PageRef{heap_id_, page_id}, is_dirty);
}

void VectorHeap::FlushAllPages() const {
    buffer_pool_.ForEachDirty([this](PageRef ref, Page& p) {
        if (ref.file_id == heap_id_) {
            file_manager_.WritePage(ref.page_no, p);
            p.ClearDirty();
        }
    });
    file_manager_.Sync();
}

VectorHeap::VectorHeap(const std::string& path, HeapId heap_id, uint32_t dimension,
                       std::size_t pool_size)
    : file_manager_(RequireArgs(dimension, heap_id, path), PAGE_SIZE),
      buffer_pool_(pool_size),
      dimension_(dimension),
      heap_id_(heap_id),
      vector_count_(0),
      durable_count_(0) {
    const PageId existing_pages = file_manager_.PageCount();
    if (existing_pages == 0) {
        PageId header_page_id = INVALID_PAGE_ID;
        Page* header_page = NewPage(header_page_id);
        VectorHeapHeader header{dimension_, heap_id_, 0};
        vector_count_ = 0;
        durable_count_ = 0;
        std::memcpy(header_page->RawData().data(), &header, sizeof(header));
        FlushAllPages();
        UnpinPage(header_page_id, false);
    } else {
        Page* header_page = FetchPage(VECTOR_HEAP_HEADER_PAGE);
        VectorHeapHeader header{};
        std::memcpy(&header, header_page->RawData().data(), sizeof(header));
        if (header.dimension != dimension) {
            UnpinPage(VECTOR_HEAP_HEADER_PAGE, false);
            throw std::invalid_argument("VectorHeap: dimension mismatch with existing file");
        }

        if (header.heap_id != heap_id_) {
            UnpinPage(VECTOR_HEAP_HEADER_PAGE, false);
            throw std::invalid_argument(
                "VectorHeap: heap id mismatch with existing file (file says " +
                std::to_string(header.heap_id) + ", opened as " + std::to_string(heap_id_) + ")");
        }
        vector_count_ = header.vector_count;
        durable_count_ = header.vector_count;
        UnpinPage(VECTOR_HEAP_HEADER_PAGE, false);
    }
}

AddressingMath VectorHeap::LocateVector(VectorId id) const {
    const uint64_t stride = static_cast<uint64_t>(dimension_) * sizeof(float);
    if (stride <= useable_page_size) {
        const uint32_t records_per_page = static_cast<uint32_t>(useable_page_size / stride);
        const uint64_t page_offset = id / records_per_page;
        if (page_offset > static_cast<uint64_t>(INT32_MAX - 1)) {
            throw AxolotlDB::utils::StorageException(
                "VectorHeap::LocateVector: id " + std::to_string(id) + " maps to page " +
                std::to_string(1 + page_offset) + ", which overflows the 32-bit PageId range");
        }
        const PageId page_id = static_cast<PageId>(1 + page_offset);
        const uint16_t offset = static_cast<uint16_t>((id % records_per_page) * stride);
        return {page_id, offset, 1};
    }

    const uint32_t pages_per_records =
        static_cast<uint32_t>((stride + useable_page_size - 1) / useable_page_size);
    const uint64_t page_offset = id * pages_per_records;
    if (page_offset > static_cast<uint64_t>(INT32_MAX - 1)) {
        throw AxolotlDB::utils::StorageException(
            "VectorHeap::LocateVector: id " + std::to_string(id) + " maps to page " +
            std::to_string(1 + page_offset) + ", which overflows the 32-bit PageId range");
    }
    const PageId page_id = static_cast<PageId>(1 + page_offset);
    const uint16_t offset = 0;
    const uint16_t page_span = static_cast<uint16_t>(pages_per_records);

    return {page_id, offset, page_span};
}

Page* VectorHeap::AcquirePage(PageId page_id) const {
    if (page_id < file_manager_.PageCount()) {
        return FetchPage(page_id);
    }

    PageId allocated = INVALID_PAGE_ID;
    Page* page = NewPage(allocated);
    if (allocated != page_id) {
        UnpinPage(allocated, false);
        throw AxolotlDB::utils::IOException("VectorHeap: expected to allocate page " +
                                            std::to_string(page_id) + " but FileManager returned " +
                                            std::to_string(allocated));
    }
    return page;
}

VectorId VectorHeap::Put(std::span<const float> vector) {
    if (vector.size() != dimension_) {
        throw AxolotlDB::utils::VectorDimensionMismatchException("VectorHeap::Put", dimension_,
                                                                 vector.size());
    }

    const VectorId vector_id = vector_count_;
    const AddressingMath info = LocateVector(vector_id);

    if (info.page_span == 1) {
        Page* page = AcquirePage(info.pid);
        std::memcpy(page->RawData().data() + info.offsets, vector.data(),
                    vector.size() * sizeof(float));
        page->MarkDirty();
        UnpinPage(info.pid, false);
    } else {
        const uint32_t floats_per_page = useable_page_size / sizeof(float);
        for (uint16_t i = 0; i < info.page_span; i++) {
            const PageId page_id = info.pid + static_cast<PageId>(i);
            Page* page = AcquirePage(page_id);
            const uint32_t first = i * floats_per_page;
            const uint32_t last = std::min((i + 1u) * floats_per_page, dimension_);
            std::memcpy(page->RawData().data(), vector.data() + first,
                        static_cast<std::size_t>(last - first) * sizeof(float));
            page->MarkDirty();
            UnpinPage(page_id, false);
        }
    }
    vector_count_++;
    return vector_id;
}

std::vector<float> VectorHeap::Get(VectorId id) const {
    if (id >= vector_count_) {
        throw AxolotlDB::utils::InvalidVectorIdException(id, vector_count_);
    }
    AddressingMath info = LocateVector(id);
    std::vector<float> result(dimension_);

    if (info.page_span == 1) {
        Page* page = FetchPage(info.pid);
        std::memcpy(result.data(), page->RawData().data() + info.offsets,
                    dimension_ * sizeof(float));
        UnpinPage(info.pid, false);
    } else {
        const uint32_t floats_per_page = useable_page_size / sizeof(float);
        for (uint16_t i = 0; i < info.page_span; i++) {
            const PageId page_id = info.pid + static_cast<PageId>(i);
            const uint32_t first = i * floats_per_page;
            const uint32_t last = std::min((i + 1u) * floats_per_page, dimension_);

            Page* page = FetchPage(page_id);
            std::memcpy(result.data() + first, page->RawData().data(),
                        static_cast<std::size_t>(last - first) * sizeof(float));
            UnpinPage(page_id, false);
        }
    }
    return result;
}

VectorRef VectorHeap::Ref(VectorId id) const {
    if (id >= vector_count_) {
        throw AxolotlDB::utils::InvalidVectorIdException(id, vector_count_);
    }
    if (id > UINT32_MAX) {
        throw AxolotlDB::utils::StorageException("VectorHeap::Ref: id " + std::to_string(id) +
                                                 " exceeds the 32-bit index a VectorRef can hold");
    }
    return VectorRef{heap_id_, static_cast<uint32_t>(id)};
}

std::vector<float> VectorHeap::Get(VectorRef ref) const {
    if (ref.heap_id != heap_id_) {
        throw AxolotlDB::utils::ForeignVectorRefException(ref.heap_id, heap_id_);
    }
    return Get(static_cast<VectorId>(ref.index));
}

std::size_t VectorHeap::Count() const noexcept {
    return vector_count_;
}

uint32_t VectorHeap::Dimension() const noexcept {
    return dimension_;
}

HeapId VectorHeap::Id() const noexcept {
    return heap_id_;
}

void VectorHeap::Sync() {
    if (vector_count_ != durable_count_) {
        Page* header_page = FetchPage(VECTOR_HEAP_HEADER_PAGE);
        const VectorHeapHeader header{dimension_, heap_id_, vector_count_};
        std::memcpy(header_page->RawData().data(), &header, sizeof(header));
        header_page->MarkDirty();
        UnpinPage(VECTOR_HEAP_HEADER_PAGE, false);
    }
    FlushAllPages();
    durable_count_ = vector_count_;
}

void VectorHeap::Rollback() noexcept {
    vector_count_ = durable_count_;
}

std::size_t VectorHeap::PendingCount() const noexcept {
    return vector_count_ - durable_count_;
}

}  // namespace AxolotlDB::storage
