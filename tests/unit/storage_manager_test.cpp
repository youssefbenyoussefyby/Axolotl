// P2 §2.1 — StorageManager. TDD target for the N6 fix: ONE shared BufferPool
// across many files, addressed by a packed PageRef instead of a file-local
// PageId. Written before the code exists; it is expected to fail to compile
// until include/storage/storage_manager.h lands, then fail at runtime until the
// body is written. That "red first" is the P2 "demonstrably failed before the
// work" contract.
//
// ─── ASSUMED API — adjust the header to match, or adjust these tests ─────────
//
//   using FileId = uint32_t;
//   inline constexpr FileId INVALID_FILE_ID = 0;
//
//   struct PageRef {
//       FileId file_id = INVALID_FILE_ID;
//       PageId page_no = INVALID_PAGE_ID;
//       [[nodiscard]] uint64_t Key() const noexcept;        // file_id<<32 | u32(page_no)
//       [[nodiscard]] static PageRef FromKey(uint64_t) noexcept;
//       friend bool operator==(const PageRef&, const PageRef&) noexcept = default;
//   };
//
//   class StorageManager {
//     explicit StorageManager(std::size_t pool_size);
//     FileId OpenFile(const std::string& path);             // idempotent per path
//     void   CloseFile(FileId);                             // flush + drop its frames
//     ReadPageGuard  FetchRead(PageRef);
//     WritePageGuard FetchWrite(PageRef);
//     WritePageGuard NewPage(FileId, PageRef& out_ref);
//     void   FlushFile(FileId);
//     void   FlushAll();
//     [[nodiscard]] PageId      PageCount(FileId) const;
//     [[nodiscard]] std::size_t PoolCapacity() const noexcept;
//   };
//
// The flush-accounting, read-failure and shared-budget-by-injection tests below
// additionally assume an injectable per-file I/O seam (§2.1 last bullet, §6.4:
// "when StorageManager makes the seam natural — this is that moment"):
//
//   struct IFileIO {                       // include/storage/file_io.h (assumed)
//     virtual ~IFileIO() = default;
//     virtual PageId AllocatePage()                 = 0;
//     virtual void   ReadPage(PageId, Page&)        = 0;   // may throw IOException
//     virtual void   WritePage(PageId, const Page&) = 0;
//     virtual PageId PageCount() const              = 0;
//     virtual void   Sync()                         = 0;
//   };
//   using FileIOFactory = std::function<std::unique_ptr<IFileIO>(const std::string&)>;
//   StorageManager(std::size_t pool_size, FileIOFactory);  // extra ctor for tests
//
// If your seam differs, only the `Injected` fixture at the bottom changes.

#include <gtest/gtest.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <utils/errors.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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

// A unique temp path per test, cleaned up on destruction.
struct TempFile {
    std::filesystem::path p;
    explicit TempFile(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            (std::string("axo_sm_") + tag + "_" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db");
        std::filesystem::remove(p);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
    std::string str() const {
        return p.string();
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PageRef — the packed key. Pure value type, no I/O. This is the N6 bug turned
// into an assertion: a pool keyed by PageId alone cannot tell these two apart.
// ─────────────────────────────────────────────────────────────────────────────

TEST(StorageManagerTest, PageRefKeyPacksFileIdAndPageNo) {
    PageRef r{/*file_id=*/7, /*page_no=*/42};
    const uint64_t expected = (static_cast<uint64_t>(7) << 32) | static_cast<uint32_t>(42);
    EXPECT_EQ(r.Key(), expected);
}

TEST(StorageManagerTest, PageRefKeyRoundTrips) {
    for (PageRef r : {PageRef{1, 0}, PageRef{3, 999}, PageRef{0xABCD, 0x1234}}) {
        const PageRef back = PageRef::FromKey(r.Key());
        EXPECT_EQ(back, r) << "FromKey(Key()) must be the identity";
    }
}

TEST(StorageManagerTest, TwoFilesSamePageNoDoNotCollide) {
    // Packed keys must differ — a PageId-only table cannot tell these apart.
    const PageRef ka{/*file_id=*/1, /*page_no=*/5};
    const PageRef kb{/*file_id=*/2, /*page_no=*/5};
    EXPECT_NE(ka.Key(), kb.Key());
    EXPECT_FALSE(ka == kb);

    // Through the pool: two files' page 0 must not share a frame. A pool keyed
    // by PageId alone returns file A's bytes for a fetch against file B.
    TempFile f1("colA"), f2("colB");
    StorageManager sm(8);
    const FileId a = sm.OpenFile(f1.str());
    const FileId b = sm.OpenFile(f2.str());
    PageRef ra{}, rb{};
    {
        auto ga = sm.NewPage(a, ra);
        (*ga).InsertTuple(AsBytes("file-A"));
    }
    {
        auto gb = sm.NewPage(b, rb);
        (*gb).InsertTuple(AsBytes("file-B"));
    }
    ASSERT_EQ(ra.page_no, rb.page_no);
    ASSERT_NE(ra.file_id, rb.file_id);

    auto ga = sm.FetchRead(ra);
    auto gb = sm.FetchRead(rb);
    EXPECT_EQ(AsString((*ga).GetTuple(0)), "file-A");
    EXPECT_EQ(AsString((*gb).GetTuple(0)), "file-B")
        << "same page_no in two files must not collide in the shared pool";
}

// ─────────────────────────────────────────────────────────────────────────────
// File registry
// ─────────────────────────────────────────────────────────────────────────────

TEST(StorageManagerTest, OpenFileIsIdempotentForSamePath) {
    TempFile f("idem");
    StorageManager sm(/*pool_size=*/16);
    const FileId a = sm.OpenFile(f.str());
    const FileId b = sm.OpenFile(f.str());
    EXPECT_EQ(a, b) << "the same path must map to the same FileId for the process";
    EXPECT_NE(a, INVALID_FILE_ID);
}

TEST(StorageManagerTest, NewPageReturnsRefIntoRequestedFile) {
    TempFile f1("nf1"), f2("nf2");
    StorageManager sm(16);
    const FileId a = sm.OpenFile(f1.str());
    const FileId b = sm.OpenFile(f2.str());

    PageRef ra{}, rb{};
    {
        auto ga = sm.NewPage(a, ra);
        (void)ga;
    }
    {
        auto gb = sm.NewPage(b, rb);
        (void)gb;
    }

    EXPECT_EQ(ra.file_id, a);
    EXPECT_EQ(rb.file_id, b);
}

TEST(StorageManagerTest, FetchWriteMarksDirtyOnMutableAccess) {
    TempFile f("dirty");
    const std::string original = "payload-AAAA";
    const std::string mutated = "payload-BBBB";
    PageRef ref{};
    {
        StorageManager sm(16);
        const FileId fid = sm.OpenFile(f.str());
        {
            auto g = sm.NewPage(fid, ref);
            (*g).InsertTuple(AsBytes(original));
        }
        sm.FlushFile(fid);  // page is clean on disk; NewPage's dirty bit is gone

        {
            auto g = sm.FetchWrite(ref);  // the API this test is named for
            (*g).UpdateTupleInPlace(0, AsBytes(mutated));
        }
        sm.FlushFile(fid);
    }
    {
        StorageManager sm(16);
        const FileId fid = sm.OpenFile(f.str());
        auto g = sm.FetchRead(PageRef{fid, ref.page_no});
        EXPECT_EQ(AsString((*g).GetTuple(0)), mutated)
            << "FetchWrite mutable access must dirty the frame so FlushFile persists it";
    }
}

TEST(StorageManagerTest, CloseFileFlushesAndInvalidatesFrames) {
    TempFile f("close");
    const std::string payload = "flushed-on-close";
    PageRef ref{};
    {
        StorageManager sm(16);
        const FileId fid = sm.OpenFile(f.str());
        auto g = sm.NewPage(fid, ref);
        (*g).InsertTuple(AsBytes(payload));
        g.Drop();           // release the pin so CloseFile can flush it
        sm.CloseFile(fid);  // must flush the dirty page, then drop frames

        EXPECT_THROW((void)sm.FetchRead(ref), utils::InvalidPageException)
            << "CloseFile must invalidate the FileId so later fetches cannot hit stale frames";
        PageRef dead{};
        EXPECT_THROW((void)sm.NewPage(fid, dead), utils::InvalidPageException);
    }
    {
        StorageManager sm(16);
        const FileId fid = sm.OpenFile(f.str());
        auto g = sm.FetchRead(PageRef{fid, ref.page_no});
        EXPECT_EQ(AsString((*g).GetTuple(0)), payload)
            << "CloseFile must flush before invalidating frames";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The actual N6 fix: one shared frame budget across every registered file.
// ─────────────────────────────────────────────────────────────────────────────

TEST(StorageManagerTest, PoolBudgetIsSharedAcrossFiles) {
    TempFile fa("budA"), fb("budB");
    StorageManager sm(/*pool_size=*/4);
    const FileId a = sm.OpenFile(fa.str());
    const FileId b = sm.OpenFile(fb.str());
    ASSERT_EQ(sm.PoolCapacity(), 4u);

    // Pin all four frames against file A and keep the guards alive.
    std::vector<WritePageGuard> held;
    for (int i = 0; i < 4; ++i) {
        PageRef r{};
        held.push_back(sm.NewPage(a, r));
    }

    // A per-file budget would give file B its own frames and this would
    // succeed. One shared budget is exhausted by A's four pins, so it throws.
    PageRef rb{};
    EXPECT_THROW((void)sm.NewPage(b, rb), utils::BufferPoolExhaustedException)
        << "frames must come from a single shared budget, not one pool per file";

    // Drop A's pins: B must be able to steal those unpinned frames (cross-file
    // eviction). A partitioned per-file quota would still refuse B here.
    held.clear();
    EXPECT_NO_THROW((void)sm.NewPage(b, rb))
        << "file B must evict file A's unpinned frames from the shared pool";
}

// ─────────────────────────────────────────────────────────────────────────────
// Injected I/O seam: flush accounting + read-failure. These pin the §2.1
// "make FileManager I/O injectable" item; the real FileManager cannot report
// Sync counts or fail a read on demand.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct CountingIO : IFileIO {
    std::map<PageId, std::array<std::byte, PAGE_SIZE>> pages;
    int sync_calls = 0;
    int writes = 0;
    bool fail_next_read = false;

    PageId AllocatePage() override {
        const PageId id = static_cast<PageId>(pages.size());
        pages[id];  // zero-filled
        return id;
    }
    void ReadPage(PageId id, Page& out) override {
        if (fail_next_read) {
            fail_next_read = false;
            throw utils::IOException("injected read failure");
        }
        out = Page(pages.at(id));
    }
    void WritePage(PageId id, const Page& p) override {
        ++writes;
        pages[id] = p.RawData();
    }
    PageId PageCount() const override {
        return static_cast<PageId>(pages.size());
    }
    void Sync() override {
        ++sync_calls;
    }
};

// A factory that hands back CountingIO objects and keeps a raw pointer to each
// so the test can read its counters. StorageManager owns the unique_ptrs.
struct IORegistry {
    std::map<std::string, CountingIO*> by_path;
    FileIOFactory factory() {
        return [this](const std::string& path) {
            auto io = std::make_unique<CountingIO>();
            by_path[path] = io.get();
            return std::unique_ptr<IFileIO>(std::move(io));
        };
    }
};

}  // namespace

TEST(StorageManagerTest, FlushFileSyncsOnlyThatFile) {
    IORegistry reg;
    StorageManager sm(/*pool_size=*/16, reg.factory());
    const FileId a = sm.OpenFile("mem://a");
    const FileId b = sm.OpenFile("mem://b");

    PageRef ra{}, rb{};
    {
        auto g = sm.NewPage(a, ra);
        (*g).InsertTuple(AsBytes("a"));
    }
    {
        auto g = sm.NewPage(b, rb);
        (*g).InsertTuple(AsBytes("b"));
    }

    sm.FlushFile(a);
    EXPECT_GE(reg.by_path.at("mem://a")->sync_calls, 1);
    EXPECT_EQ(reg.by_path.at("mem://b")->sync_calls, 0) << "FlushFile(a) must not touch b";
}

TEST(StorageManagerTest, FlushAllSyncsEachFileExactlyOnce) {
    IORegistry reg;
    StorageManager sm(16, reg.factory());
    const FileId a = sm.OpenFile("mem://a");
    const FileId b = sm.OpenFile("mem://b");
    PageRef ra{}, rb{};
    {
        auto g = sm.NewPage(a, ra);
        (*g).InsertTuple(AsBytes("a"));
    }
    {
        auto g = sm.NewPage(b, rb);
        (*g).InsertTuple(AsBytes("b"));
    }

    sm.FlushAll();
    EXPECT_EQ(reg.by_path.at("mem://a")->sync_calls, 1);
    EXPECT_EQ(reg.by_path.at("mem://b")->sync_calls, 1);
}

TEST(StorageManagerTest, ReadFailurePropagatesWithoutLeakingFrames) {
    IORegistry reg;
    StorageManager sm(/*pool_size=*/4, reg.factory());
    const FileId a = sm.OpenFile("mem://a");

    PageRef ref{};
    {
        auto g = sm.NewPage(a, ref);
        (*g).InsertTuple(AsBytes("x"));
    }
    sm.FlushFile(a);

    // Evict ref from the 4-frame pool by allocating 4 dummy pages
    for (int i = 0; i < 4; ++i) {
        PageRef dummy{};
        auto g = sm.NewPage(a, dummy);
    }

    // Force the next physical read to fail; the fetch must throw and must NOT
    // leave the target frame pinned.
    reg.by_path.at("mem://a")->fail_next_read = true;
    EXPECT_THROW((void)sm.FetchRead(ref), utils::IOException);

    // If the failed fetch leaked a pin, filling the pool would exhaust one frame
    // early. All four frames must still be reusable.
    std::vector<WritePageGuard> held;
    for (int i = 0; i < 4; ++i) {
        PageRef r{};
        EXPECT_NO_THROW(held.push_back(sm.NewPage(a, r)))
            << "a failed read leaked a pinned frame (iteration " << i << ")";
    }
}
