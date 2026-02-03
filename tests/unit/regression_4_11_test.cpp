// Regression tests for §4.11 "Key Bug Fixes in Phase 2".
//
// Naming convention: every test is *_RegressionFix4_11_N where N matches the
// numbered bug in the report, so a bisect can be traced back to the exact fix.
//
// DESIGN RULE: each test is written to FAIL against the buggy code as described
// in §4.11, not merely to pass against the current fix.  A comment in every
// test explains what the buggy code did and why the assertion would have been
// red before the fix.

#include <catalog/catalog.h>
#include <gtest/gtest.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/vector_heap.h>
#include <utils/errors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace AxolotlDB;
using namespace AxolotlDB::storage;
using namespace AxolotlDB::catalog;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Shared fixture: temp directory + chdir so catalog.db lands there.
// ---------------------------------------------------------------------------
struct RegressionFix4_11 : ::testing::Test {
    fs::path dir;
    fs::path orig_dir;
    void SetUp() override {
        orig_dir = fs::current_path();
        dir = fs::temp_directory_path() /
              ("axo_reg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        fs::current_path(dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::current_path(orig_dir, ec);
        fs::remove_all(dir, ec);
    }
};

// ===========================================================================
// §4.11 Bug #1 — UB pointer arithmetic in exception messages
//
// Buggy:   "Table with ID : " + id   where id is uint32_t
//          → C-string pointer arithmetic, not concatenation → UB/garbage.
// Fix:     std::to_string(id) in every lookup/error path.
// Breaks if: + id replaced with pointer arithmetic → message contains no digit.
// ===========================================================================
TEST_F(RegressionFix4_11, ExceptionMessageContainsNumericId_RegressionFix4_11_1) {
    storage::StorageManager sm(16);
    catalog::Catalog cat(sm);

    // GetTableName path: "There is no table with this ID : 9999"
    try {
        (void)cat.GetTableName(9999);
        FAIL() << "Expected StorageException";
    } catch (const utils::StorageException& e) {
        EXPECT_NE(std::string(e.what()).find("9999"), std::string::npos)
            << "Message must embed the numeric id; before fix '+id' on uint32_t "
               "was pointer arithmetic producing UB/garbage: " << e.what();
    }

    // GetTableId path
    try {
        (void)cat.GetTableId("__no_such__");
        FAIL() << "Expected StorageException";
    } catch (const utils::StorageException& e) {
        EXPECT_NE(std::string(e.what()).find("__no_such__"), std::string::npos)
            << "Message must embed the table name: " << e.what();
    }

    // GetSchema by id path
    try {
        (void)cat.GetSchema(static_cast<TableId>(42));
        FAIL() << "Expected StorageException";
    } catch (const utils::StorageException& e) {
        EXPECT_NE(std::string(e.what()).find("42"), std::string::npos)
            << "Message must embed '42': " << e.what();
    }
}

// ===========================================================================
// §4.11 Bug #2 — Catalog metadata header overwrite (write at byte 0)
//
// Buggy:   dst = guard->RawData().data()   — writes magic 0xCA7A1060 at
//          offset 0, clobbering the PageHeader.
// Fix:     dst = guard->RawData().data() + sizeof(PageHeader).
// Breaks if: the +24 offset is removed → page_id field becomes 0xCA7A1060.
// ===========================================================================
TEST_F(RegressionFix4_11, CatalogMetadataDoesNotClobberPageHeader_RegressionFix4_11_2) {
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        cat.CreateTable("t", storage::Schema{{
            storage::ColumnDef{"x", storage::ColumnType::INT32},
        }});
        sm.FlushAll();
    }

    // Re-read catalog.db's page 0 raw and inspect the PageHeader fields.
    {
        storage::StorageManager sm(16);
        storage::FileId fid = sm.OpenFile("catalog.db");
        ASSERT_GE(sm.PageCount(fid), 1u);
        storage::ReadPageGuard guard = sm.FetchRead(storage::PageRef{fid, 0});
        const auto& raw = guard->RawData();

        // PageHeader: page_id is the first 4 bytes (int32_t, little-endian).
        int32_t stored_page_id = 0;
        std::memcpy(&stored_page_id, raw.data(), sizeof(stored_page_id));
        EXPECT_EQ(stored_page_id, 0)
            << "page_id at offset 0 was overwritten (expected 0, got "
            << stored_page_id << ").\n"
               "Before fix, magic 0xCA7A1060 was written at byte 0, "
               "clobbering the PageHeader.";

        // free_ptr is uint16_t at byte offset 6 in PageHeader.
        uint16_t free_ptr = 0;
        std::memcpy(&free_ptr, raw.data() + 6, sizeof(free_ptr));
        EXPECT_LE(free_ptr, static_cast<uint16_t>(PAGE_SIZE))
            << "free_ptr=" << free_ptr << " exceeds PAGE_SIZE — PageHeader was overwritten.";
    }
}

// ===========================================================================
// §4.11 Bug #3 — Double persist & counter desync (next_table_id_ vs
//                next_heap_id_)
//
// Buggy:   AllocateHeapId() incremented next_table_id_ (copy-paste error).
// Fix:     AllocateHeapIdInternal() increments next_heap_id_ exclusively.
// Breaks if: the copy-paste is reintroduced → heap IDs alias table IDs.
// ===========================================================================
TEST_F(RegressionFix4_11, CountersDoNotDesyncAcrossTwoVectorTables_RegressionFix4_11_3) {
    storage::StorageManager sm(64);
    catalog::Catalog cat(sm);

    const auto schema = storage::Schema{{
        storage::ColumnDef{"id",  storage::ColumnType::INT32},
        storage::ColumnDef{"emb", storage::ColumnType::VECTOR, false, 4},
    }};

    const TableId  t1 = cat.CreateTable("first",  schema);
    const HeapId   h1 = cat.GetVectorHeap(t1, "emb").Id();

    const TableId  t2 = cat.CreateTable("second", schema);
    const HeapId   h2 = cat.GetVectorHeap(t2, "emb").Id();

    EXPECT_LT(t1, t2) << "table IDs must be strictly increasing";
    EXPECT_LT(h1, h2) << "heap IDs must be strictly increasing";

    // Under the copy-paste bug h1 == next value of next_table_id_ (not heap),
    // so h1 == t2 in the simplest failure case.
    EXPECT_NE(h1, t2)
        << "heap id from first table (" << h1 << ") must not equal second "
           "table id (" << t2 << ").\n"
           "Before fix, AllocateHeapId incremented next_table_id_ instead of "
           "next_heap_id_, making these collide.";
    EXPECT_NE(h1, INVALID_HEAP_ID);
    EXPECT_NE(h2, INVALID_HEAP_ID);

    // The next explicit AllocateHeapId() must be > h2.
    const HeapId h3 = cat.AllocateHeapId();
    EXPECT_GT(h3, h2)
        << "next_heap_id_ must have been tracking the heap counter exclusively; "
           "got h3=" << h3 << " which is not > h2=" << h2;
}

// ===========================================================================
// §4.11 Bug #4 — Catalog load offset misalignment (read from byte 0)
//
// Buggy:   src = guard->RawData().data()   — reads from offset 0 (PageHeader)
//          while persist wrote from offset 24.  Magic check triggers
//          "Corrupt catalog header magic" (page_id==0 != 0xCA7A1060), or
//          num_tables == 0 silently.
// Fix:     src = guard->RawData().data() + sizeof(PageHeader).
// Breaks if: the +24 offset is removed → all tables lost on reopen.
// ===========================================================================
TEST_F(RegressionFix4_11, LoadOffsetMatchesPersistOffset_RegressionFix4_11_4) {
    constexpr int kN = 4;
    std::vector<TableId> orig_ids;

    // Session 1: create kN tables, flush, destroy everything.
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        for (int i = 0; i < kN; ++i) {
            orig_ids.push_back(cat.CreateTable(
                "t" + std::to_string(i),
                storage::Schema{{storage::ColumnDef{"v", storage::ColumnType::INT32}}}));
        }
        sm.FlushAll();
    }

    // Session 2: brand-new SM + Catalog — zero shared in-memory state.
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);  // calls LoadCatalogMetadata

        for (int i = 0; i < kN; ++i) {
            const std::string name = "t" + std::to_string(i);
            EXPECT_NO_THROW((void)cat.GetTableId(name))
                << "Table '" << name << "' must survive reopen.\n"
                   "Before fix, LoadCatalogMetadata read from byte 0 (PageHeader) "
                   "instead of byte 24, so magic check failed or num_tables==0.";

            EXPECT_EQ(cat.GetTableId(name), orig_ids[static_cast<size_t>(i)])
                << "TableId for '" << name << "' must match first session.\n"
                   "Before fix, misaligned reads corrupted every stored TableId.";

            EXPECT_EQ(cat.GetSchema(orig_ids[static_cast<size_t>(i)]).ColumnCount(), 1u);
        }
    }
}

// ===========================================================================
// §4.11 Bug #5 — VectorHeap boundary math with 24-byte PageHeader
//
// Buggy:   Tests used PAGE_SIZE/sizeof(float)==1024 as the one-page constant.
//          With 24-byte header, usable bytes == 4072 → 1018 floats per page.
//          A 1024-float Put() spans 2 data pages, not 1.
// Fix:     Constant changed to (PAGE_SIZE-sizeof(PageHeader))/sizeof(float)==1018.
// Breaks if: dim==1024 is used as the "exactly one page" seam.
// Sub-test 5a: 1018 floats → exactly 2 file pages (header + 1 data page).
// Sub-test 5b: 1019 floats → exactly 3 file pages (header + 2 data pages).
// ===========================================================================
TEST_F(RegressionFix4_11, ExactlyOnePageWide_RegressionFix4_11_5a) {
    static_assert(sizeof(float) == 4);
    const uint32_t floats_per_page =
        static_cast<uint32_t>((PAGE_SIZE - sizeof(PageHeader)) / sizeof(float));
    ASSERT_EQ(floats_per_page, 1018u)
        << "Expected 1018 floats per page (4072 usable bytes / 4).\n"
           "If this fails, sizeof(PageHeader) changed from 24 — recheck the seam.";

    const std::string path = (dir / "vh_1018.db").string();
    {
        VectorHeap vh(path, /*heap_id=*/1, floats_per_page);
        std::vector<float> vec(floats_per_page, 1.0f);
        EXPECT_EQ(vh.Put(vec), 0u);
        const auto got = vh.Get(0u);
        ASSERT_EQ(got.size(), floats_per_page);
        EXPECT_FLOAT_EQ(got[0], 1.0f);
        vh.Sync();
    }

    const storage::FileManager fm(path, PAGE_SIZE);
    EXPECT_EQ(fm.PageCount(), 2u)
        << "1018-float vector must use exactly 1 data page (2 total with header).";

    // Detection power: the pre-fix seam was PAGE_SIZE/sizeof(float)==1024.
    // With 24-byte header math that must occupy 2 data pages (3 file pages).
    // If useable_page_size silently reverts to PAGE_SIZE, 1024 floats fit in
    // one data page and this assertion turns red.
    const uint32_t old_one_page_seam =
        static_cast<uint32_t>(PAGE_SIZE / sizeof(float));
    ASSERT_EQ(old_one_page_seam, 1024u);
    const std::string path_old = (dir / "vh_1024.db").string();
    {
        VectorHeap vh(path_old, /*heap_id=*/3, old_one_page_seam);
        std::vector<float> vec(old_one_page_seam, 3.0f);
        EXPECT_EQ(vh.Put(vec), 0u);
        vh.Sync();
    }
    const storage::FileManager fm_old(path_old, PAGE_SIZE);
    EXPECT_EQ(fm_old.PageCount(), 3u)
        << "1024-float vector (the old one-page constant) must span 2 data pages "
           "(3 total) under 24-byte header usable-size math.\n"
           "A useable_page_size==PAGE_SIZE revert makes this 2 file pages.";
}

TEST_F(RegressionFix4_11, OneMoreFloatSpansTwo_RegressionFix4_11_5b) {
    const uint32_t floats_per_page =
        static_cast<uint32_t>((PAGE_SIZE - sizeof(PageHeader)) / sizeof(float));
    const uint32_t dim_two = floats_per_page + 1;  // 1019

    const std::string path = (dir / "vh_1019.db").string();
    {
        VectorHeap vh(path, /*heap_id=*/2, dim_two);
        std::vector<float> vec(dim_two, 2.0f);
        EXPECT_EQ(vh.Put(vec), 0u);
        const auto got = vh.Get(0u);
        ASSERT_EQ(got.size(), dim_two);
        for (float f : got) EXPECT_FLOAT_EQ(f, 2.0f);
        vh.Sync();
    }

    const storage::FileManager fm(path, PAGE_SIZE);
    EXPECT_EQ(fm.PageCount(), 3u)
        << "1019-float vector must span 2 data pages (3 total with header).\n"
           "Confirms the boundary is at 1018, not 1024.";
}

// ===========================================================================
// §4.11 Bug #6 — Persistence gate modernization to StorageManager
//
// Buggy:   persistence_test.cpp used the deprecated P1 BufferPool(n,&fm) API.
// Fix:     Modernized to StorageManager + WritePageGuard/ReadPageGuard.
// Breaks if: the deprecated API is used — either fails to compile or misses
//            StorageManager book-keeping (eviction doesn't flush).
// Test strategy: Re-assert the persistence contract through the StorageManager
//                API surface alone.  500 rows under a tiny 8-frame pool.
// ===========================================================================
TEST_F(RegressionFix4_11, PersistenceContractViaStorageManagerAPI_RegressionFix4_11_6) {
    const fs::path db_path = dir / "persist_regression.db";
    constexpr int kRows = 40;
    // ~2 KiB payloads: two rows fill a page, 40 rows ≈ 20 data pages against
    // an 8-frame pool. FlushAll cannot cover pages already evicted; if
    // StorageManager skips write-back on eviction, reopen must lose rows.
    constexpr int kPayloadPad = 2000;

    std::vector<std::string> payloads;
    std::vector<storage::RID> rids;
    payloads.reserve(kRows);
    for (int i = 0; i < kRows; ++i)
        payloads.push_back("row_" + std::to_string(i) + std::string(kPayloadPad, 'x'));

    {
        storage::StorageManager sm(8);
        const storage::FileId fid = sm.OpenFile(db_path.string());
        storage::TableHeap th(sm, fid);
        for (const auto& p : payloads) {
            auto sp = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(p.data()), p.size());
            rids.push_back(th.Insert(sp));
        }
        sm.FlushAll();
    }

    {
        storage::StorageManager sm(8);
        const storage::FileId fid = sm.OpenFile(db_path.string());
        storage::TableHeap th(sm, fid);
        for (int i = 0; i < kRows; ++i) {
            storage::ReadPageGuard g;
            const auto bytes = th.Get(rids[static_cast<size_t>(i)], g);
            const std::string got(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            EXPECT_EQ(got, payloads[static_cast<size_t>(i)])
                << "Row " << i << " lost after reopen through StorageManager.\n"
                   "Must fail if eviction write-back is skipped even when FlushAll remains.";
        }
    }
}

// ===========================================================================
// §4.11 Bug #7 — Page checksum validation absent from ValidateOrThrow
//
// Buggy:   ValidateOrThrow checked structural invariants only; it never called
//          IsValidChecksum() — a bit-flip in the payload silently passed.
// Fix:     Added `if (hdr.checksum != 0 && !IsValidChecksum()) throw
//          CorruptPageException;` after structural checks.
// Breaks if: the if-block is removed → bit-flip passes ValidateOrThrow.
// ===========================================================================
TEST_F(RegressionFix4_11, FlippedByteFailsValidateOrThrow_RegressionFix4_11_7) {
    // Build a structurally valid page, call UpdateChecksum().
    Page p;
    p.SetPageId(0);
    {
        const std::string data(16, 'A');
        std::span<const std::byte> sp{
            reinterpret_cast<const std::byte*>(data.data()), data.size()};
        (void)p.InsertTuple(sp);
    }
    p.UpdateChecksum();

    // Sanity: correct page must pass.
    {
        Page good(p.RawData());
        EXPECT_NO_THROW(good.ValidateOrThrow())
            << "Correct checksum must pass ValidateOrThrow";
    }

    // Flip one bit in the payload area (byte 100, safely past the header).
    std::array<std::byte, PAGE_SIZE> corrupted = p.RawData();
    corrupted[100] ^= std::byte{0x01};

    // Corrupted page must throw CorruptPageException.
    // Before fix: ValidateOrThrow returned silently — silent data corruption.
    EXPECT_THROW(
        {
            Page bad(corrupted);
            bad.ValidateOrThrow();
        },
        utils::CorruptPageException)
        << "ValidateOrThrow must throw CorruptPageException on checksum mismatch.\n"
           "Before fix, the checksum branch was absent and bit-flips passed silently.";
}
