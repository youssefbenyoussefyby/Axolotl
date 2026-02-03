#include <storage/page_guard.h>
#include <storage/storage_manager.h>

#include <utility>

namespace AxolotlDB::storage {

ReadPageGuard::ReadPageGuard(StorageManager* sm, PageRef ref, const Page* page) noexcept
    : sm_(sm), ref_(ref), page_(page) {}

ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept
    : sm_(other.sm_), ref_(other.ref_), page_(other.page_) {
    other.sm_ = nullptr;
    other.ref_ = {};
    other.page_ = nullptr;
}

ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) noexcept {
    if (this != &other) {
        Drop();
        sm_ = other.sm_;
        ref_ = other.ref_;
        page_ = other.page_;
        other.sm_ = nullptr;
        other.ref_ = {};
        other.page_ = nullptr;
    }
    return *this;
}

ReadPageGuard::~ReadPageGuard() {
    Drop();
}

const Page& ReadPageGuard::operator*() const noexcept {
    return *page_;
}

const Page* ReadPageGuard::operator->() const noexcept {
    return page_;
}

void ReadPageGuard::Drop() noexcept {
    if (sm_ && page_) {
        sm_->Unpin(ref_, false);
    }
    sm_ = nullptr;
    ref_ = {};
    page_ = nullptr;
}

bool ReadPageGuard::IsValid() const noexcept {
    return page_ != nullptr;
}

PageRef ReadPageGuard::Ref() const noexcept {
    return ref_;
}

WritePageGuard::WritePageGuard(StorageManager* sm, PageRef ref, Page* page, bool is_dirty) noexcept
    : sm_(sm), ref_(ref), page_(page), dirty_(is_dirty) {}

WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : sm_(other.sm_), ref_(other.ref_), page_(other.page_), dirty_(other.dirty_) {
    other.sm_ = nullptr;
    other.ref_ = {};
    other.page_ = nullptr;
    other.dirty_ = false;
}

WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) noexcept {
    if (this != &other) {
        Drop();
        sm_ = other.sm_;
        ref_ = other.ref_;
        page_ = other.page_;
        dirty_ = other.dirty_;
        other.sm_ = nullptr;
        other.ref_ = {};
        other.page_ = nullptr;
        other.dirty_ = false;
    }
    return *this;
}

WritePageGuard::~WritePageGuard() {
    Drop();
}

Page& WritePageGuard::operator*() noexcept {
    dirty_ = true;
    if (page_) {
        page_->MarkDirty();
    }
    return *page_;
}

Page* WritePageGuard::operator->() noexcept {
    dirty_ = true;
    if (page_) {
        page_->MarkDirty();
    }
    return page_;
}

const Page& WritePageGuard::operator*() const noexcept {
    return *page_;
}

const Page* WritePageGuard::operator->() const noexcept {
    return page_;
}

void WritePageGuard::Drop() noexcept {
    if (sm_ && page_) {
        sm_->Unpin(ref_, dirty_ || page_->IsDirty());
    }
    sm_ = nullptr;
    ref_ = {};
    page_ = nullptr;
    dirty_ = false;
}

bool WritePageGuard::IsValid() const noexcept {
    return page_ != nullptr;
}

PageRef WritePageGuard::Ref() const noexcept {
    return ref_;
}

}  // namespace AxolotlDB::storage
