// P2 Gate — the whole phase in one test. It must FAIL before P2 is built and
// pass after. See docs/phases/P2.md#gate.
//
//   * CREATE TABLE through the Catalog
//   * 10 000 rows inserted through TableHeap
//   * every object closed, then reopened from the same files
//   * full scan returns all 10 000 IN RID ORDER
//   * pool sized at one-tenth the page count, so eviction is real
//   * deleted rows' space is reused (B7)
//   * a dropped-then-recreated vector column gets a DIFFERENT HeapId
//
// Uses the assumed StorageManager / Catalog / TableHeap / PageGuard API
// documented in the corresponding unit tests.

#include <catalog/catalog.h>
#include <gtest/gtest.h>
#include <storage/file_io.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <storage/vector_heap.h>
#include <utils/errors.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace AxolotlDB;
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::RID;
using AxolotlDB::storage::Schema;
using AxolotlDB::storage::Value;

namespace {

constexpr int kRows = 10000;

Schema RowSchema() {
    return Schema{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR}}};
}

std::vector<std::byte> Row(int i) {
    return storage::Tuple::Serialize(RowSchema(),
                                     {Value::Int32(i), Value::Varchar("row-" + std::to_string(i))});
}

bool RidLess(RID a, RID b) {
    return a.page_id != b.page_id ? a.page_id < b.page_id : a.slot_idx < b.slot_idx;
}

struct CountingFileIO final : storage::IFileIO {
    explicit CountingFileIO(const std::string& path) : fm_(path) {}

    storage::PageId AllocatePage() override {
        return fm_.AllocatePage();
    }
    void ReadPage(storage::PageId id, storage::Page& out) override {
        ++read_calls;
        fm_.ReadPage(id, out);
    }
    void WritePage(storage::PageId id, const storage::Page& p) override {
        ++write_calls;
        fm_.WritePage(id, p);
    }
    storage::PageId PageCount() const override {
        return fm_.PageCount();
    }
    void Sync() override {
        ++sync_calls;
        fm_.Sync();
    }

    storage::FileManager fm_;
    std::atomic<std::size_t> read_calls{0};
    std::atomic<std::size_t> write_calls{0};
    std::atomic<std::size_t> sync_calls{0};
};

struct IORegistry {
    std::map<std::string, CountingFileIO*> by_path;
    storage::FileIOFactory factory() {
        return [this](const std::string& path) {
            auto io = std::make_unique<CountingFileIO>(path);
            by_path[path] = io.get();
            return io;
        };
    }
};

struct Gate : ::testing::Test {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    void SetUp() override {
        orig_dir = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
              ("axo_gate_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

using StorageServicesGate = Gate;

TEST_F(StorageServicesGate, TenThousandRowsSurviveReopenUnderEviction) {
    // ── Phase 1: create + fill, with a generous pool, then flush and close. ──
    catalog::TableId tid = 0;
    {
        storage::StorageManager sm(/*pool_size=*/256);
        catalog::Catalog cat(sm);
        tid = cat.CreateTable("t", RowSchema());
        storage::TableHeap& th = cat.GetTableHeap(tid);
        for (int i = 0; i < kRows; ++i) {
            (void)th.Insert(Row(i));
        }
        sm.FlushAll();
    }

    // ── Phase 2: compute exact page count and size pool ──
    const auto table_path = dir / ("table_" + std::to_string(tid) + ".db");
    ASSERT_TRUE(std::filesystem::exists(table_path)) << "Table file must exist on disk";
    const std::size_t file_bytes = std::filesystem::file_size(table_path);
    const std::size_t page_count = file_bytes / storage::PAGE_SIZE;
    ASSERT_GT(page_count, 0u) << "10,000 rows must span at least 1 page";

    // Pool sized at exactly one-tenth the page count (computed, not hardcoded).
    const std::size_t pool_size = std::max<std::size_t>(1, page_count / 10);

    // ── Phase 3: reopen with pool, verify full scan in RID order and eviction proof ──
    std::vector<RID> seen;
    {
        IORegistry reg;
        storage::StorageManager sm(pool_size, reg.factory());

        catalog::Catalog cat(sm);
        const auto reopened_tid = cat.GetTableId("t");
        EXPECT_EQ(reopened_tid, tid);
        storage::TableHeap& th = cat.GetTableHeap(reopened_tid);

        CountingFileIO* table_io = reg.by_path.at("table_" + std::to_string(tid) + ".db");

        seen.reserve(kRows);
        for (RID r : th)
            seen.push_back(r);

        EXPECT_EQ(seen.size(), static_cast<std::size_t>(kRows))
            << "every inserted row must survive close + reopen under eviction";
        for (std::size_t i = 1; i < seen.size(); ++i)
            ASSERT_TRUE(RidLess(seen[i - 1], seen[i]))
                << "the full scan must return rows in RID order";

        // Spot-check a row's payload round-trips through the reopened heap.
        if (!seen.empty()) {
            storage::ReadPageGuard g;
            const auto bytes = th.Get(seen.front(), g);
            const auto vals = storage::Tuple::Deserialize(RowSchema(), bytes);
            EXPECT_EQ(vals.at(0).AsInt32(), 0);
        }

        EXPECT_GT(table_io->read_calls.load(), page_count)
            << "Scan must re-fetch at least one page that was evicted and "
               "re-read, proving eviction under this pool size — otherwise "
               "the 1/10th sizing is decorative";

        // ── Phase 4: B7 assertion — deleted rows' space reused by subsequent inserts ──
        sm.FlushAll();
        const std::size_t pages_before_b7 =
            std::filesystem::file_size(table_path) / storage::PAGE_SIZE;

        constexpr int kDeleteCount = 50;
        constexpr int kReinsertCount = 30;
        ASSERT_GE(seen.size(), static_cast<std::size_t>(kDeleteCount));

        // Delete 50 rows from the end of the table and record their page_ids
        std::set<storage::PageId> freed_pages;
        const std::size_t start_delete_idx = seen.size() - kDeleteCount;
        for (std::size_t i = start_delete_idx; i < seen.size(); ++i) {
            freed_pages.insert(seen[i].page_id);
            th.Delete(seen[i]);
        }
        ASSERT_FALSE(freed_pages.empty());

        // Re-insert rows into reclaimed space
        std::vector<RID> reinserted_rids;
        reinserted_rids.reserve(kReinsertCount);
        for (int i = 0; i < kReinsertCount; ++i) {
            const RID new_rid = th.Insert(Row(i));
            ASSERT_TRUE(new_rid.IsValid());
            reinserted_rids.push_back(new_rid);
        }

        // Positional B7 assertion: every reinserted row must land on a page freed by the deletes
        for (const auto& new_rid : reinserted_rids) {
            EXPECT_TRUE(freed_pages.count(new_rid.page_id) > 0)
                << "Reinserted row landed on page " << new_rid.page_id
                << " which was not one of the pages freed by the prior "
                   "deletes -- B7 reuse did not actually happen here, "
                   "the file just had unrelated slack";
        }

        sm.FlushAll();
        const std::size_t pages_after_b7 =
            std::filesystem::file_size(table_path) / storage::PAGE_SIZE;

        EXPECT_EQ(pages_after_b7, pages_before_b7)
            << "B7 gate: deleted rows' space must be reused instead of growing the file";
    }
}

TEST_F(StorageServicesGate, DroppedThenRecreatedVectorColumnGetsDifferentHeapId) {
    const Schema vec{
        {ColumnDef{"id", ColumnType::INT32}, ColumnDef{"emb", ColumnType::VECTOR, false, 4}}};

    HeapId first = 0;
    {
        storage::StorageManager sm(64);
        catalog::Catalog cat(sm);
        const auto t1 = cat.CreateTable("v1", vec);
        first = cat.GetVectorHeap(t1, "emb").Id();
        cat.DropTable("v1");
        sm.FlushAll();
    }

    {
        storage::StorageManager sm(64);
        catalog::Catalog cat(sm);
        const auto t2 = cat.CreateTable("v2", vec);
        const HeapId second = cat.GetVectorHeap(t2, "emb").Id();

        EXPECT_NE(first, second)
            << "a dropped-then-recreated vector column must get a fresh HeapId (B5)";
        EXPECT_GT(second, first) << "HeapId counter must be strictly monotonic across reopen";
    }
}
