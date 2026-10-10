#pragma once

#include <storage/file_io.h>
#include <storage/page.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace AxolotlDB::storage {

using FileId = uint32_t;
inline constexpr FileId INVALID_FILE_ID = 0;

struct PageRef {
    FileId file_id = INVALID_FILE_ID;
    PageId page_no = INVALID_PAGE_ID;

    [[nodiscard]] constexpr uint64_t Key() const noexcept {
        return (static_cast<uint64_t>(file_id) << 32) | static_cast<uint32_t>(page_no);
    }

    [[nodiscard]] static PageRef FromKey(uint64_t key) noexcept {
        return {static_cast<FileId>(key >> 32), static_cast<PageId>(static_cast<uint32_t>(key))};
    }

    friend bool operator==(const PageRef&, const PageRef&) noexcept = default;
};

class BufferPool;
class ReadPageGuard;
class WritePageGuard;
class ILogManager;

class StorageManager {
   public:
    static constexpr std::size_t DEFAULT_POOL_SIZE = 128;

    /// @param pool_size Frames shared across every file registered with
    ///                  this manager -- not per file.
    /// @throws std::invalid_argument if pool_size == 0.
    explicit StorageManager(std::size_t pool_size = DEFAULT_POOL_SIZE, ILogManager* log = nullptr);

    /// Constructor supporting custom FileIO injection for testing.
    explicit StorageManager(std::size_t pool_size, FileIOFactory factory,
                            ILogManager* log = nullptr);

    ~StorageManager();

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;
    StorageManager(StorageManager&&) = delete;
    StorageManager& operator=(StorageManager&&) = delete;

    /// @returns A FileId stable for the lifetime of this process.
    /// @throws AxolotlDB::utils::IOException if the file cannot be created or opened.
    [[nodiscard]] FileId OpenFile(const std::string& path);

    /// @throws AxolotlDB::utils::InvalidPageException if `id` is not open.
    void CloseFile(FileId id);

    /// @throws AxolotlDB::utils::InvalidPageException if `ref.file_id` is
    ///         not open, or `ref.page_no` is out of range for that file.
    [[nodiscard]] ReadPageGuard FetchRead(PageRef ref);

    /// @throws AxolotlDB::utils::InvalidPageException if `ref.file_id` is
    ///         not open, or `ref.page_no` is out of range for that file.
    [[nodiscard]] WritePageGuard FetchWrite(PageRef ref);

    /// @param file The file to allocate into. Must already be open.
    /// @param[out] out The PageRef of the newly allocated page.
    /// @throws AxolotlDB::utils::InvalidPageException if `file` is not open.
    /// @throws AxolotlDB::utils::BufferPoolExhaustedException if no frame is available.
    [[nodiscard]] WritePageGuard NewPage(FileId file, PageRef& out);

    /// @throws AxolotlDB::utils::InvalidPageException if `id` is not open.
    void FlushFile(FileId id);

    void FlushAll();

    /// @throws AxolotlDB::utils::InvalidPageException if `id` is not open.
    [[nodiscard]] PageId PageCount(FileId id) const;

    [[nodiscard]] std::size_t PoolCapacity() const noexcept;
    [[nodiscard]] std::size_t OpenFileCount() const noexcept;

    void Unpin(PageRef ref, bool is_dirty);

   private:
    std::unique_ptr<BufferPool> pool_;
    FileIOFactory factory_;
    std::unordered_map<FileId, std::unique_ptr<IFileIO>> files_;
    std::unordered_map<std::string, FileId> path_to_id_;
    FileId next_file_id_ = INVALID_FILE_ID + 1;
    ILogManager* log_manager_;

    /// @throws AxolotlDB::utils::InvalidPageException naming `id`.
    IFileIO& RequireFile(FileId id) const;
};

}  // namespace AxolotlDB::storage
