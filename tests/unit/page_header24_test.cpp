// P2 §2.3 — Page header grows 16 → 24 bytes: page_lsn (u64 @8), checksum
// (u32 @16), _pad2 (u32 @20). CRC32 protects the page's bytes; ValidateOrThrow
// verifies it after the structural invariants.
//
// ─── ASSUMED additions to include/storage/page.h + src/storage/page.cpp ─────
//
//   struct PageHeader {
//       PageId   page_id;      // @0
//       uint16_t slot_count;   // @4
//       uint16_t free_ptr;     // @6
//       uint64_t page_lsn;     // @8
//       uint32_t checksum;     // @16
//       uint32_t _pad2;        // @20
//   };                          // sizeof == 24
//
//   class Page {
//     void     UpdateChecksum() noexcept;          // recompute + store
//     uint32_t GetChecksum()  const noexcept;
//     uint64_t GetPageLSN()   const noexcept;
//     void     SetPageLSN(uint64_t) noexcept;
//     // ValidateOrThrow() now also verifies the stored checksum, AFTER the
//     // structural (header/slot) invariants.
//   };
//
// Note on the covered span: the ChecksumExcludesItselfAndPad2 test asserts the
// *behaviour* (the checksum value must not depend on the checksum field or on
// _pad2), not a literal offset, so it holds whichever contiguous payload span
// you choose. The roadmap's excluded-fields requirement makes [24, 4096) the
// consistent choice.

#include <gtest/gtest.h>
#include <storage/page.h>
#include <utils/errors.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>

using namespace AxolotlDB::storage;
namespace utils = AxolotlDB::utils;

// ─── Layout: compile-time where possible ─────────────────────────────────────
static_assert(sizeof(PageHeader) == 24, "PageHeader must grow to exactly 24 bytes");
static_assert(offsetof(PageHeader, page_lsn) == 8, "page_lsn claims the old _pad[8] at offset 8");
static_assert(offsetof(PageHeader, checksum) == 16, "checksum at offset 16");
static_assert(offsetof(PageHeader, _pad2) == 20, "_pad2 at offset 20");

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace

TEST(PageTest, HeaderIsTwentyFourBytes) {
    EXPECT_EQ(sizeof(PageHeader), 24u);
    EXPECT_EQ(offsetof(PageHeader, page_lsn), 8u);
    EXPECT_EQ(offsetof(PageHeader, checksum), 16u);
}

TEST(PageTest, SlotArrayStartsAtTwentyFour) {
    // The slot directory grows forward from the end of the header, so its first
    // entry sits at offset 24 and MAX_SLOTS shrinks by two entries' worth.
    EXPECT_EQ(MAX_SLOTS, static_cast<uint16_t>((PAGE_SIZE - 24) / sizeof(SlotEntry)));

    Page p;
    const std::string payload = "slot0";
    const SlotIdx s = p.InsertTuple(AsBytes(payload));
    ASSERT_EQ(s, 0);

    // The slot entry for slot 0 lives at [24, 28); its length field must equal
    // the payload size.
    const auto& raw = p.RawData();
    uint16_t stored_len = 0;
    std::memcpy(&stored_len, raw.data() + 24 + sizeof(uint16_t), sizeof(stored_len));
    EXPECT_EQ(stored_len, payload.size());
}

TEST(PageTest, ChecksumExcludesItselfAndPad2) {
    Page p;
    p.InsertTuple(AsBytes("payload-under-checksum"));
    p.UpdateChecksum();
    const uint32_t c0 = p.GetChecksum();

    // Scribble over the checksum field and _pad2 (bytes [16, 24)) and recompute.
    // The value must be unchanged: the checksum covers neither of them.
    auto& raw = p.RawData();
    std::memset(raw.data() + 16, 0xAB, 8);
    p.UpdateChecksum();
    EXPECT_EQ(p.GetChecksum(), c0) << "the checksum must not depend on itself or on _pad2";
}

TEST(PageCorruption, FlippedByteInPayloadFailsChecksum) {
    Page p;
    p.InsertTuple(AsBytes("integrity"));
    p.UpdateChecksum();

    // Flip one payload byte without updating the checksum, as a bad disk would.
    auto& raw = p.RawData();
    raw[PAGE_SIZE - 1] ^= std::byte{0xFF};

    EXPECT_THROW(p.ValidateOrThrow(), utils::CorruptPageException)
        << "a flipped payload byte must be caught by the stored checksum";
}

TEST(PageCorruption, StructuralViolationReportedBeforeChecksumFailure) {
    // A page that is BOTH structurally impossible and checksum-wrong must be
    // diagnosed as structural — that is the real defect; a checksum complaint
    // would mislead. ValidateOrThrow checks structure first.
    Page p;
    p.InsertTuple(AsBytes("both-wrong"));
    p.UpdateChecksum();

    auto& raw = p.RawData();
    const uint16_t insane_slot_count = 60000;  // structural: > MAX_SLOTS
    std::memcpy(raw.data() + 4, &insane_slot_count, sizeof(insane_slot_count));
    raw[PAGE_SIZE - 1] ^= std::byte{0xFF};  // also checksum-wrong

    try {
        p.ValidateOrThrow();
        FAIL() << "expected CorruptPageException";
    } catch (const utils::CorruptPageException& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.find("checksum"), std::string::npos)
            << "structural violation must be reported before the checksum: " << what;
    }
}
