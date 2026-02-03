// P2 §2.5 — TableHeap. Rows addressed by RID, iteration in RID order, a
// free-space map that steers inserts to a page that fits, and B7 compaction so
// deleted space is reused.

#include <gtest/gtest.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <utils/errors.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;
namespace utils = AxolotlDB::utils;

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
std::string AsString(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}
bool RidLess(RID a, RID b) {
    return a.page_id != b.page_id ? a.page_id < b.page_id : a.slot_idx < b.slot_idx;
}

}  // namespace

struct TableHeapTest : ::testing::Test {
    std::filesystem::path path;
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
               ("axo_th_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db");
        std::filesystem::remove(path);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

TEST_F(TableHeapTest, InsertReturnsResolvableRid) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    std::vector<std::pair<RID, std::string>> inserted;
    for (int i = 0; i < 50; ++i) {
        const std::string row = "row-" + std::to_string(i) + "-" + std::string(30, 'x');
        const RID rid = th.Insert(AsBytes(row));
        ASSERT_TRUE(rid.IsValid()) << "Insert must return a valid RID";
        inserted.emplace_back(rid, row);
    }

    // Verify all RIDs resolve immediately and contents match
    for (const auto& [rid, expected] : inserted) {
        ReadPageGuard g;
        EXPECT_EQ(AsString(th.Get(rid, g)), expected);
    }
}

TEST_F(TableHeapTest, RowLargerThanPageThrows) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    // Any payload exceeding PAGE_SIZE - sizeof(PageHeader) - sizeof(SlotEntry)
    // cannot fit even on a fresh empty page and must throw ValueTooLargeException.
    const size_t max_payload = PAGE_SIZE - sizeof(PageHeader) - sizeof(SlotEntry);

    const std::string just_too_big(max_payload + 1, 'x');
    EXPECT_THROW((void)th.Insert(AsBytes(just_too_big)), utils::ValueTooLargeException)
        << "Inserting payload exceeding page capacity must throw ValueTooLargeException";

    const std::string huge(PAGE_SIZE * 2, 'y');
    EXPECT_THROW((void)th.Insert(AsBytes(huge)), utils::ValueTooLargeException);

    // Border condition: exact max_payload must fit on an empty page
    const std::string fits_empty(max_payload, 'z');
    RID fit_rid;
    EXPECT_NO_THROW({ fit_rid = th.Insert(AsBytes(fits_empty)); });
    EXPECT_TRUE(fit_rid.IsValid());
}

TEST_F(TableHeapTest, GetReturnsInsertedBytes) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    const RID r1 = th.Insert(AsBytes("alpha"));
    const RID r2 = th.Insert(AsBytes("beta"));
    ReadPageGuard g1, g2;
    EXPECT_EQ(AsString(th.Get(r1, g1)), "alpha");
    EXPECT_EQ(AsString(th.Get(r2, g2)), "beta");
}

TEST_F(TableHeapTest, GetOnTombstoneThrows) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    const RID r1 = th.Insert(AsBytes("survivor_1"));
    const RID r2 = th.Insert(AsBytes("doomed"));
    const RID r3 = th.Insert(AsBytes("survivor_2"));

    th.Delete(r2);

    ReadPageGuard g;
    EXPECT_THROW((void)th.Get(r2, g), utils::StorageException)
        << "Get on deleted/tombstoned row must throw";

    // Deleted twice or getting non-deleted rows
    EXPECT_EQ(AsString(th.Get(r1, g)), "survivor_1");
    EXPECT_EQ(AsString(th.Get(r3, g)), "survivor_2");
}

TEST_F(TableHeapTest, DeletedRidIsNotReadable) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    const RID a = th.Insert(AsBytes("keep"));
    const RID b = th.Insert(AsBytes("drop"));
    th.Delete(b);

    std::vector<RID> seen;
    for (RID r : th)
        seen.push_back(r);
    EXPECT_NE(std::find(seen.begin(), seen.end(), a), seen.end());
    EXPECT_EQ(std::find(seen.begin(), seen.end(), b), seen.end());
}

TEST_F(TableHeapTest, UpdateInPlaceKeepsRid) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    const RID rid = th.Insert(AsBytes("sizeAAAA"));

    // Case 1: Same length fits slot in-place
    const RID after_same = th.Update(rid, AsBytes("sizeBBBB"));
    EXPECT_EQ(after_same, rid) << "in-place update of equal length must retain RID";
    {
        ReadPageGuard g;
        EXPECT_EQ(AsString(th.Get(after_same, g)), "sizeBBBB");
    }

    // Case 2: Smaller length fits slot in-place
    const RID after_smaller = th.Update(rid, AsBytes("tiny"));
    EXPECT_EQ(after_smaller, rid) << "in-place update of smaller length must retain RID";
    {
        ReadPageGuard g;
        EXPECT_EQ(AsString(th.Get(after_smaller, g)), "tiny");
    }
}

TEST_F(TableHeapTest, UpdateGrowingRowReturnsNewRid) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    const RID rid = th.Insert(AsBytes("small"));
    const std::string bigger(2000, 'y');  // will not fit the old slot
    const RID after = th.Update(rid, AsBytes(bigger));
    EXPECT_NE(after, rid) << "growing update that exceeds slot must return a new RID";
    ReadPageGuard g;
    EXPECT_EQ(AsString(th.Get(after, g)), bigger);
}

TEST_F(TableHeapTest, OldRidIsInvalidAfterGrowingUpdate) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    const RID rid = th.Insert(AsBytes("small"));
    const RID after = th.Update(rid, AsBytes(std::string(2000, 'y')));
    ASSERT_NE(after, rid);
    ReadPageGuard g;
    EXPECT_THROW((void)th.Get(rid, g), utils::StorageException)
        << "a grown update leaves the old RID pointing at a tombstone";
}

TEST_F(TableHeapTest, IterationSkipsTombstones) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    std::vector<RID> live;
    for (int i = 0; i < 20; ++i) {
        const RID r = th.Insert(AsBytes("row-" + std::to_string(i)));
        if (i % 2 == 0)
            live.push_back(r);
        else
            th.Delete(r);
    }
    std::vector<RID> seen;
    for (RID r : th)
        seen.push_back(r);
    EXPECT_EQ(seen.size(), live.size());
    for (RID r : seen)
        EXPECT_NE(std::find(live.begin(), live.end(), r), live.end());
}

TEST_F(TableHeapTest, IterationVisitsRowsInRidOrder) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    for (int i = 0; i < 200; ++i)
        th.Insert(AsBytes("r" + std::to_string(i)));

    std::vector<RID> seen;
    for (RID r : th)
        seen.push_back(r);
    ASSERT_EQ(seen.size(), 200u);
    for (std::size_t i = 1; i < seen.size(); ++i)
        EXPECT_TRUE(RidLess(seen[i - 1], seen[i]))
            << "iteration must be strictly ascending in (page_id, slot)";
}

TEST_F(TableHeapTest, EmptyHeapIterationTerminates) {
    StorageManager sm(16);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);
    EXPECT_EQ(th.begin(), th.end());
    std::size_t n = 0;
    for (RID r : th) {
        (void)r;
        ++n;
    }
    EXPECT_EQ(n, 0u);
}

TEST_F(TableHeapTest, FreeSpaceMapDirectsInsertToAPageThatFits) {
    StorageManager sm(64);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    // Nearly fill page 0 with one large row (3500 bytes)
    const RID r_large1 = th.Insert(AsBytes(std::string(3500, 'A')));
    EXPECT_EQ(r_large1.page_id, 0u);

    // Insert another large row (1000 bytes) that cannot fit on page 0
    const RID r_large2 = th.Insert(AsBytes(std::string(1000, 'B')));
    EXPECT_NE(r_large2.page_id, 0u) << "must allocate page 1 because page 0 only has ~500B left";

    // Insert a small row (50 bytes) that DOES fit in the remaining space of page 0
    const RID r_small = th.Insert(AsBytes(std::string(50, 'C')));
    EXPECT_EQ(r_small.page_id, 0u) << "FSM must route small insert back to page 0 which has room";

    ReadPageGuard g1, g2, g3;
    EXPECT_EQ(AsString(th.Get(r_large1, g1)), std::string(3500, 'A'));
    EXPECT_EQ(AsString(th.Get(r_large2, g2)), std::string(1000, 'B'));
    EXPECT_EQ(AsString(th.Get(r_small, g3)), std::string(50, 'C'));
}

TEST_F(TableHeapTest, FreeSpaceMapSurvivesReopen) {
    std::vector<std::pair<RID, std::string>> rows;
    {
        StorageManager sm(64);
        const FileId fid = sm.OpenFile(path.string());
        TableHeap th(sm, fid);
        for (int i = 0; i < 50; ++i) {
            const std::string v = "persist-" + std::to_string(i);
            rows.emplace_back(th.Insert(AsBytes(v)), v);
        }
        sm.FlushAll();
    }
    {
        StorageManager sm(64);
        const FileId fid = sm.OpenFile(path.string());
        TableHeap th(sm, fid);
        for (auto& [rid, v] : rows) {
            ReadPageGuard g;
            EXPECT_EQ(AsString(th.Get(rid, g)), v);
        }
        const RID more = th.Insert(AsBytes("after-reopen"));
        ReadPageGuard g;
        EXPECT_EQ(AsString(th.Get(more, g)), "after-reopen");
    }
}

TEST_F(TableHeapTest, DeletedSpaceIsReusedBySubsequentInserts) {
    // B7. Fill a page, delete most of it, then insert as much again. Compaction
    // must reclaim the tombstoned space, so the file does not grow.
    StorageManager sm(64);
    const FileId fid = sm.OpenFile(path.string());
    TableHeap th(sm, fid);

    std::vector<RID> rids;
    for (int i = 0; i < 40; ++i)
        rids.push_back(th.Insert(AsBytes(std::string(80, 'z'))));
    sm.FlushFile(fid);
    const PageId before = sm.PageCount(fid);

    for (int i = 0; i < 40; ++i)
        th.Delete(rids[i]);
    for (int i = 0; i < 40; ++i)
        th.Insert(AsBytes(std::string(80, 'q')));
    sm.FlushFile(fid);
    const PageId after = sm.PageCount(fid);

    EXPECT_EQ(after, before) << "reclaimed space must be reused instead of growing the file (B7)";
}
