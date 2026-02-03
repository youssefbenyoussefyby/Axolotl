#pragma once

#include <storage/page.h>
#include <utils/errors.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace AxolotlDB::storage {

class FileManager {
   public:
    /// @param path       Path to the database file on disk.
    /// @param page_size  Must match PAGE_SIZE (default 4096).
    explicit FileManager(const std::string& path, uint32_t page_size = PAGE_SIZE);

    ~FileManager();

    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&&) = delete;
    FileManager& operator=(FileManager&&) = delete;
    /// @returns The new page's ID.
    [[nodiscard]] PageId AllocatePage();

    /// @throws IOException on I/O error.
    /// @throws InvalidPageException if page_id is out of range.
    void ReadPage(PageId page_id, Page& out);

    /// @throws IOException on I/O error.
    /// @throws InvalidPageException if page_id is out of range.
    void WritePage(PageId page_id, const Page& page);

    [[nodiscard]] PageId PageCount() const noexcept;

    [[nodiscard]] const std::string& FilePath() const noexcept;

    void Sync();

   private:
    std::string path_;
    uint32_t page_size_;
    std::fstream file_;
    std::atomic<int32_t> page_count_{0};
    mutable std::mutex mutex_;
    int sync_fd_{-1};
    bool sync_failed_{false};
    void EnsureValidPageId(PageId id) const;
};

}  // namespace AxolotlDB::storage
