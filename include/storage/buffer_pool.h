#pragma once

#include <storage/page.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AxolotlDB::storage {

inline constexpr std::size_t DEFAULT_POOL_SIZE = 128;

struct FrameDescriptor {
    Page page;
    PageRef ref = {INVALID_FILE_ID, INVALID_PAGE_ID};
    int pin_count = 0;
};

class BufferPool {
   public:
    /// @param pool_size Number of frames (pages) held in RAM.
    /// @throws std::invalid_argument if pool_size == 0.
    explicit BufferPool(std::size_t pool_size = DEFAULT_POOL_SIZE);

    ~BufferPool() = default;
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    struct PinResult {
        std::size_t frame_idx;
        Page* page;
        bool was_in_pool;
        PageRef evicted_ref;
        bool evicted_dirty;
    };

    /// @throws BufferPoolExhaustedException if every frame is pinned.
    [[nodiscard]] PinResult Pin(PageRef ref);

    /// @throws InvalidPageException if ref is not currently pinned.
    void Unpin(PageRef ref, bool is_dirty);

    void UnpinAndDrop(PageRef ref);

    template <typename Fn>
    void ForEachDirty(Fn&& fn) {
        std::lock_guard lock(mutex_);
        for (auto& f : frames_) {
            if (f.ref.file_id != INVALID_FILE_ID && f.page.IsDirty()) {
                fn(f.ref, f.page);
            }
        }
    }

    void InvalidateFile(FileId file_id);

    [[nodiscard]] std::size_t OccupiedFrames() const;
    [[nodiscard]] std::size_t Capacity() const noexcept;

   private:
    [[nodiscard]] std::size_t SelectVictim() const;
    void TouchFrame(std::size_t frame_idx);

    std::vector<FrameDescriptor> frames_;
    std::unordered_map<uint64_t, std::size_t> page_table_;
    std::list<std::size_t> lru_list_;
    std::unordered_map<std::size_t, std::list<std::size_t>::iterator> lru_map_;
    mutable std::mutex mutex_;
};

}  // namespace AxolotlDB::storage