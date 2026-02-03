#include <gtest/gtest.h>
#include <storage/buffer_pool.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string AsString(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Page-level: the dirty flag is in-memory state, not page content.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PageDirtyFlag, FreshPageIsClean) {
    Page p;
    EXPECT_FALSE(p.IsDirty());
}

TEST(PageDirtyFlag, InsertTupleMarksDirty) {
    Page p;
    const std::string payload = "hello";
    ASSERT_EQ(p.InsertTuple(AsBytes(payload)), 0);
    EXPECT_TRUE(p.IsDirty()) << "a modified page must report itself dirty";
}

TEST(PageDirtyFlag, DeleteTupleMarksDirty) {
    Page p;
    const std::string payload = "hello";
    const SlotIdx slot = p.InsertTuple(AsBytes(payload));
    p.ClearDirty();
    ASSERT_FALSE(p.IsDirty());

    p.DeleteTuple(slot);
    EXPECT_TRUE(p.IsDirty()) << "a tombstoned slot is a modification";
}

TEST(PageDirtyFlag, ClearDirtyResets) {
    Page p;
    p.MarkDirty();
    ASSERT_TRUE(p.IsDirty());
    p.ClearDirty();
    EXPECT_FALSE(p.IsDirty());
}

TEST(PageDirtyFlag, DirtyFlagIsNotStoredInPageBytes) {
    Page a;
    Page b;
    a.SetPageId(7);
    b.SetPageId(7);
    ASSERT_TRUE(a.RawData() == b.RawData()) << "identical pages must start identical";

    a.MarkDirty();

    EXPECT_TRUE(a.IsDirty());
    EXPECT_FALSE(b.IsDirty());
    EXPECT_TRUE(a.RawData() == b.RawData())
        << "the dirty flag must not live inside the page's serialised bytes";
}

TEST(PageDirtyFlag, ClearDirtyDoesNotChangePageBytes) {
    Page a;
    Page b;
    a.SetPageId(3);
    b.SetPageId(3);
    a.MarkDirty();
    a.ClearDirty();

    EXPECT_TRUE(a.RawData() == b.RawData()) << "clearing the dirty flag must not modify the page";
}

// ─────────────────────────────────────────────────────────────────────────────
// StorageManager + BufferPool integration.
// ─────────────────────────────────────────────────────────────────────────────

class BufferPoolDirtyTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        db_path = std::string("test_b1_") + info->name() + ".db";
        std::filesystem::remove(db_path);
    }

    void TearDown() override {
        std::filesystem::remove(db_path);
    }

    std::string db_path;
};

TEST_F(BufferPoolDirtyTest, ModifiedPageSurvivesEvictionWhenUnpinnedClean) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    {
        auto g0 = sm.NewPage(fid, p0);
        const std::string payload = "durable-b1";
        ASSERT_EQ((*g0).InsertTuple(AsBytes(payload)), 0);
    }

    // Push p0 out of the 2-frame pool.
    PageRef p1{}, p2{};
    { auto g1 = sm.NewPage(fid, p1); }
    { auto g2 = sm.NewPage(fid, p2); }

    auto refetched = sm.FetchRead(p0);
    ASSERT_EQ((*refetched).SlotCount(), 1)
        << "the insert was silently discarded when the frame was evicted";
    EXPECT_EQ(AsString((*refetched).GetTuple(0)), "durable-b1");
}

TEST_F(BufferPoolDirtyTest, PageReadFromDiskIsClean) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    {
        auto g0 = sm.NewPage(fid, p0);
        const std::string payload = "on-disk";
        ASSERT_EQ((*g0).InsertTuple(AsBytes(payload)), 0);
        sm.FlushAll();
    }

    // Evict p0 so the refetch has to go to disk.
    PageRef p1{}, p2{};
    { auto g1 = sm.NewPage(fid, p1); }
    { auto g2 = sm.NewPage(fid, p2); }

    auto refetched = sm.FetchRead(p0);
    EXPECT_FALSE((*refetched).IsDirty())
        << "a page freshly read from disk is identical to disk, so it is clean";
}

TEST_F(BufferPoolDirtyTest, NewlyAllocatedPageIsDirty) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    auto g0 = sm.NewPage(fid, p0);
    EXPECT_TRUE((*g0).IsDirty()) << "a new page's header only exists in RAM until it is flushed";
}

TEST_F(BufferPoolDirtyTest, NewPageHeaderSurvivesEvictionWithoutModification) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    { auto g0 = sm.NewPage(fid, p0); }

    PageRef p1{}, p2{};
    { auto g1 = sm.NewPage(fid, p1); }
    { auto g2 = sm.NewPage(fid, p2); }

    auto refetched = sm.FetchRead(p0);
    EXPECT_GT((*refetched).FreeSpace(), 0u) << "the new page's header was never written to disk";
    EXPECT_EQ((*refetched).GetPageId(), p0.page_no);
}

TEST_F(BufferPoolDirtyTest, FlushPageClearsDirty) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    {
        auto g0 = sm.NewPage(fid, p0);
        const std::string payload = "flushed";
        ASSERT_EQ((*g0).InsertTuple(AsBytes(payload)), 0);
    }
    sm.FlushFile(fid);

    auto same = sm.FetchRead(p0);
    EXPECT_FALSE((*same).IsDirty()) << "a just-flushed page matches disk";
}

TEST_F(BufferPoolDirtyTest, OutOfRangeFetchThrows) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    { auto g = sm.NewPage(fid, p0); }

    EXPECT_THROW((void)sm.FetchRead(PageRef{fid, 9999}), AxolotlDB::utils::InvalidPageException);
    EXPECT_THROW((void)sm.FetchRead(PageRef{fid, -1}), AxolotlDB::utils::InvalidPageException);
}

TEST_F(BufferPoolDirtyTest, FailedFetchDoesNotConsumeFrames) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{}, p1{}, p2{};
    { auto g0 = sm.NewPage(fid, p0); }
    { auto g1 = sm.NewPage(fid, p1); }
    { auto g2 = sm.NewPage(fid, p2); }

    for (int i = 0; i < 10; ++i) {
        EXPECT_THROW((void)sm.FetchRead(PageRef{fid, 9999}), AxolotlDB::utils::InvalidPageException);
    }

    auto a = sm.FetchRead(p0);
    auto b = sm.FetchRead(p1);
    auto c = sm.FetchRead(p2);

    EXPECT_EQ(sm.PoolCapacity(), 3u);
}

TEST_F(BufferPoolDirtyTest, FailedFetchLeavesCachedPageIntact) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    const std::string payload = "untouched";
    {
        auto g0 = sm.NewPage(fid, p0);
        ASSERT_EQ((*g0).InsertTuple(AsBytes(payload)), 0);
    }

    EXPECT_THROW((void)sm.FetchRead(PageRef{fid, 9999}), AxolotlDB::utils::InvalidPageException);

    auto refetched = sm.FetchRead(p0);
    ASSERT_EQ((*refetched).SlotCount(), 1);
    EXPECT_EQ(AsString((*refetched).GetTuple(0)), payload);
}

TEST_F(BufferPoolDirtyTest, FlushAllPagesPersistsModifiedPages) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{};
    const std::string payload = "persisted";
    {
        auto g0 = sm.NewPage(fid, p0);
        ASSERT_EQ((*g0).InsertTuple(AsBytes(payload)), 0);
    }

    sm.FlushAll();

    auto same = sm.FetchRead(p0);
    ASSERT_EQ((*same).SlotCount(), 1);
    EXPECT_EQ(AsString((*same).GetTuple(0)), payload);
    EXPECT_FALSE((*same).IsDirty());
}

TEST(BufferPoolConstruction, ZeroPoolSizeThrows) {
    EXPECT_THROW(BufferPool(0), std::invalid_argument);
}

TEST(BufferPoolConstruction, ValidPoolSizeInitializes) {
    BufferPool pool(128);
    EXPECT_EQ(pool.Capacity(), 128u);
    EXPECT_EQ(pool.OccupiedFrames(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Page slot-directory semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST(PageSlotDirectory, InsertThrowsPageFullWhenNoRoomRemains) {
    Page p;
    const std::string big(PAGE_SIZE / 4, 'x');

    int inserted = 0;
    bool threw = false;
    for (int i = 0; i < 10; ++i) {
        try {
            (void)p.InsertTuple(AsBytes(big));
            ++inserted;
        } catch (const AxolotlDB::utils::PageFullException&) {
            threw = true;
            break;
        }
    }

    EXPECT_TRUE(threw) << "page never reported itself full";
    EXPECT_GT(inserted, 0) << "page rejected even the first tuple";
    EXPECT_LT(inserted, 5) << "page claimed to fit more than physically possible";
}

TEST(PageSlotDirectory, PageFullLeavesEarlierTuplesReadable) {
    Page p;
    const std::string first = "keep-me";
    const SlotIdx slot = p.InsertTuple(AsBytes(first));

    const std::string huge(PAGE_SIZE, 'y');
    EXPECT_THROW((void)p.InsertTuple(AsBytes(huge)), AxolotlDB::utils::PageFullException);

    EXPECT_EQ(p.SlotCount(), 1) << "failed insert still consumed a slot";
    EXPECT_EQ(AsString(p.GetTuple(slot)), first) << "failed insert damaged an existing tuple";
}

TEST(PageSlotDirectory, ZeroLengthInsertIsRejected) {
    Page p;
    EXPECT_THROW((void)p.InsertTuple(std::span<const std::byte>{}),
                 AxolotlDB::utils::StorageException);
}

TEST(PageSlotDirectory, GetTupleOnDeletedSlotThrows) {
    Page p;
    const SlotIdx slot = p.InsertTuple(AsBytes(std::string("doomed")));
    ASSERT_EQ(AsString(p.GetTuple(slot)), "doomed");

    p.DeleteTuple(slot);
    EXPECT_THROW((void)p.GetTuple(slot), AxolotlDB::utils::InvalidSlotException)
        << "a deleted slot must not hand back its old bytes";
}

TEST(PageSlotDirectory, DoubleDeleteThrows) {
    Page p;
    const SlotIdx slot = p.InsertTuple(AsBytes(std::string("once")));
    p.DeleteTuple(slot);
    EXPECT_THROW(p.DeleteTuple(slot), AxolotlDB::utils::InvalidSlotException);
}

TEST(PageSlotDirectory, OutOfRangeSlotThrows) {
    Page p;
    (void)p.InsertTuple(AsBytes(std::string("only-slot-0")));

    EXPECT_THROW((void)p.GetTuple(1), AxolotlDB::utils::InvalidSlotException);
    EXPECT_THROW((void)p.GetTuple(9999), AxolotlDB::utils::InvalidSlotException);
    EXPECT_THROW(p.DeleteTuple(1), AxolotlDB::utils::InvalidSlotException);
}

TEST(PageSlotDirectory, DeletingOneSlotLeavesItsNeighboursIntact) {
    Page p;
    const SlotIdx a = p.InsertTuple(AsBytes(std::string("alpha")));
    const SlotIdx b = p.InsertTuple(AsBytes(std::string("bravo")));
    const SlotIdx c = p.InsertTuple(AsBytes(std::string("charlie")));

    p.DeleteTuple(b);

    EXPECT_EQ(AsString(p.GetTuple(a)), "alpha");
    EXPECT_THROW((void)p.GetTuple(b), AxolotlDB::utils::InvalidSlotException);
    EXPECT_EQ(AsString(p.GetTuple(c)), "charlie")
        << "deleting a slot shifted or clobbered a later one";

    EXPECT_EQ(p.SlotCount(), 3);
}

TEST(PageSlotDirectory, DeleteTupleAloneDoesNotReclaimUntilCompact_B7Closed) {
    Page p;
    const std::string payload(200, 'z');
    const SlotIdx slot = p.InsertTuple(AsBytes(payload));

    const uint32_t after_insert = p.FreeSpace();
    p.DeleteTuple(slot);
    const uint32_t after_delete = p.FreeSpace();

    // Bare DeleteTuple only tombstones; space is not reclaimed until Compact()
    EXPECT_EQ(after_delete, after_insert);

    p.Compact();
    const uint32_t after_compact = p.FreeSpace();
    EXPECT_EQ(after_compact, after_insert + 200);
}

TEST(PageSlotDirectory, FreeSpaceShrinksByPayloadPlusSlotOverhead) {
    Page p;
    const uint32_t initial = p.FreeSpace();
    ASSERT_GT(initial, 0u);

    const std::string payload(100, 'q');
    (void)p.InsertTuple(AsBytes(payload));

    const uint32_t after = p.FreeSpace();
    EXPECT_EQ(initial - after, payload.size() + sizeof(SlotEntry));
}

TEST(PageSlotDirectory, FreeSpaceIsMonotonicAcrossManyInserts) {
    Page p;
    uint32_t previous = p.FreeSpace();
    const std::string payload(64, 'm');

    for (int i = 0; i < 10; ++i) {
        (void)p.InsertTuple(AsBytes(payload));
        const uint32_t now = p.FreeSpace();
        EXPECT_LT(now, previous) << "FreeSpace did not shrink on insert " << i;
        previous = now;
    }
}

TEST(PageSlotDirectory, FreeSpaceNeverExceedsPageSize) {
    Page p;
    EXPECT_LT(p.FreeSpace(), PAGE_SIZE);
}

TEST_F(BufferPoolDirtyTest, EvictionFollowsLeastRecentlyUsedOrder) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(db_path);

    PageRef p0{}, p1{}, p2{};
    {
        auto g0 = sm.NewPage(fid, p0);
        (*g0).InsertTuple(AsBytes("zero"));
    }
    {
        auto g1 = sm.NewPage(fid, p1);
        (*g1).InsertTuple(AsBytes("one"));
    }
    {
        auto g2 = sm.NewPage(fid, p2);
        (*g2).InsertTuple(AsBytes("two"));
    }

    // Touch p0, making p1 the least-recently-used frame.
    { auto touched = sm.FetchRead(p0); }

    // A fourth page must evict p1, not p0.
    PageRef p3{};
    { auto g3 = sm.NewPage(fid, p3); }

    EXPECT_EQ(sm.PoolCapacity(), 3u);

    for (const auto& [ref, want] :
         std::vector<std::pair<PageRef, std::string>>{{p0, "zero"}, {p1, "one"}, {p2, "two"}}) {
        auto p = sm.FetchRead(ref);
        EXPECT_EQ(AsString((*p).GetTuple(0)), want)
            << "page " << ref.page_no << " came back with the wrong contents";
    }
}

TEST(BufferPoolDirectTest, OverUnpinThrowsButKeepsDirtyFlag) {
    BufferPool bp(4);
    PageRef ref{1, 10};
    auto r = bp.Pin(ref);
    r.page->RawData()[100] = std::byte{0xAB};
    r.page->ClearDirty();
    bp.Unpin(ref, /*is_dirty=*/false);  // balanced; pin_count is now 0

    // The unbalanced one must complain.
    EXPECT_THROW(bp.Unpin(ref, /*is_dirty=*/true), AxolotlDB::utils::InvalidPageException);

    // ...but the modification it was told about must still have been recorded.
    auto again = bp.Pin(ref);
    EXPECT_TRUE(again.page->IsDirty()) << "an over-unpin lost the dirty flag";
    EXPECT_EQ(again.page->RawData()[100], std::byte{0xAB});
    bp.Unpin(ref, false);
}

TEST(BufferPoolDirectTest, OverUnpinNamesThePageItRefused) {
    BufferPool bp(4);
    PageRef ref{1, 42};
    (void)bp.Pin(ref);
    bp.Unpin(ref, false);

    try {
        bp.Unpin(ref, false);
        FAIL() << "expected InvalidPageException";
    } catch (const AxolotlDB::utils::InvalidPageException& e) {
        EXPECT_NE(std::string(e.what()).find("42"), std::string::npos) << e.what();
    }
}
