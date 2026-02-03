#include <storage/page.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
uint32_t ComputeChecksumCRC32(std::span<const std::byte> bytes) noexcept {
    static constexpr uint32_t crc32_poly = 0xEDB88320U;
    uint32_t crc = 0xFFFFFFFFU;
    static const std::array<uint32_t, 256> crc32_lookup = []() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c >> 1) ^ ((c & 1) ? crc32_poly : 0);
            }
            table[i] = c;
        }
        return table;
    }();
    for (const std::byte b : bytes) {
        crc = (crc >> 8) ^ crc32_lookup[(crc ^ std::to_integer<uint8_t>(b)) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFU;
}
}  // namespace

namespace AxolotlDB::storage {

PageHeader& Page::Header() noexcept {
    return *reinterpret_cast<PageHeader*>(data_.data());
}

const PageHeader& Page::Header() const noexcept {
    return *reinterpret_cast<const PageHeader*>(data_.data());
}

SlotEntry* Page::Slots() noexcept {
    return reinterpret_cast<SlotEntry*>(data_.data() + sizeof(PageHeader));
}

const SlotEntry* Page::Slots() const noexcept {
    return reinterpret_cast<const SlotEntry*>(data_.data() + sizeof(PageHeader));
}

uint32_t Page::SlotArrayEnd() const noexcept {
    return static_cast<uint32_t>(sizeof(PageHeader)) +
           static_cast<uint32_t>(Header().slot_count) * sizeof(SlotEntry);
}

Page::Page() {
    data_.fill(std::byte{0});
    auto& hdr = Header();
    hdr.page_id = INVALID_PAGE_ID;
    hdr.slot_count = 0;
    hdr.free_ptr = PAGE_SIZE;
}

Page::Page(const std::array<std::byte, PAGE_SIZE>& raw) : data_(raw) {
    ValidateOrThrow();
}

PageId Page::GetPageId() const noexcept {
    return Header().page_id;
}

void Page::SetPageId(PageId id) noexcept {
    Header().page_id = id;
}

uint32_t Page::FreeSpace() const noexcept {
    const auto& hdr = Header();
    const uint32_t slot_end = SlotArrayEnd();
    if (hdr.free_ptr <= slot_end)
        return 0;
    const uint32_t available = hdr.free_ptr - slot_end;
    const uint32_t slot_overhead = sizeof(SlotEntry);
    if (available <= slot_overhead)
        return 0;
    return available - slot_overhead;
}

uint16_t Page::SlotCount() const noexcept {
    return Header().slot_count;
}

void Page::EnsureHeaderSane() const {
    const auto& hdr = Header();

    if (hdr.slot_count > MAX_SLOTS) {
        throw AxolotlDB::utils::CorruptPageException(
            hdr.page_id, "slot_count " + std::to_string(hdr.slot_count) + " exceeds the " +
                             std::to_string(MAX_SLOTS) + " slots a page can hold");
    }

    if (hdr.free_ptr > PAGE_SIZE) {
        throw AxolotlDB::utils::CorruptPageException(
            hdr.page_id,
            "free_ptr " + std::to_string(hdr.free_ptr) + " is past the end of the page");
    }

    if (hdr.free_ptr < SlotArrayEnd()) {
        throw AxolotlDB::utils::CorruptPageException(hdr.page_id,
                                                    "free_ptr " + std::to_string(hdr.free_ptr) +
                                                        " is below the end of the slot array (" +
                                                        std::to_string(SlotArrayEnd()) + ")");
    }
}

void Page::EnsureSlotInBounds(SlotIdx slot, const SlotEntry& entry) const {
    const auto& hdr = Header();
    const uint32_t end = static_cast<uint32_t>(entry.offset) + entry.length;
    if (entry.offset < hdr.free_ptr || end > PAGE_SIZE) {
        throw AxolotlDB::utils::CorruptPageException(
            hdr.page_id, "slot " + std::to_string(slot) + " payload [" +
                             std::to_string(entry.offset) + ", " + std::to_string(end) +
                             ") escapes the payload region [" + std::to_string(hdr.free_ptr) +
                             ", " + std::to_string(PAGE_SIZE) + ")");
    }
}

void Page::ValidateOrThrow() const {
    EnsureHeaderSane();
    const auto& hdr = Header();
    for (SlotIdx i = 0; i < hdr.slot_count; ++i) {
        const SlotEntry& entry = Slots()[i];
        if (entry.length == 0)
            continue;
        EnsureSlotInBounds(i, entry);
    }
    if (hdr.checksum != 0 && !IsValidChecksum()) {
        throw AxolotlDB::utils::CorruptPageException(
            hdr.page_id, "checksum mismatch: stored " + std::to_string(hdr.checksum) +
                             ", computed " + std::to_string(ComputeChecksum()));
    }
}
SlotIdx Page::InsertTuple(std::span<const std::byte> tuple_data) {
    EnsureHeaderSane();
    const uint32_t len = static_cast<uint32_t>(tuple_data.size());
    if (len == 0) {
        throw AxolotlDB::utils::StorageException("Cannot insert a zero-length tuple");
    }
    if (FreeSpace() < len) {
        throw AxolotlDB::utils::PageFullException{};
    }

    auto& hdr = Header();

    hdr.free_ptr -= static_cast<uint16_t>(len);
    std::memcpy(data_.data() + hdr.free_ptr, tuple_data.data(), len);
    const SlotIdx slot = hdr.slot_count;
    Slots()[slot] = SlotEntry{hdr.free_ptr, static_cast<uint16_t>(len)};
    ++hdr.slot_count;

    isDirty_ = 1;
    return slot;
}

std::span<const std::byte> Page::GetTuple(SlotIdx slot) const {
    EnsureHeaderSane();
    const auto& hdr = Header();
    if (slot >= hdr.slot_count) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    const SlotEntry& entry = Slots()[slot];
    if (entry.length == 0) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    EnsureSlotInBounds(slot, entry);
    return std::span<const std::byte>(data_.data() + entry.offset, entry.length);
}

void Page::DeleteTuple(SlotIdx slot) {
    EnsureHeaderSane();
    auto& hdr = Header();
    if (slot >= hdr.slot_count) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    SlotEntry& entry = Slots()[slot];
    if (entry.length == 0) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    entry.length = 0;
    isDirty_ = 1;
}

void Page::UpdateTupleInPlace(SlotIdx slot, std::span<const std::byte> data) {
    EnsureHeaderSane();
    auto& hdr = Header();
    if (slot >= hdr.slot_count) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    SlotEntry& entry = Slots()[slot];
    if (entry.length == 0) {
        throw AxolotlDB::utils::InvalidSlotException{slot};
    }
    std::memcpy(data_.data() + entry.offset, data.data(), data.size());
    entry.length = static_cast<uint16_t>(data.size());
    isDirty_ = 1;
}

bool Page::IsDirty() const noexcept {
    return isDirty_ != 0;
}
void Page::MarkDirty() noexcept {
    isDirty_ = 1;
}
void Page::ClearDirty() noexcept {
    isDirty_ = 0;
}

const std::array<std::byte, PAGE_SIZE>& Page::RawData() const noexcept {
    return data_;
}
std::array<std::byte, PAGE_SIZE>& Page::RawData() noexcept {
    return data_;
}

void Page::SetPageLSN(uint64_t lsn) noexcept {
    Header().page_lsn = lsn;
}
uint64_t Page::GetPageLSN() const noexcept {
    return Header().page_lsn;
}
void Page::SetChecksum(uint32_t checksum) noexcept {
    Header().checksum = checksum;
}
uint32_t Page::GetChecksum() const noexcept {
    return Header().checksum;
}
bool Page::IsValidChecksum() const noexcept {
    if (Header().checksum == 0) {
        return false;
    }

    return Header().checksum == ComputeChecksum();
}
void Page::UpdateChecksum() noexcept {
    Header().checksum = ComputeChecksum();
}

uint32_t Page::ComputeChecksum() const noexcept {
    return ComputeChecksumCRC32(std::span<const std::byte>(RawData().data() + 24, PAGE_SIZE - 24));
}

void Page::Compact() noexcept {
    EnsureHeaderSane();
    auto& hdr = Header();
    if (hdr.slot_count == 0) {
        hdr.free_ptr = PAGE_SIZE;
        MarkDirty();
        return;
    }

    const uint16_t old_free_ptr = hdr.free_ptr;
    std::array<std::byte, PAGE_SIZE> tmp_data = data_;
    uint16_t new_free_ptr = PAGE_SIZE;

    for (SlotIdx i = 0; i < hdr.slot_count; i++) {
        SlotEntry& entry = Slots()[i];
        if (entry.length == 0) {
            entry.offset = 0;
            continue;
        }

        new_free_ptr -= entry.length;
        std::memcpy(tmp_data.data() + new_free_ptr, data_.data() + entry.offset, entry.length);
        entry.offset = new_free_ptr;
    }

    if (new_free_ptr < PAGE_SIZE) {
        std::memcpy(data_.data() + new_free_ptr, tmp_data.data() + new_free_ptr,
                    PAGE_SIZE - new_free_ptr);
    }
    if (new_free_ptr > old_free_ptr) {
        std::memset(data_.data() + old_free_ptr, 0, new_free_ptr - old_free_ptr);
    }
    hdr.free_ptr = new_free_ptr;
    MarkDirty();
}

}  // namespace AxolotlDB::storage