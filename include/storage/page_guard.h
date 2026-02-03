#pragma once

#include <storage/page.h>
#include <storage/storage_manager.h>

namespace AxolotlDB::storage {

class StorageManager;

class ReadPageGuard {
   public:
    ReadPageGuard() noexcept = default;
    ReadPageGuard(StorageManager* sm, PageRef ref, const Page* page) noexcept;

    ReadPageGuard(ReadPageGuard&& other) noexcept;
    ReadPageGuard& operator=(ReadPageGuard&& other) noexcept;

    ReadPageGuard(const ReadPageGuard&) = delete;
    ReadPageGuard& operator=(const ReadPageGuard&) = delete;

    ~ReadPageGuard();

    [[nodiscard]] const Page& operator*() const noexcept;
    [[nodiscard]] const Page* operator->() const noexcept;

    void Drop() noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] PageRef Ref() const noexcept;

   private:
    StorageManager* sm_ = nullptr;
    PageRef ref_ = {};
    const Page* page_ = nullptr;
};

class WritePageGuard {
   public:
    WritePageGuard() noexcept = default;
    WritePageGuard(StorageManager* sm, PageRef ref, Page* page, bool is_dirty = false) noexcept;

    WritePageGuard(WritePageGuard&& other) noexcept;
    WritePageGuard& operator=(WritePageGuard&& other) noexcept;

    WritePageGuard(const WritePageGuard&) = delete;
    WritePageGuard& operator=(const WritePageGuard&) = delete;

    ~WritePageGuard();

    [[nodiscard]] Page& operator*() noexcept;
    [[nodiscard]] Page* operator->() noexcept;

    [[nodiscard]] const Page& operator*() const noexcept;
    [[nodiscard]] const Page* operator->() const noexcept;

    void Drop() noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] PageRef Ref() const noexcept;

   private:
    StorageManager* sm_ = nullptr;
    PageRef ref_ = {};
    Page* page_ = nullptr;
    bool dirty_ = false;
};

}  // namespace AxolotlDB::storage
