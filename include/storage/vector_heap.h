#pragma once

#include <storage/buffer_pool.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/vector_ref.h>
#include <utils/errors.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AxolotlDB::storage {
using VectorId = uint64_t;
inline constexpr PageId VECTOR_HEAP_HEADER_PAGE = 0;
const uint32_t useable_page_size = PAGE_SIZE - sizeof(PageHeader);

inline constexpr uint32_t MAX_VECTOR_DIMENSION =
    static_cast<uint32_t>((static_cast<uint64_t>(UINT16_MAX) * useable_page_size) / sizeof(float));

struct VectorHeapHeader {
    uint32_t dimension = 0;
    HeapId heap_id = INVALID_HEAP_ID;
    uint64_t vector_count = 0;
};

static_assert(sizeof(VectorHeapHeader) == 16, "VectorHeapHeader is an on-disk format");

struct AddressingMath {
    PageId pid;
    uint32_t offsets;
    uint16_t page_span;
};

class VectorHeap {
   public:
    /// @param path       Path to the heap file on disk.
    /// @param heap_id     Catalog-assigned identity for this heap. Must not be
    ///                    INVALID_HEAP_ID. Stamped into every VectorRef the
    ///                    heap issues, and checked against the stored header
    ///                    when an existing file is reopened.
    /// @param dimension  Fixed vector width for every record in this heap.
    ///                   Must be > 0.
    /// @throws std::invalid_argument if dimension == 0, if heap_id is
    ///         INVALID_HEAP_ID, or if the file already exists and its stored
    ///         header's dimension or heap id does not match (same fail-fast
    ///         contract as FileManager's page_size check -- see A6 in
    ///         docs/audit.md).
    explicit VectorHeap(const std::string& path, HeapId heap_id, uint32_t dimension,
                        std::size_t pool_size = DEFAULT_POOL_SIZE);
    ~VectorHeap() = default;
    VectorHeap(const VectorHeap&) = delete;
    VectorHeap& operator=(const VectorHeap&) = delete;
    VectorHeap(VectorHeap&&) = delete;
    VectorHeap& operator=(VectorHeap&&) = delete;

    /// @returns The new record's VectorId.
    /// @throws  AxolotlDB::utils::StorageException if vector.size() != Dimension().
    [[nodiscard]] VectorId Put(std::span<const float> vector);
    /// @throws AxolotlDB::utils::InvalidVectorIdException if id >= Count().
    ///
    /// Takes a raw heap-local address, so it cannot check provenance. Rows
    /// store a VectorRef and should resolve through the overload below.
    [[nodiscard]] std::vector<float> Get(VectorId id) const;

    /// @throws AxolotlDB::utils::ForeignVectorRefException if the reference was
    ///         issued by a different heap.
    /// @throws AxolotlDB::utils::InvalidVectorIdException if the index is out
    ///         of range for this heap.
    [[nodiscard]] std::vector<float> Get(VectorRef ref) const;

    /// @throws AxolotlDB::utils::InvalidVectorIdException if id >= Count(), or
    ///         if the id is too large for a VectorRef's 32-bit index.
    [[nodiscard]] VectorRef Ref(VectorId id) const;

    [[nodiscard]] std::size_t Count() const noexcept;
    [[nodiscard]] uint32_t Dimension() const noexcept;
    [[nodiscard]] HeapId Id() const noexcept;

    void Sync();

    void Rollback() noexcept;
    [[nodiscard]] std::size_t PendingCount() const noexcept;

   private:
    mutable FileManager file_manager_;
    mutable BufferPool buffer_pool_;
    AddressingMath LocateVector(VectorId id) const;
    Page* AcquirePage(PageId page_id) const;
    Page* FetchPage(PageId page_id) const;
    Page* NewPage(PageId& out_page_id) const;
    void UnpinPage(PageId page_id, bool is_dirty) const;
    void FlushAllPages() const;

    uint32_t dimension_;
    HeapId heap_id_;
    std::size_t vector_count_;
    std::size_t durable_count_;
};

}  // namespace AxolotlDB::storage
