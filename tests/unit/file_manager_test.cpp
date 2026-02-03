// Regression tests for audit finding A4 -- FileManager::Sync() did not sync
// (docs/audit.md).
//
// Before the fix Sync() was a bare file_.flush(): it moved bytes from the
// std::fstream's userspace buffer into the OS page cache and returned. The
// header comment claimed "(fsync)". Nothing ever reached the device, and
// nothing in the repo called Sync() at all -- BufferPool::FlushAllPages()
// wrote every dirty frame and returned.
//
// What these tests can and cannot prove
// -------------------------------------
// No unit test can prove that fsync(2) reached the platter; that needs real
// power loss or a fault-injecting block device. Killing a subprocess proves
// only that data left the userspace buffer -- the kernel still holds it -- so
// such a test passes just as happily against the old broken flush()-only code
// and would be worse than useless here.
//
// What is testable, and is what these tests pin down:
//   * Sync() completes without throwing on a healthy file, on every platform
//     (_commit / F_FULLFSYNC / fsync all wired up and given a valid fd).
//   * Sync() moves data far enough that an independent reader sees it -- this
//     fails outright if Sync() is ever reduced to a no-op.
//   * A durability round trip: bytes written through the buffer pool survive
//     destruction of every in-process object and reopen byte-identical.
//   * FlushAllPages() actually reaches Sync(), which is the half of A4 that
//     made the fsync unreachable.
//   * Sync() refuses to report success on a stream that is already poisoned,
//     rather than flushing nothing and returning cleanly.

#include <gtest/gtest.h>
#include <storage/buffer_pool.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string AsString(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Fill a page's raw bytes with a per-page deterministic pattern, so a page
// read back from disk can be attributed to the page that wrote it. A page
// that came back zero-filled, or that came back as its neighbour, both fail.
void PaintPage(Page& p, PageId id) {
    auto& raw = p.RawData();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::byte>((static_cast<std::size_t>(id) * 31 + i) & 0xFF);
    }
}

void ExpectPainted(const Page& p, PageId id) {
    const auto& raw = p.RawData();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto want = static_cast<std::byte>((static_cast<std::size_t>(id) * 31 + i) & 0xFF);
        ASSERT_EQ(raw[i], want) << "page " << id << " byte " << i
                                << " did not survive the round trip";
    }
}

}  // namespace

class FileManagerSyncTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        db_path = std::string("test_a4_") + info->name() + ".db";
        std::filesystem::remove(db_path);
    }

    void TearDown() override {
        std::filesystem::remove(db_path);
    }

    std::string db_path;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sync() is wired up and does not throw on a healthy file.
//
// These are thin, but they are the tests that catch a broken platform branch:
// a sync_fd_ opened read-only fails _commit() on Windows with EACCES while
// working fine under POSIX fsync, so a bad #ifdef is invisible on Linux CI.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, SyncOnEmptyFileSucceeds) {
    FileManager fm(db_path);
    EXPECT_NO_THROW(fm.Sync());
}

TEST_F(FileManagerSyncTest, SyncAfterWriteSucceeds) {
    FileManager fm(db_path);
    const PageId id = fm.AllocatePage();

    Page p;
    PaintPage(p, id);
    fm.WritePage(id, p);

    EXPECT_NO_THROW(fm.Sync());
}

TEST_F(FileManagerSyncTest, SyncIsIdempotent) {
    FileManager fm(db_path);
    const PageId id = fm.AllocatePage();

    Page p;
    PaintPage(p, id);
    fm.WritePage(id, p);

    EXPECT_NO_THROW(fm.Sync());
    EXPECT_NO_THROW(fm.Sync()) << "a second Sync with nothing dirty must be a no-op, "
                                  "not an error";
    EXPECT_NO_THROW(fm.Sync());
}

// ─────────────────────────────────────────────────────────────────────────────
// Sync() pushes data out of the process.
//
// The strongest claim a unit test can make. A second FileManager on the same
// path is an independent set of handles; if Sync() left the bytes sitting in
// the first fstream's userspace buffer, the reader sees a zero-filled page.
// This is the test that fails if Sync() is ever gutted back to a no-op.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, SyncedDataIsVisibleToAnIndependentReader) {
    FileManager writer(db_path);
    const PageId id = writer.AllocatePage();

    Page written;
    PaintPage(written, id);
    writer.WritePage(id, written);
    writer.Sync();

    // writer is deliberately still open and un-destroyed.
    FileManager reader(db_path);
    ASSERT_EQ(reader.PageCount(), 1);

    Page read_back;
    reader.ReadPage(id, read_back);
    ExpectPainted(read_back, id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Durability round trip -- also the persistence test audit section D says the
// suite has never had.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, PagesSurviveReopen) {
    constexpr PageId kPages = 5;

    {
        FileManager fm(db_path);
        for (PageId i = 0; i < kPages; ++i) {
            const PageId id = fm.AllocatePage();
            ASSERT_EQ(id, i);
            Page p;
            PaintPage(p, id);
            fm.WritePage(id, p);
        }
        fm.Sync();
    }  // fm destroyed -- every in-process buffer is gone

    FileManager reopened(db_path);
    ASSERT_EQ(reopened.PageCount(), kPages)
        << "page count must be recovered from the file size on reopen";

    for (PageId id = 0; id < kPages; ++id) {
        Page p;
        reopened.ReadPage(id, p);
        ExpectPainted(p, id);
    }
}

TEST_F(FileManagerSyncTest, PageCountRecoveredOnReopen) {
    {
        FileManager fm(db_path);
        for (int i = 0; i < 3; ++i) {
            (void)fm.AllocatePage();
        }
        ASSERT_EQ(fm.PageCount(), 3);
        fm.Sync();
    }

    FileManager reopened(db_path);
    EXPECT_EQ(reopened.PageCount(), 3);

    // And allocation continues from where it left off rather than clobbering.
    const PageId next = reopened.AllocatePage();
    EXPECT_EQ(next, 3);
    EXPECT_EQ(reopened.PageCount(), 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// FlushAllPages() reaches Sync().
//
// This is the half of A4 that left the fsync unreachable: Sync() had zero
// call sites in the repo, so a correct implementation changed nothing.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, TuplesSurviveFlushAllPagesAndReopen) {
    const std::string payload = "A4 durability round trip";
    PageId written_id = INVALID_PAGE_ID;
    SlotIdx slot = INVALID_SLOT;

    {
        StorageManager sm(4);
        const FileId fid = sm.OpenFile(db_path);

        PageRef pref;
        WritePageGuard guard = sm.NewPage(fid, pref);
        written_id = pref.page_no;
        slot = guard->InsertTuple(AsBytes(payload));
        guard.Drop();

        sm.FlushAll();
    }  // sm destroyed

    StorageManager reopened(4);
    const FileId reopened_fid = reopened.OpenFile(db_path);

    ReadPageGuard rguard = reopened.FetchRead(PageRef{reopened_fid, written_id});
    EXPECT_EQ(AsString(rguard->GetTuple(slot)), payload);
}

TEST_F(FileManagerSyncTest, FlushAllPagesCallsSync) {
    StorageManager sm(4);
    const FileId fid = sm.OpenFile(db_path);

    PageRef pref;
    WritePageGuard guard = sm.NewPage(fid, pref);
    (void)guard->InsertTuple(AsBytes(std::string("payload")));
    guard.Drop();

    PageRef pref2;
    WritePageGuard guard2 = sm.NewPage(fid, pref2);
    guard2.Drop();
    sm.FlushAll();  // file is now 2 pages

    // Evict pref2 from the 4-frame pool by allocating pages in a second file.
    const std::string dummy_path = db_path + ".dummy";
    const FileId fid2 = sm.OpenFile(dummy_path);
    for (int i = 0; i < 5; ++i) {
        PageRef dr;
        WritePageGuard dg = sm.NewPage(fid2, dr);
        dg.Drop();
    }
    sm.FlushFile(fid2);

    Page scratch;
    std::error_code ec;
    std::filesystem::resize_file(db_path, PAGE_SIZE, ec);
    if (ec) {
        GTEST_SKIP() << "cannot truncate an open database file on this platform: " << ec.message();
    }

    // Reading the now-missing page 1 hits EOF on the evicted page.
    // ReadPage tolerates eofbit (audit finding A7) and does not throw --
    // but the stream stays poisoned.
    try {
        (void)sm.FetchRead(pref2);
    } catch (const AxolotlDB::utils::AxolotException&) {
        // Fine either way; what matters is the stream state afterwards.
    }

    // Nothing is dirty on fid, so the write loop is a no-op and Sync() is the
    // only statement left that can fail. A clean return means Sync() was never
    // called -- exactly the A4 defect.
    EXPECT_THROW(sm.FlushAll(), AxolotlDB::utils::IOException)
        << "FlushAllPages must reach Sync(); a clean return means the fsync "
           "is unreachable and A4 is not fixed";

    std::filesystem::remove(dummy_path, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sync() does not report success it cannot deliver.
//
// The trap in the fstream-plus-side-descriptor design: fsync flushes the
// kernel's dirty pages and knows nothing about the userspace stream buffer.
// A Sync() that fsyncs without a successful flush() first returns success
// having synced whatever happened to already be in the page cache.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, SyncThrowsOnPoisonedStream) {
    FileManager fm(db_path);
    (void)fm.AllocatePage();
    (void)fm.AllocatePage();
    fm.Sync();

    std::error_code ec;
    std::filesystem::resize_file(db_path, PAGE_SIZE, ec);
    if (ec) {
        GTEST_SKIP() << "cannot truncate an open database file on this platform: " << ec.message();
    }

    Page scratch;
    try {
        fm.ReadPage(1, scratch);
    } catch (const AxolotlDB::utils::AxolotException&) {
    }

    EXPECT_THROW(fm.Sync(), AxolotlDB::utils::IOException)
        << "Sync() on a stream that can no longer flush must fail loudly, not "
           "silently fsync a stale page cache";
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction failure paths -- the sync descriptor must not change them.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, ConstructionOnUncreatablePathThrows) {
    const std::string bad = "no_such_dir_a4/nested/db.axolotl";
    ASSERT_FALSE(std::filesystem::exists("no_such_dir_a4"));
    EXPECT_THROW(FileManager fm(bad), AxolotlDB::utils::IOException);
}

TEST_F(FileManagerSyncTest, RepeatedFailedConstructionDoesNotExhaustDescriptors) {
    // A sync_fd_ opened before the constructor's last throw site would leak on
    // every failure, because a throwing constructor never runs ~FileManager().
    // 300 failures is well under any fd limit if nothing leaks, and well over
    // the default soft limit's headroom if each one leaks a descriptor.
    const std::string bad = "no_such_dir_a4/nested/db.axolotl";
    for (int i = 0; i < 300; ++i) {
        EXPECT_THROW(FileManager fm(bad), AxolotlDB::utils::IOException);
    }

    // The process must still be able to open files afterwards.
    FileManager fm(db_path);
    EXPECT_NO_THROW(fm.Sync());
}

// ─────────────────────────────────────────────────────────────────────────────
// A6 -- FileManager accepts a page_size it cannot honour.
//
// Page::data_ is std::array<std::byte, PAGE_SIZE>, fixed at compile time.
// page_size_ is a runtime constructor parameter with nothing checking it
// against PAGE_SIZE. A larger page_size makes ReadPage/WritePage memcpy past
// the end of the fixed-size buffer -- a real overflow, not just a logic bug.
// A smaller page_size doesn't overflow anything, but silently changes the
// file's page-boundary math (AllocatePage's offsets, PageCount()'s division)
// away from what Page's own layout (e.g. free_ptr = PAGE_SIZE) assumes,
// which the header comment ("Must match PAGE_SIZE") rules out either way.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, OversizedPageSizeThrows) {
    EXPECT_THROW(FileManager fm(db_path, PAGE_SIZE * 2), std::invalid_argument);
}

TEST_F(FileManagerSyncTest, UndersizedPageSizeThrows) {
    EXPECT_THROW(FileManager fm(db_path, PAGE_SIZE / 2), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// N10 -- AllocatePage claimed the page before writing it, and flushed per call.
//
// The old code did fetch_add, wrote, and on failure did fetch_sub. That rewind
// left a window: a partial write had already extended the file while the
// counter had disowned the page. Reopening derives page_count_ from the file
// size, so the next run counted a page holding garbage -- and a Page built from
// garbage bytes is exactly what N1's validation exists to reject. The two
// findings are in P0 together because this is the path that joins them.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FileManagerSyncTest, PageCountSurvivesReopenAfterAllocate) {
    constexpr int kPages = 32;
    {
        FileManager fm(db_path);
        for (int i = 0; i < kPages; ++i) {
            const PageId id = fm.AllocatePage();
            EXPECT_EQ(id, i) << "AllocatePage must hand out dense ids from zero";
        }
        EXPECT_EQ(fm.PageCount(), kPages);
        fm.Sync();
    }
    {
        FileManager fm(db_path);
        EXPECT_EQ(fm.PageCount(), kPages)
            << "page_count_ is derived from the file size on reopen; dropping "
               "the per-call flush must not change what reaches the file";
    }
}

TEST_F(FileManagerSyncTest, BulkAllocateIsStillDurableAfterSync) {
    // Removing the per-call flush() is a throughput change, not a durability
    // one: durability begins when Sync() returns (audit A4's contract). This
    // asserts the contract still holds, rather than counting flushes.
    constexpr int kPages = 64;
    {
        FileManager fm(db_path);
        for (int i = 0; i < kPages; ++i)
            (void)fm.AllocatePage();

        Page p;
        p.SetPageId(kPages - 1);
        (void)p.InsertTuple(AsBytes("written after a bulk allocate"));
        fm.WritePage(kPages - 1, p);
        fm.Sync();
    }
    {
        FileManager fm(db_path);
        ASSERT_EQ(fm.PageCount(), kPages);
        Page p;
        fm.ReadPage(kPages - 1, p);
        EXPECT_EQ(AsString(p.GetTuple(0)), "written after a bulk allocate");
    }
}

TEST_F(FileManagerSyncTest, DisownedPageReadsAsCorruptNotAsValid) {
    // Simulate the surviving skew: a page's bytes reached the file while the
    // in-memory counter did not claim it. On reopen the count is derived from
    // the file size, so that page IS addressable -- and it holds garbage.
    {
        FileManager fm(db_path);
        (void)fm.AllocatePage();
        Page p;
        p.SetPageId(0);
        (void)p.InsertTuple(AsBytes("legitimate"));
        fm.WritePage(0, p);
        fm.Sync();
    }

    // Append a page's worth of garbage, the way a partial write would leave it.
    {
        std::ofstream f(db_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(f.is_open());
        std::vector<char> junk(PAGE_SIZE);
        for (std::size_t i = 0; i < junk.size(); ++i)
            junk[i] = static_cast<char>(0xDD);
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    FileManager fm(db_path);
    ASSERT_EQ(fm.PageCount(), 2) << "the appended page is addressable on reopen";

    Page good;
    fm.ReadPage(0, good);
    EXPECT_EQ(AsString(good.GetTuple(0)), "legitimate") << "the real page must be unaffected";

    // The disowned one must be refused rather than interpreted. Before N1 this
    // read its 0xDDDD slot_count as truth and indexed far past the page.
    Page disowned;
    ASSERT_NO_THROW(fm.ReadPage(1, disowned));
    EXPECT_THROW(disowned.ValidateOrThrow(), AxolotlDB::utils::CorruptPageException);
    EXPECT_THROW((void)disowned.GetTuple(0), AxolotlDB::utils::CorruptPageException);
}
