// P2 §2.7 — the WAL *contract*, not the implementation. ILogManager + a
// NullLogManager, and the WAL rule living inside the BufferPool: consult the log
// before every page write. This is what makes P5 a drop-in — write the test now
// even though the null implementation satisfies it trivially.
//
// ─── ASSUMED API — include/storage/log_manager.h ────────────────────────────
//
//   class ILogManager {
//    public:
//     virtual ~ILogManager() = default;
//     virtual void     FlushUpTo(uint64_t lsn) = 0;   // durably persist log <= lsn
//     [[nodiscard]] virtual uint64_t DurableLsn() const = 0;
//   };
//   class NullLogManager : public ILogManager {
//     void FlushUpTo(uint64_t) override {}
//     uint64_t DurableLsn() const override { return 0; }
//   };
//
// And BufferPool gains an ILogManager it consults before writing a dirty page:
//
//   explicit BufferPool(std::size_t pool_size, FileManager* fm,
//                       ILogManager* log = /* shared NullLogManager */ nullptr);

#include <gtest/gtest.h>
#include <storage/log_manager.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

std::span<const std::byte> AsBytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

struct TempFile {
    std::filesystem::path p;
    explicit TempFile(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            (std::string("axo_log_") + tag + "_" +
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

// Records every FlushUpTo call so the test can prove the manager consulted it.
struct SpyLog : ILogManager {
    std::vector<uint64_t> flushed;
    uint64_t durable = 0;
    void FlushUpTo(uint64_t lsn) override {
        flushed.push_back(lsn);
        durable = lsn;
    }
    uint64_t DurableLsn() const override {
        return durable;
    }
};

}  // namespace

TEST(LogManagerTest, NullManagerReportsZeroDurableLsn) {
    NullLogManager null;
    EXPECT_EQ(null.DurableLsn(), 0u);
    EXPECT_NO_THROW(null.FlushUpTo(123));  // a no-op, never meaningfully called
    EXPECT_EQ(null.DurableLsn(), 0u);
}

TEST(BufferPoolTest, ConsultsLogManagerBeforeWritingPage) {
    // WAL rule: a dirty page carrying page_lsn = L must not be written to disk
    // until the log is durable up to at least L. The rule lives in the storage manager,
    // so a spy attached to the manager sees FlushUpTo before flush.
    TempFile f("wal");
    SpyLog spy;
    StorageManager sm(/*pool_size=*/4, &spy);
    const FileId fid = sm.OpenFile(f.str());

    PageRef ref{};
    {
        auto guard = sm.NewPage(fid, ref);
        (*guard).InsertTuple(AsBytes("write-ahead"));
        (*guard).SetPageLSN(42);  // this page's changes are logged at LSN 42
    }

    ASSERT_TRUE(spy.flushed.empty()) << "nothing should be flushed before the write";
    sm.FlushFile(fid);  // forces the dirty write-back

    ASSERT_FALSE(spy.flushed.empty())
        << "the manager must consult the log before writing a dirty page";
    EXPECT_GE(spy.flushed.back(), 42u)
        << "the log must be durable up to at least the page's LSN before its write";
}

TEST(BufferPoolTest, ConsultsLogManagerOnEvictionOfDirtyPage) {
    TempFile f("wal_evict");
    SpyLog spy;
    StorageManager sm(/*pool_size=*/2, &spy);
    const FileId fid = sm.OpenFile(f.str());

    PageRef p0{};
    {
        auto g0 = sm.NewPage(fid, p0);
        (*g0).InsertTuple(AsBytes("p0"));
        (*g0).SetPageLSN(100);
    }

    // Allocate 2 more pages to force eviction of p0 from 2-frame pool
    PageRef p1{}, p2{};
    { auto g1 = sm.NewPage(fid, p1); }
    { auto g2 = sm.NewPage(fid, p2); }

    ASSERT_FALSE(spy.flushed.empty()) << "the manager must consult the log on dirty page eviction";
    EXPECT_GE(spy.flushed.back(), 100u);
}
