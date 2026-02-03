#include <storage/buffer_pool.h>

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>

namespace AxolotlDB::storage {

BufferPool::BufferPool(std::size_t pool_size) {
    if (pool_size == 0) {
        throw std::invalid_argument("Buffer pool size must be greater than 0");
    }
    frames_.resize(pool_size);
    for (std::size_t i = 0; i < pool_size; i++) {
        lru_list_.push_back(i);
        lru_map_[i] = std::prev(lru_list_.end());
    }
}

BufferPool::PinResult BufferPool::Pin(PageRef ref) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(ref.Key());
    if (it != page_table_.end()) {
        std::size_t frame_id = it->second;
        FrameDescriptor& frame = frames_[frame_id];
        frame.pin_count++;
        TouchFrame(frame_id);
        return {frame_id, &frame.page, true, {}, false};
    }

    std::size_t frame_id = SelectVictim();
    if (frame_id == SIZE_MAX) {
        throw AxolotlDB::utils::BufferPoolExhaustedException();
    }

    FrameDescriptor& frame = frames_[frame_id];
    PinResult result{frame_id, &frame.page, false, frame.ref, frame.page.IsDirty()};

    if (frame.ref.file_id != INVALID_FILE_ID) {
        page_table_.erase(frame.ref.Key());
    }

    frame.ref = ref;
    frame.pin_count = 1;
    frame.page.ClearDirty();
    page_table_[ref.Key()] = frame_id;
    TouchFrame(frame_id);

    return result;
}

void BufferPool::Unpin(PageRef ref, bool is_dirty) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_table_.find(ref.Key());
    if (it == page_table_.end()) {
        throw AxolotlDB::utils::InvalidPageException("Unpin: Page not found in buffer pool");
    }

    std::size_t frame_id = it->second;
    FrameDescriptor& frame = frames_[frame_id];

    if (is_dirty) {
        frame.page.MarkDirty();
    }

    if (frame.pin_count <= 0) {
        throw AxolotlDB::utils::InvalidPageException("Unpin: page (file=" + std::to_string(ref.file_id) + ", page=" + std::to_string(ref.page_no) + ") is not pinned");
    }

    frame.pin_count--;
    if (frame.pin_count == 0) {
        TouchFrame(frame_id);
    }
}

void BufferPool::UnpinAndDrop(PageRef ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(ref.Key());
    if (it != page_table_.end()) {
        std::size_t idx = it->second;
        page_table_.erase(it);
        frames_[idx].ref = {INVALID_FILE_ID, INVALID_PAGE_ID};
        frames_[idx].pin_count = 0;
        frames_[idx].page.ClearDirty();
        TouchFrame(idx);
    }
}

void BufferPool::InvalidateFile(FileId file_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& frame : frames_) {
        if (frame.ref.file_id == file_id) {
            page_table_.erase(frame.ref.Key());
            frame.ref = {INVALID_FILE_ID, INVALID_PAGE_ID};
            frame.pin_count = 0;
            frame.page.ClearDirty();
        }
    }
}

std::size_t BufferPool::OccupiedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return page_table_.size();
}

std::size_t BufferPool::Capacity() const noexcept {
    return frames_.size();
}

void BufferPool::TouchFrame(std::size_t frame_id) {
    auto it = lru_map_.find(frame_id);
    if (it != lru_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    }
}

std::size_t BufferPool::SelectVictim() const {
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); it++) {
        std::size_t frame_id = *it;
        if (frames_[frame_id].pin_count == 0) {
            return frame_id;
        }
    }
    return SIZE_MAX;
}

}  // namespace AxolotlDB::storage
