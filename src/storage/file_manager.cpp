#include <storage/file_manager.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <ios>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <system_error>
namespace AxolotlDB::storage {

namespace {

std::string ErrnoMessage(int err) {
    return std::error_code(err, std::generic_category()).message();
}

}  // namespace

FileManager::FileManager(const std::string& path, uint32_t page_size)
    : path_(path), page_size_(page_size) {
    if (page_size != PAGE_SIZE)
        throw std::invalid_argument("FileManager: page_size must be exactly " +std::to_string(PAGE_SIZE) + ", got " +std::to_string(page_size));

    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        file_.open(path_, std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            throw AxolotlDB::utils::IOException("Cannot create database file: " + path_);
        }
        file_.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            throw AxolotlDB::utils::IOException("Cannot open database file after creation: " +path_);
        }
    }
    file_.seekg(0, std::ios::end);
    const auto file_size = file_.tellg();
    if (file_size < 0) {
        throw AxolotlDB::utils::IOException("Failed to seek to end of file: " + path_);
    }

#ifdef _WIN32
    if (::_sopen_s(&sync_fd_, path_.c_str(), _O_RDWR | _O_BINARY, _SH_DENYNO, 0) != 0) {
        sync_fd_ = -1;
    }
#else
    sync_fd_ = ::open(path_.c_str(), O_RDWR);
#endif
    if (sync_fd_ < 0) {
        throw AxolotlDB::utils::IOException("Cannot open sync descriptor for " + path_ + ": " +
                                           ErrnoMessage(errno));
    }
    page_count_.store(static_cast<int32_t>(file_size / page_size_), std::memory_order_relaxed);
}

FileManager::~FileManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
#ifdef _WIN32
    if (sync_fd_ >= 0)
        ::_close(sync_fd_);
#else
    if (sync_fd_ >= 0)
        ::close(sync_fd_);
#endif
}
PageId FileManager::PageCount() const noexcept {
    return page_count_.load(std::memory_order_relaxed);
}

const std::string& FileManager::FilePath() const noexcept {
    return path_;
}

void FileManager::EnsureValidPageId(PageId id) const {
    if (id < 0 || id >= page_count_.load(std::memory_order_relaxed)) {
        throw AxolotlDB::utils::InvalidPageException("Page ID " + std::to_string(id) + " is out of range (total pages: " +std::to_string(page_count_.load(std::memory_order_relaxed)) + ")");
    }
}

PageId FileManager::AllocatePage() {
    std::lock_guard<std::mutex> lock(mutex_);
    const PageId new_id = page_count_.load(std::memory_order_relaxed);

    const std::streampos offset = static_cast<std::streampos>(new_id) * page_size_;
    file_.seekp(offset, std::ios::beg);
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Seek failed during AllocatePage");
    }

    std::array<char, PAGE_SIZE> zeros{};
    file_.write(zeros.data(), page_size_);
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Write failed during AllocatePage");
    }

    page_count_.store(new_id + 1, std::memory_order_relaxed);

    return new_id;
}

void FileManager::ReadPage(PageId page_id, Page& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureValidPageId(page_id);

    const std::streampos offset = static_cast<std::streampos>(page_id) * page_size_;
    file_.seekg(offset, std::ios::beg);
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Seek failed reading page " + std::to_string(page_id));
    }

    file_.read(reinterpret_cast<char*>(out.RawData().data()), page_size_);
    if (file_.gcount() != page_size_) {
        throw AxolotlDB::utils::IOException("Read failed for page " + std::to_string(page_id));
    }

    out.ClearDirty();
}

void FileManager::WritePage(PageId page_id, const Page& page) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureValidPageId(page_id);

    const std::streampos offset = static_cast<std::streampos>(page_id) * page_size_;
    file_.seekp(offset, std::ios::beg);
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Seek failed writing page " + std::to_string(page_id));
    }

    file_.write(reinterpret_cast<const char*>(page.RawData().data()), page_size_);
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Write failed for page " + std::to_string(page_id));
    }
}

void FileManager::Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sync_failed_) {
        throw AxolotlDB::utils::IOException("FileManager is unusable: a previous fsync failed on " +path_);
    }
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Stream is in a failed state; cannot sync " + path_);
    }

    file_.flush();
    if (!file_.good()) {
        throw AxolotlDB::utils::IOException("Flush to OS failed before fsync on " + path_);
    }

#if defined(_WIN32)
    const int rc = ::_commit(sync_fd_);
#elif defined(__APPLE__)
    const int rc = ::fcntl(sync_fd_, F_FULLFSYNC, 0);
#else
    const int rc = ::fsync(sync_fd_);
#endif

    if (rc != 0) {
        sync_failed_ = true;
        throw AxolotlDB::utils::IOException("fsync failed on " + path_ + ": " + ErrnoMessage(errno));
    }
}

}  // namespace AxolotlDB::storage