#include <storage/buffer_pool.h>
#include <storage/file_manager.h>
#include <storage/log_manager.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace AxolotlDB::storage {

namespace {

struct FileManagerIO final : IFileIO {
    explicit FileManagerIO(const std::string& path) : fm_(path) {}
    PageId AllocatePage() override {
        return fm_.AllocatePage();
    }
    void ReadPage(PageId id, Page& out) override {
        fm_.ReadPage(id, out);
    }
    void WritePage(PageId id, const Page& p) override {
        fm_.WritePage(id, p);
    }
    PageId PageCount() const override {
        return fm_.PageCount();
    }
    void Sync() override {
        fm_.Sync();
    }

   private:
    FileManager fm_;
};

}  // namespace

StorageManager::StorageManager(std::size_t pool_size, ILogManager* log)
    : StorageManager(
          pool_size,
          [](const std::string& path) -> std::unique_ptr<IFileIO> {
              return std::make_unique<FileManagerIO>(path);
          },
          log) {}

StorageManager::StorageManager(std::size_t pool_size, FileIOFactory factory, ILogManager* log)
    : factory_(std::move(factory)) {
    if (pool_size == 0) {
        throw std::invalid_argument("Pool size must not be zero.");
    }

    pool_ = std::make_unique<BufferPool>(pool_size);
    log_manager_ = log;
}

StorageManager::~StorageManager() = default;

FileId StorageManager::OpenFile(const std::string& path) {
    auto it = path_to_id_.find(path);
    if (it != path_to_id_.end()) {
        return it->second;
    }

    auto io = factory_(path);
    if (!io) {
        throw AxolotlDB::utils::IOException("Failed to open file: " + path);
    }

    FileId id = next_file_id_++;
    files_[id] = std::move(io);
    path_to_id_[path] = id;
    return id;
}

void StorageManager::CloseFile(FileId id) {
    RequireFile(id);
    FlushFile(id);
    pool_->InvalidateFile(id);

    for (auto it = path_to_id_.begin(); it != path_to_id_.end();) {
        if (it->second == id) {
            it = path_to_id_.erase(it);
        } else {
            ++it;
        }
    }
    files_.erase(id);
}

ReadPageGuard StorageManager::FetchRead(PageRef ref) {
    IFileIO& io = RequireFile(ref.file_id);
    if (ref.page_no < 0 || ref.page_no >= io.PageCount()) {
        throw AxolotlDB::utils::InvalidPageException("FetchRead: page_no " +
                                                     std::to_string(ref.page_no) + " out of range");
    }

    auto pin_res = pool_->Pin(ref);

    if (pin_res.evicted_dirty && pin_res.evicted_ref.file_id != INVALID_FILE_ID) {
        if (log_manager_) {
            log_manager_->FlushUpTo(pin_res.page->GetPageLSN());
        }
        RequireFile(pin_res.evicted_ref.file_id)
            .WritePage(pin_res.evicted_ref.page_no, *pin_res.page);
    }

    if (!pin_res.was_in_pool) {
        try {
            io.ReadPage(ref.page_no, *pin_res.page);
            pin_res.page->ClearDirty();
        } catch (...) {
            pool_->UnpinAndDrop(ref);
            throw;
        }
    }

    return ReadPageGuard(this, ref, pin_res.page);
}

WritePageGuard StorageManager::FetchWrite(PageRef ref) {
    IFileIO& io = RequireFile(ref.file_id);
    if (ref.page_no < 0 || ref.page_no >= io.PageCount()) {
        throw AxolotlDB::utils::InvalidPageException("FetchWrite: page_no " +
                                                     std::to_string(ref.page_no) + " out of range");
    }

    auto pin_res = pool_->Pin(ref);

    if (pin_res.evicted_dirty && pin_res.evicted_ref.file_id != INVALID_FILE_ID) {
        if (log_manager_) {
            log_manager_->FlushUpTo(pin_res.page->GetPageLSN());
        }
        RequireFile(pin_res.evicted_ref.file_id)
            .WritePage(pin_res.evicted_ref.page_no, *pin_res.page);
    }

    if (!pin_res.was_in_pool) {
        try {
            io.ReadPage(ref.page_no, *pin_res.page);
            pin_res.page->ClearDirty();
        } catch (...) {
            pool_->UnpinAndDrop(ref);
            throw;
        }
    }

    return WritePageGuard(this, ref, pin_res.page);
}

WritePageGuard StorageManager::NewPage(FileId file, PageRef& out) {
    IFileIO& io = RequireFile(file);
    PageId new_page_id = io.AllocatePage();
    out = PageRef{file, new_page_id};

    auto pin_res = pool_->Pin(out);

    if (pin_res.evicted_dirty && pin_res.evicted_ref.file_id != INVALID_FILE_ID) {
        if (log_manager_) {
            log_manager_->FlushUpTo(pin_res.page->GetPageLSN());
        }
        RequireFile(pin_res.evicted_ref.file_id)
            .WritePage(pin_res.evicted_ref.page_no, *pin_res.page);
    }

    *pin_res.page = Page();
    pin_res.page->SetPageId(new_page_id);
    pin_res.page->MarkDirty();

    return WritePageGuard(this, out, pin_res.page, true);
}

void StorageManager::FlushFile(FileId id) {
    IFileIO& io = RequireFile(id);
    pool_->ForEachDirty([&](PageRef ref, Page& page) {
        if (ref.file_id == id) {
            if (log_manager_) {
                log_manager_->FlushUpTo(page.GetPageLSN());
            }
            io.WritePage(ref.page_no, page);
            page.ClearDirty();
        }
    });
    io.Sync();
}

void StorageManager::FlushAll() {
    pool_->ForEachDirty([&](PageRef ref, Page& page) {
        if (log_manager_) {
            log_manager_->FlushUpTo(page.GetPageLSN());
        }
        RequireFile(ref.file_id).WritePage(ref.page_no, page);
        page.ClearDirty();
    });
    for (auto& [id, io] : files_) {
        io->Sync();
    }
}

PageId StorageManager::PageCount(FileId id) const {
    return RequireFile(id).PageCount();
}

std::size_t StorageManager::PoolCapacity() const noexcept {
    return pool_ ? pool_->Capacity() : 0;
}

std::size_t StorageManager::OpenFileCount() const noexcept {
    return files_.size();
}

void StorageManager::Unpin(PageRef ref, bool is_dirty) {
    if (pool_) {
        pool_->Unpin(ref, is_dirty);
    }
}

IFileIO& StorageManager::RequireFile(FileId id) const {
    auto it = files_.find(id);
    if (it == files_.end()) {
        throw AxolotlDB::utils::InvalidPageException("RequireFile: file_id " + std::to_string(id) +
                                                     " is not open");
    }
    return *it->second;
}

}  // namespace AxolotlDB::storage
