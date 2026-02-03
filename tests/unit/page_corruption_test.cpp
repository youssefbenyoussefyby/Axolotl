// Regression tests for finding N1 -- Page never validated a header or slot
// entry that came off disk (docs/ROADMAP.md section 2).
//
// FileManager::ReadPage memcpys 4096 raw disk bytes straight into Page::data_,
// and Page::Header() reinterprets the first 16 of them as truth. Nothing
// checked them, which gave three separate out-of-bounds accesses:
//
//   1. GetTuple guarded only `slot >= slot_count`, and slot_count is disk
//      data. A corrupt 60000 indexed the slot array ~236 KB past the page.
//   2. The returned span was {data_ + entry.offset, entry.length} with neither
//      field validated -- and Tuple::Deserialize could not catch it, because
//      its own A2 bounds checks are all relative to the span it is handed.
//   3. free_ptr is a uint16_t. A corrupt 65535 made FreeSpace() report 65515,
//      so InsertTuple passed its guard and memcpy'd ~61 KB out of bounds -- a
//      write, not a read.
//
// This is the A2 bug class (trusting sizes that came off disk) applied to Page
// rather than Tuple. A2 was found and fixed thoroughly; the Page equivalent was
// never looked at, because that audit's scope was the serialisation layer.
//
// Where the checks live, and why not in ReadPage
// ----------------------------------------------
// The obvious home is FileManager::ReadPage -- validate once, on the way in.
// That does not work yet, for two reasons that are both features, not bugs:
//
//   * AllocatePage zero-fills. A freshly allocated page read back has
//     free_ptr == 0, which fails "free_ptr >= SlotArrayEnd()". Legitimate.
//   * VectorHeap data pages have no PageHeader at all -- records are memcpy'd
//     from byte 0 -- so their first 16 bytes are float payload. Every
//     VectorHeap::Get would throw.
//
// Telling the two apart needs a page-type tag, which is P2's work (see N9). So
// the invariants are enforced by the slotted-page accessors instead, in O(1),
// which is where the out-of-bounds access actually happened. Nothing can read
// or write out of bounds either way; ValidateOrThrow() is the explicit
// whole-page pass, for callers that would rather reject a bad page up front.

#include <gtest/gtest.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <utils/errors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

using namespace AxolotlDB::storage;

namespace {

// Overwrite a page's header in place. Page offers no way to reach these fields
// -- which is the point -- so the test writes the bytes the way a corrupt disk
// would, through RawData().
void StompHeader(Page& p, uint16_t slot_count, uint16_t free_ptr) {
    auto& raw = p.RawData();
    std::memcpy(raw.data() + 4, &slot_count, sizeof(slot_count));
    std::memcpy(raw.data() + 6, &free_ptr, sizeof(free_ptr));
}

void StompSlot(Page& p, uint16_t slot, uint16_t offset, uint16_t length) {
    auto& raw = p.RawData();
    std::byte* entry = raw.data() + sizeof(PageHeader) + slot * sizeof(SlotEntry);
    std::memcpy(entry, &offset, sizeof(offset));
    std::memcpy(entry + sizeof(offset), &length, sizeof(length));
}

std::span<const std::byte> AsBytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

}  // namespace

// --- invariant 1: slot_count ------------------------------------------------

TEST(PageCorruption, SlotCountBeyondPageCapacityThrows) {
    Page p;
    StompHeader(p, /*slot_count=*/60000, /*free_ptr=*/PAGE_SIZE);

    // Under the old code this indexed data_ + 16 + 59999*4 -- roughly 236 KB
    // past a 4096-byte array. ASan reports it as a heap-buffer-overflow.
    EXPECT_THROW((void)p.GetTuple(59999), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
}

TEST(PageCorruption, SlotCountExactlyAtCapacityIsAccepted) {
    // MAX_SLOTS slots leave the slot array ending exactly at PAGE_SIZE, which
    // is a legal (if useless) page. An off-by-one in the guard would reject it.
    Page p;
    StompHeader(p, /*slot_count=*/MAX_SLOTS, /*free_ptr=*/PAGE_SIZE);
    EXPECT_NO_THROW(p.ValidateOrThrow());
}

TEST(PageCorruption, SlotCountOneBeyondCapacityThrows) {
    Page p;
    StompHeader(p, static_cast<uint16_t>(MAX_SLOTS + 1), PAGE_SIZE);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
}

// --- invariant 2: free_ptr past the end of the page -------------------------

TEST(PageCorruption, FreePtrBeyondPageSizeThrows) {
    Page p;
    StompHeader(p, /*slot_count=*/0, /*free_ptr=*/65535);

    // This is the out-of-bounds WRITE. FreeSpace() reported 65515, so a
    // 100-byte insert passed the guard and memcpy'd at data_ + 65435.
    const std::string payload(100, 'x');
    EXPECT_THROW((void)p.InsertTuple(AsBytes(payload)), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
}

// --- invariant 3: slot array and payload region have crossed ----------------

TEST(PageCorruption, FreePtrBelowSlotArrayThrows) {
    // 100 slots need 16 + 400 = 416 bytes, but free_ptr claims payloads start
    // at 200 -- the two regions have crossed, which no real page can express.
    Page p;
    StompHeader(p, /*slot_count=*/100, /*free_ptr=*/200);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW((void)p.GetTuple(0), AxolotlDB::utils::CorruptPageException);
}

// --- invariant 4: a slot's payload escapes the payload region ---------------

TEST(PageCorruption, SlotOffsetOutsidePayloadRegionThrows) {
    Page p;
    const SlotIdx slot = p.InsertTuple(AsBytes("hello"));
    // Point the slot below free_ptr, into the free space / slot array.
    StompSlot(p, slot, /*offset=*/20, /*length=*/5);
    EXPECT_THROW((void)p.GetTuple(slot), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
}

TEST(PageCorruption, SlotExtendingPastPageEndThrows) {
    Page p;
    const SlotIdx slot = p.InsertTuple(AsBytes("hello"));
    // Offset is inside the payload region, but the length runs off the end.
    StompSlot(p, slot, static_cast<uint16_t>(PAGE_SIZE - 4), /*length=*/64);
    EXPECT_THROW((void)p.GetTuple(slot), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
}

TEST(PageCorruption, SlotEndingExactlyAtPageEndIsAccepted) {
    // A tuple whose last byte is the page's last byte is the normal case for
    // the first insert. Rejecting it would break every well-formed page.
    Page p;
    (void)p.InsertTuple(AsBytes("hello"));
    EXPECT_NO_THROW(p.ValidateOrThrow());
    EXPECT_NO_THROW((void)p.GetTuple(0));
}

TEST(PageCorruption, TombstonedSlotKeepsItsStaleOffsetWithoutFailingValidation) {
    // DeleteTuple only zeroes the length; the offset is left behind by design,
    // and GetTuple refuses the slot before ever reading it. Validating a
    // tombstone's offset would reject perfectly well-formed pages.
    Page p;
    const SlotIdx a = p.InsertTuple(AsBytes("first"));
    const SlotIdx b = p.InsertTuple(AsBytes("second"));
    p.DeleteTuple(a);

    EXPECT_NO_THROW(p.ValidateOrThrow());
    EXPECT_THROW((void)p.GetTuple(a), AxolotlDB::utils::InvalidSlotException);
    EXPECT_NO_THROW((void)p.GetTuple(b));
}

// --- a healthy page must not be disturbed -----------------------------------

TEST(PageCorruption, FreshPageValidates) {
    Page p;
    EXPECT_NO_THROW(p.ValidateOrThrow());
}

TEST(PageCorruption, WellFormedPageWithManyTuplesValidates) {
    Page p;
    for (int i = 0; i < 50; ++i) {
        const std::string s = "tuple-" + std::to_string(i);
        (void)p.InsertTuple(AsBytes(s));
    }
    EXPECT_NO_THROW(p.ValidateOrThrow());
    for (SlotIdx s = 0; s < p.SlotCount(); ++s) {
        EXPECT_NO_THROW((void)p.GetTuple(s));
    }
}

// --- the full read path ------------------------------------------------------

TEST(PageCorruption, CorruptPageFromDiskSurvivesTheReadPathWithoutOutOfBounds) {
    const auto path = std::filesystem::temp_directory_path() / "AxolotlDB_page_corruption_test.db";
    std::filesystem::remove(path);

    PageId id = INVALID_PAGE_ID;
    {
        FileManager fm(path.string());
        id = fm.AllocatePage();
        Page good;
        good.SetPageId(id);
        (void)good.InsertTuple(AsBytes("intact"));
        fm.WritePage(id, good);
        fm.Sync();
    }

    // Corrupt the header on disk, the way a torn write or a bit flip would.
    {
        std::fstream f(path.string(), std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        const uint16_t bad_slot_count = 60000;
        f.seekp(4, std::ios::beg);
        f.write(reinterpret_cast<const char*>(&bad_slot_count), sizeof(bad_slot_count));
        f.flush();
    }

    {
        FileManager fm(path.string());
        Page p;
        // ReadPage itself does not validate -- see the note at the top of this
        // file. What must hold is that nothing downstream reads out of bounds.
        ASSERT_NO_THROW(fm.ReadPage(id, p));
        EXPECT_THROW(p.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
        EXPECT_THROW((void)p.GetTuple(59999), AxolotlDB::utils::CorruptPageException);
        EXPECT_THROW((void)p.GetTuple(0), AxolotlDB::utils::CorruptPageException);
        EXPECT_THROW((void)p.InsertTuple(AsBytes("more")), AxolotlDB::utils::CorruptPageException);
    }

    std::filesystem::remove(path);
}

TEST(PageCorruption, ErrorMessageNamesTheViolatedInvariant) {
    Page p;
    StompHeader(p, /*slot_count=*/60000, /*free_ptr=*/PAGE_SIZE);
    try {
        p.ValidateOrThrow();
        FAIL() << "expected CorruptPageException";
    } catch (const AxolotlDB::utils::CorruptPageException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("slot_count"), std::string::npos) << msg;
        EXPECT_NE(msg.find("60000"), std::string::npos) << msg;
    }
}

// --- the raw-array constructor -----------------------------------------------

TEST(PageCorruption, RawArrayConstructorValidates) {
    // The only constructor that takes bytes this process did not produce, and
    // therefore the only one that validates. It has no call sites today; this
    // pins the contract for its first one.
    Page good;
    (void)good.InsertTuple(AsBytes("payload"));
    std::array<std::byte, PAGE_SIZE> bytes = good.RawData();

    const uint16_t bad_slot_count = 60000;
    std::memcpy(bytes.data() + 4, &bad_slot_count, sizeof(bad_slot_count));

    EXPECT_THROW(Page{bytes}, AxolotlDB::utils::CorruptPageException);
}

TEST(PageCorruption, RawArrayConstructorAcceptsAWellFormedPage) {
    Page good;
    (void)good.InsertTuple(AsBytes("payload"));
    const std::array<std::byte, PAGE_SIZE> bytes = good.RawData();

    Page copy{bytes};
    EXPECT_EQ(copy.SlotCount(), good.SlotCount());
    EXPECT_NO_THROW((void)copy.GetTuple(0));
}
