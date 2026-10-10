// P2 §2.4 — VectorHeap adopts the standard 24-byte PageHeader (N9 option A), so
// every data page has somewhere to put a page_lsn and there is ONE recovery
// path, not two. The usable region of a data page shrinks from PAGE_SIZE to
// PAGE_SIZE - sizeof(PageHeader).
//
// This is the discriminating test the roadmap points at: it fails against the
// old headerless layout (usable == PAGE_SIZE) and passes against the new one
// (usable == PAGE_SIZE - 24). No new StorageManager API is needed; VectorHeap's
// public surface is unchanged, only its per-page arithmetic.
//
// ALSO update the two existing tests in vector_heap_test.cpp deliberately, as
// §4.4 requires — their premises change with the new usable region:
//   * RecordRollsOntoTheNextPageAtTheBoundary (dim 300, stride 1200): still 3
//     per page since 3*1200=3600 <= 4072, but the boundary comment is now wrong.
//   * VectorExactlyOnePageWide (dim 1024, stride 4096): 4096 > 4072, so it is no
//     longer the "small path, page_span == 1" case — it now spans TWO pages.
// Do not touch those until you can state the new arithmetic.

#include <gtest/gtest.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/vector_heap.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

constexpr HeapId kHeapId = 7;
// The whole point of §2.4: a data page can hold this many bytes of record, not
// PAGE_SIZE. If this static_assert or the counts below are wrong, the header
// did not actually land.
constexpr uint32_t kUsable = PAGE_SIZE - static_cast<uint32_t>(sizeof(PageHeader));
static_assert(sizeof(PageHeader) == 24, "requires the §2.3 24-byte header first");

std::vector<float> Ramp(uint32_t dim, float base) {
    std::vector<float> v(dim);
    for (uint32_t i = 0; i < dim; ++i)
        v[i] = base + static_cast<float>(i);
    return v;
}

struct TempFile {
    std::filesystem::path p;
    explicit TempFile(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            (std::string("axo_vh_") + tag + "_" +
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

TEST(VectorHeapHeaderTest, SmallRecordsPackAtReducedUsableRegion) {
    // dim 340 → stride 1360 bytes.
    //   old (usable 4096): floor(4096/1360) = 3 per page.
    //   new (usable 4072): floor(4072/1360) = 2 per page  (3*1360 = 4080 > 4072).
    // Put 6 records → new layout needs 3 data pages, old needed 2. Plus the
    // header page (page 0), the file must be 4 pages, not 3.
    constexpr uint32_t kDim = 340;
    ASSERT_EQ(kUsable / (kDim * sizeof(float)), 2u)
        << "test assumes 2 records per page under the new usable region";

    TempFile f("pack");
    {
        VectorHeap vh(f.str(), kHeapId, kDim);
        std::vector<VectorId> ids;
        for (int i = 0; i < 6; ++i)
            ids.push_back(vh.Put(Ramp(kDim, i * 1000.0f)));
        vh.Sync();
        // Everything still round-trips through the new arithmetic.
        for (int i = 0; i < 6; ++i) {
            const auto got = vh.Get(ids[i]);
            ASSERT_EQ(got.size(), kDim);
            EXPECT_FLOAT_EQ(got[0], i * 1000.0f);
        }
    }
    // Inspect the raw file: 1 header page + 3 data pages.
    FileManager fm(f.str());
    EXPECT_EQ(fm.PageCount(), 4) << "with a 24-byte header only 2 of these records fit per page";
}

TEST(VectorHeapHeaderTest, VectorFillingOldPageNowSpansTwoPages) {
    // dim 1024 → stride 4096 bytes == old PAGE_SIZE exactly. Under the old
    // layout this was one page (page_span 1); under the new header it no longer
    // fits (4096 > 4072) and must span two pages.
    constexpr uint32_t kDim = PAGE_SIZE / sizeof(float);  // 1024
    ASSERT_GT(kDim * sizeof(float), kUsable);

    TempFile f("span");
    {
        VectorHeap vh(f.str(), kHeapId, kDim);
        const VectorId a = vh.Put(Ramp(kDim, 0.0f));
        const VectorId b = vh.Put(Ramp(kDim, 5000.0f));
        vh.Sync();
        const auto ga = vh.Get(a);
        const auto gb = vh.Get(b);
        ASSERT_EQ(ga.size(), kDim);
        ASSERT_EQ(gb.size(), kDim);
        EXPECT_FLOAT_EQ(ga[0], 0.0f);
        EXPECT_FLOAT_EQ(ga[kDim - 1], static_cast<float>(kDim - 1));
        EXPECT_FLOAT_EQ(gb[0], 5000.0f);
    }
    // Two records, each spanning 2 data pages → 4 data pages + 1 header = 5.
    FileManager fm(f.str());
    EXPECT_EQ(fm.PageCount(), 5)
        << "a PAGE_SIZE-wide record no longer fits one page under the new header";
}
