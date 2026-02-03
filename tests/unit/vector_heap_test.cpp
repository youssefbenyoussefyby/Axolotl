// Tests for VectorHeap -- the B2 answer (docs/audit.md): a durable,
// append-only store for fixed-dimension float vectors, one heap per
// (table, vector column).
//
// Why this component exists: a 1536-dim float32 embedding is 6144 bytes,
// larger than an entire 4096-byte Page. Page::InsertTuple throws
// PageFullException and there is no overflow mechanism, so the row store
// physically cannot hold the vectors this database exists to store. See
// audit finding B2.
//
// What these tests pin down, and why each matters:
//
//   * The two addressing regimes. VectorHeap's whole design rests on
//     LocateVector mapping a VectorId to (page, offset, span) by arithmetic
//     alone -- no per-record pointer is ever stored. Records are packed
//     several-per-page when stride <= PAGE_SIZE and span whole pages when it
//     doesn't. Both regimes need independent proof, plus the boundaries
//     between them.
//   * Page rollover. The record that lands exactly at a page boundary, and
//     the one immediately after it, are where off-by-one errors in
//     records_per_page live.
//   * Persistence. The heap is only useful if a VectorId still resolves to
//     the same bytes after every in-process object is destroyed. That covers
//     both the vector data and the header's vector_count -- if the count is
//     lost, the next Put() hands out an id that aliases an existing record
//     and silently overwrites it.
//   * Construction contracts. A dimension mismatch against an existing file
//     must throw rather than silently misread every record, the same
//     fail-fast discipline as audit finding A6.

#include <gtest/gtest.h>
#include <storage/vector_heap.h>
#include <utils/errors.h>

#include <cstdint>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

// Catalog-assigned heap identity. Every heap in these tests uses this one
// unless the test is specifically about provenance, so a stray mismatch
// stands out.
constexpr HeapId kHeapId = 7;

// v[i] = base + i. A positional pattern, so a record that comes back as its
// neighbour, or shifted by a few floats, fails loudly instead of looking
// plausible.
std::vector<float> Ramp(uint32_t dimension, float base) {
    std::vector<float> v(dimension);
    std::iota(v.begin(), v.end(), base);
    return v;
}

void ExpectRamp(const std::vector<float>& got, uint32_t dimension, float base,
                const std::string& what) {
    ASSERT_EQ(got.size(), dimension) << what << ": wrong length";
    for (uint32_t i = 0; i < dimension; ++i) {
        ASSERT_FLOAT_EQ(got[i], base + static_cast<float>(i))
            << what << ": mismatch at index " << i;
    }
}

}  // namespace

class VectorHeapTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path = std::string("test_vh_") + info->name() + ".db";
        std::filesystem::remove(path);
    }

    void TearDown() override {
        std::filesystem::remove(path);
    }

    std::string path;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, ZeroDimensionThrows) {
    EXPECT_THROW(VectorHeap(path, kHeapId, 0), std::invalid_argument);
}

TEST_F(VectorHeapTest, ZeroDimensionDoesNotCreateTheFile) {
    // The dimension check runs as an argument to file_manager_'s own
    // constructor, so it fires before FileManager can open-or-create the
    // path. An invalid call must leave nothing behind on disk.
    EXPECT_THROW(VectorHeap(path, kHeapId, 0), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path))
        << "a rejected construction still created the heap file";
}

TEST_F(VectorHeapTest, FreshHeapIsEmptyAndRemembersItsDimension) {
    VectorHeap vh(path, kHeapId, 128);
    EXPECT_EQ(vh.Count(), 0u);
    EXPECT_EQ(vh.Dimension(), 128u);
}

TEST_F(VectorHeapTest, ReopenWithMatchingDimensionSucceeds) {
    { VectorHeap vh(path, kHeapId, 64); }
    VectorHeap reopened(path, kHeapId, 64);
    EXPECT_EQ(reopened.Dimension(), 64u);
    EXPECT_EQ(reopened.Count(), 0u);
}

TEST_F(VectorHeapTest, ReopenWithWrongDimensionThrows) {
    // The A6 lesson applied to this component: the stored header is the
    // authority, and disagreeing with it is a hard error rather than a
    // silent misread of every record in the file.
    { VectorHeap vh(path, kHeapId, 64); }
    EXPECT_THROW(VectorHeap(path, kHeapId, 128), std::invalid_argument);
    EXPECT_THROW(VectorHeap(path, kHeapId, 63), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Small vectors: stride <= PAGE_SIZE, several records share a page.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, SmallVectorRoundTrip) {
    VectorHeap vh(path, kHeapId, 4);
    const VectorId id = vh.Put(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f});

    const auto back = vh.Get(id);
    ASSERT_EQ(back.size(), 4u);
    EXPECT_FLOAT_EQ(back[0], 1.0f);
    EXPECT_FLOAT_EQ(back[1], 2.0f);
    EXPECT_FLOAT_EQ(back[2], 3.0f);
    EXPECT_FLOAT_EQ(back[3], 4.0f);
}

TEST_F(VectorHeapTest, PutReturnsSequentialIdsAndAdvancesCount) {
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_EQ(vh.Count(), 0u);

    for (uint64_t expected = 0; expected < 5; ++expected) {
        const VectorId id = vh.Put(Ramp(8, static_cast<float>(expected) * 100.0f));
        EXPECT_EQ(id, expected) << "ids must be dense and monotonic";
        EXPECT_EQ(vh.Count(), expected + 1);
    }
}

TEST_F(VectorHeapTest, WrongSizedVectorThrows) {
    using AxolotlDB::utils::VectorDimensionMismatchException;
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_THROW((void)vh.Put(Ramp(7, 0.0f)), VectorDimensionMismatchException);
    EXPECT_THROW((void)vh.Put(Ramp(9, 0.0f)), VectorDimensionMismatchException);
    EXPECT_THROW((void)vh.Put(std::vector<float>{}), VectorDimensionMismatchException);
    EXPECT_EQ(vh.Count(), 0u) << "a rejected Put must not consume an id";
}

TEST_F(VectorHeapTest, ManyVectorsSharingOnePageAllSurvive) {
    // dimension 300 -> stride 1200 bytes -> 3 records per 4096-byte page,
    // with 496 bytes of tail deliberately wasted. A stride that does NOT
    // divide PAGE_SIZE evenly is the case where offset arithmetic is most
    // likely to drift into a neighbouring record.
    constexpr uint32_t kDim = 300;
    VectorHeap vh(path, kHeapId, kDim);

    std::vector<VectorId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(vh.Put(Ramp(kDim, static_cast<float>(i) * 1000.0f)));
    }

    for (int i = 0; i < 3; ++i) {
        ExpectRamp(vh.Get(ids[i]), kDim, static_cast<float>(i) * 1000.0f,
                   "record " + std::to_string(i) + " sharing page 1");
    }
}

TEST_F(VectorHeapTest, RecordRollsOntoTheNextPageAtTheBoundary) {
    // THE small-vector boundary test. With 3 records per page, record 2 is
    // the last one page 1 can hold and record 3 is the first that must land
    // on page 2 at offset 0. An off-by-one in records_per_page shows up here
    // as record 3 either overwriting record 0 or reading back as it.
    constexpr uint32_t kDim = 300;  // stride 1200, 3 per page
    VectorHeap vh(path, kHeapId, kDim);

    std::vector<VectorId> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(vh.Put(Ramp(kDim, static_cast<float>(i) * 1000.0f)));
    }

    // Every record still reads back as itself -- including record 0, which a
    // wrongly-computed record 3 would have clobbered.
    for (int i = 0; i < 4; ++i) {
        ExpectRamp(vh.Get(ids[i]), kDim, static_cast<float>(i) * 1000.0f,
                   "record " + std::to_string(i) + " across the page boundary");
    }
}

TEST_F(VectorHeapTest, VectorExactlyOnePageWide) {
    // stride == useable_page_size exactly (1018 floats * 4 bytes = 4072 bytes).
    // This is the seam between the two addressing regimes: it is still the
    // "small" path (page_span == 1) but fills its page completely, so every
    // record starts at offset 0 on a page of its own.
    constexpr uint32_t kDim = (PAGE_SIZE - sizeof(PageHeader)) / sizeof(float);  // 1018
    VectorHeap vh(path, kHeapId, kDim);

    const VectorId a = vh.Put(Ramp(kDim, 0.0f));
    const VectorId b = vh.Put(Ramp(kDim, 5000.0f));

    ExpectRamp(vh.Get(a), kDim, 0.0f, "exact-page-width record a");
    ExpectRamp(vh.Get(b), kDim, 5000.0f, "exact-page-width record b");
}

// ─────────────────────────────────────────────────────────────────────────────
// Large vectors: stride > PAGE_SIZE, one record spans multiple whole pages.
// This is the case B2 is actually about -- the one the row store cannot do.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, LargeVectorSpanningTwoPagesRoundTrips) {
    // 1536 floats = 6144 bytes = the exact size the audit cites as larger
    // than an entire page. Spans 2 pages with the second only partly used.
    constexpr uint32_t kDim = 1536;
    VectorHeap vh(path, kHeapId, kDim);

    const VectorId id = vh.Put(Ramp(kDim, 0.0f));
    ExpectRamp(vh.Get(id), kDim, 0.0f, "1536-dim record");
}

TEST_F(VectorHeapTest, LargeVectorSpanningThreePagesRoundTrips) {
    // 3000 floats = 12000 bytes -> 3 pages, last one partial. More than two
    // pages means the middle chunk is neither the first nor the last, which
    // is where a loop that special-cases only its endpoints breaks.
    constexpr uint32_t kDim = 3000;
    VectorHeap vh(path, kHeapId, kDim);

    const VectorId id = vh.Put(Ramp(kDim, 0.0f));
    ExpectRamp(vh.Get(id), kDim, 0.0f, "3000-dim record");
}

TEST_F(VectorHeapTest, ConsecutiveLargeVectorsDoNotCollide) {
    // Each record claims page_span whole pages. If the per-record page stride
    // is wrong, record 1 lands on top of record 0's second page and both come
    // back corrupted.
    constexpr uint32_t kDim = 1536;
    VectorHeap vh(path, kHeapId, kDim);

    const VectorId a = vh.Put(Ramp(kDim, 0.0f));
    const VectorId b = vh.Put(Ramp(kDim, 10000.0f));
    const VectorId c = vh.Put(Ramp(kDim, 20000.0f));

    ExpectRamp(vh.Get(a), kDim, 0.0f, "large record a");
    ExpectRamp(vh.Get(b), kDim, 10000.0f, "large record b");
    ExpectRamp(vh.Get(c), kDim, 20000.0f, "large record c");
    EXPECT_EQ(vh.Count(), 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence -- the property the whole component exists for.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, SmallVectorsSurviveReopen) {
    constexpr uint32_t kDim = 300;  // 3 per page, spills onto a second page
    constexpr int kCount = 7;

    {
        VectorHeap vh(path, kHeapId, kDim);
        for (int i = 0; i < kCount; ++i) {
            ASSERT_EQ(vh.Put(Ramp(kDim, static_cast<float>(i) * 1000.0f)),
                      static_cast<VectorId>(i));
        }
        vh.Sync();  // commit: ids are not permanent until this returns
    }  // every in-process object destroyed

    VectorHeap reopened(path, kHeapId, kDim);
    ASSERT_EQ(reopened.Count(), static_cast<std::size_t>(kCount))
        << "vector_count was not recovered from the stored header";
    for (int i = 0; i < kCount; ++i) {
        ExpectRamp(reopened.Get(static_cast<VectorId>(i)), kDim, static_cast<float>(i) * 1000.0f,
                   "reopened record " + std::to_string(i));
    }
}

TEST_F(VectorHeapTest, LargeVectorsSurviveReopen) {
    constexpr uint32_t kDim = 1536;
    constexpr int kCount = 3;

    {
        VectorHeap vh(path, kHeapId, kDim);
        for (int i = 0; i < kCount; ++i) {
            (void)vh.Put(Ramp(kDim, static_cast<float>(i) * 10000.0f));
        }
        vh.Sync();
    }

    VectorHeap reopened(path, kHeapId, kDim);
    ASSERT_EQ(reopened.Count(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ExpectRamp(reopened.Get(static_cast<VectorId>(i)), kDim, static_cast<float>(i) * 10000.0f,
                   "reopened large record " + std::to_string(i));
    }
}

TEST_F(VectorHeapTest, PutAfterReopenAppendsRatherThanOverwrites) {
    // The failure this guards against is silent and total: if vector_count_
    // came back as 0 on reopen, the next Put() would hand out VectorId 0 --
    // an id that already points at a stored record -- and overwrite it while
    // reporting success.
    constexpr uint32_t kDim = 300;  // 3 per page

    {
        VectorHeap vh(path, kHeapId, kDim);
        (void)vh.Put(Ramp(kDim, 0.0f));
        (void)vh.Put(Ramp(kDim, 1000.0f));
        vh.Sync();
    }

    {
        VectorHeap vh(path, kHeapId, kDim);
        ASSERT_EQ(vh.Count(), 2u);
        const VectorId next = vh.Put(Ramp(kDim, 2000.0f));
        EXPECT_EQ(next, 2u) << "ids must continue past what is already stored";
        EXPECT_EQ(vh.Count(), 3u);
        vh.Sync();
    }

    // All three, including the two written before the restart.
    VectorHeap vh(path, kHeapId, kDim);
    ASSERT_EQ(vh.Count(), 3u);
    ExpectRamp(vh.Get(0), kDim, 0.0f, "pre-restart record 0");
    ExpectRamp(vh.Get(1), kDim, 1000.0f, "pre-restart record 1");
    ExpectRamp(vh.Get(2), kDim, 2000.0f, "post-restart record 2");
}

TEST_F(VectorHeapTest, PutAcrossRestartsFillsAPartiallyUsedPage) {
    // Reopening mid-page is the case where Put() must choose FetchPage over
    // NewPage: record 1 belongs at offset 1200 of a page that already exists
    // and already holds record 0. Getting that branch wrong either throws
    // (page out of range) or allocates a fresh page and strands record 0.
    constexpr uint32_t kDim = 300;  // 3 per page

    {
        VectorHeap vh(path, kHeapId, kDim);
        (void)vh.Put(Ramp(kDim, 0.0f));
        vh.Sync();
    }
    {
        VectorHeap vh(path, kHeapId, kDim);
        (void)vh.Put(Ramp(kDim, 1000.0f));
        vh.Sync();
    }
    {
        VectorHeap vh(path, kHeapId, kDim);
        (void)vh.Put(Ramp(kDim, 2000.0f));
        vh.Sync();
    }

    VectorHeap vh(path, kHeapId, kDim);
    ASSERT_EQ(vh.Count(), 3u);
    for (int i = 0; i < 3; ++i) {
        ExpectRamp(vh.Get(static_cast<VectorId>(i)), kDim, static_cast<float>(i) * 1000.0f,
                   "record " + std::to_string(i) + " written in its own session");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Buffer pool interaction.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, DataSurvivesEvictionUnderATinyPool) {
    // A pool far smaller than the page count forces real eviction during both
    // the write and the read pass. It also catches leaked pins: a heap that
    // never unpins exhausts a 3-frame pool within a few Puts.
    constexpr uint32_t kDim = 1024;  // exactly one page per record
    constexpr int kCount = 12;       // 12 data pages + 1 header page, pool of 3

    VectorHeap vh(path, kHeapId, kDim, /*pool_size=*/3);
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(vh.Put(Ramp(kDim, static_cast<float>(i) * 10000.0f)), static_cast<VectorId>(i))
            << "Put " << i << " failed -- a leaked pin exhausts a small pool";
    }

    // Read in reverse so the access pattern differs from the write pattern
    // and the LRU order actually has to evict.
    for (int i = kCount - 1; i >= 0; --i) {
        ExpectRamp(vh.Get(static_cast<VectorId>(i)), kDim, static_cast<float>(i) * 10000.0f,
                   "record " + std::to_string(i) + " after eviction");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error contracts.
//
// These matter beyond tidiness: an out-of-range id that is NOT rejected here
// runs the addressing arithmetic anyway and either reads whatever bytes live
// at the computed offset, or surfaces InvalidPageException from deep inside
// BufferPool -- an implementation detail leaking out in place of this
// component's own contract.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, GetOutOfRangeIdThrows) {
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 0.0f));

    EXPECT_THROW((void)vh.Get(1), AxolotlDB::utils::InvalidVectorIdException);
    EXPECT_THROW((void)vh.Get(99999), AxolotlDB::utils::InvalidVectorIdException);
}

TEST_F(VectorHeapTest, GetOnAnEmptyHeapThrows) {
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_THROW((void)vh.Get(0), AxolotlDB::utils::InvalidVectorIdException);
}

TEST_F(VectorHeapTest, WrongSizedVectorThrowsDimensionMismatch) {
    // The specific type matters: a caller distinguishing "you gave me the
    // wrong shape" from "that id doesn't exist" needs them to differ.
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_THROW((void)vh.Put(Ramp(7, 0.0f)), AxolotlDB::utils::VectorDimensionMismatchException);
}

TEST_F(VectorHeapTest, ErrorMessagesCarryTheNumbersThatMatter) {
    // A message naming only the failure kind forces a debugger session; one
    // carrying the operands usually ends the investigation on the spot.
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 0.0f));

    try {
        (void)vh.Get(42);
        FAIL() << "expected InvalidVectorIdException";
    } catch (const AxolotlDB::utils::InvalidVectorIdException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("42"), std::string::npos) << "message omits the bad id: " << msg;
        EXPECT_NE(msg.find('1'), std::string::npos) << "message omits the count: " << msg;
    }

    try {
        (void)vh.Put(Ramp(3, 0.0f));
        FAIL() << "expected VectorDimensionMismatchException";
    } catch (const AxolotlDB::utils::VectorDimensionMismatchException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("8"), std::string::npos) << "message omits expected: " << msg;
        EXPECT_NE(msg.find("3"), std::string::npos) << "message omits actual: " << msg;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Commit / rollback -- the orphaned-vector leak.
//
// The problem: Put() used to commit immediately. If the row referencing the
// new vector then failed to insert, the vector was durably stored, counted,
// and unreferenced forever. Retrying the insert allocated a *new* id, so the
// retry was correct but never reclaimed the abandoned record -- every failed
// insert grew the file permanently.
//
// The fix moves the commit point to Sync(): Put() writes the data pages but
// leaves the durable count alone, so an abandoned Put() is reclaimed either
// immediately by Rollback() or implicitly by a restart. Nothing is leaked and
// no free list is needed.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, PutIsVisibleLocallyButNotYetCommitted) {
    VectorHeap vh(path, kHeapId, 8);
    const VectorId id = vh.Put(Ramp(8, 1.0f));

    // Readable right away -- the caller needs the data back to build its row.
    ExpectRamp(vh.Get(id), 8, 1.0f, "uncommitted record");
    EXPECT_EQ(vh.Count(), 1u);
    EXPECT_EQ(vh.PendingCount(), 1u) << "Put must not commit on its own";

    vh.Sync();
    EXPECT_EQ(vh.PendingCount(), 0u) << "Sync must commit everything pending";
}

TEST_F(VectorHeapTest, RollbackReclaimsTheIdOfAnAbandonedPut) {
    VectorHeap vh(path, kHeapId, 8);
    const VectorId first = vh.Put(Ramp(8, 1.0f));
    ASSERT_EQ(first, 0u);

    // The row insert failed -- give the id back.
    vh.Rollback();
    EXPECT_EQ(vh.Count(), 0u);
    EXPECT_EQ(vh.PendingCount(), 0u);

    // The retry must reuse id 0, not leak it and move to 1.
    const VectorId retried = vh.Put(Ramp(8, 99.0f));
    EXPECT_EQ(retried, 0u) << "rolled-back id was leaked instead of reused";

    vh.Sync();
    ExpectRamp(vh.Get(0), 8, 99.0f,
               "retried record");  // the retry's data, not the abandoned attempt's
    EXPECT_EQ(vh.Count(), 1u) << "the abandoned Put still occupies a slot";
}

TEST_F(VectorHeapTest, RollbackAfterSyncKeepsCommittedRecords) {
    // Rollback must only discard what is pending -- committed records are
    // permanent and must survive it untouched.
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 1.0f));
    (void)vh.Put(Ramp(8, 2.0f));
    vh.Sync();

    (void)vh.Put(Ramp(8, 3.0f));
    ASSERT_EQ(vh.Count(), 3u);
    vh.Rollback();

    EXPECT_EQ(vh.Count(), 2u) << "Rollback discarded committed records";
    ExpectRamp(vh.Get(0), 8, 1.0f, "committed record 0");
    ExpectRamp(vh.Get(1), 8, 2.0f, "committed record 1");
    EXPECT_THROW((void)vh.Get(2), AxolotlDB::utils::InvalidVectorIdException);
}

TEST_F(VectorHeapTest, RollbackIsIdempotentAndSafeOnAFreshHeap) {
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_NO_THROW(vh.Rollback());
    EXPECT_NO_THROW(vh.Rollback());
    EXPECT_EQ(vh.Count(), 0u);

    const VectorId id = vh.Put(Ramp(8, 5.0f));
    EXPECT_EQ(id, 0u) << "rollback on an empty heap disturbed the id sequence";
}

TEST_F(VectorHeapTest, UncommittedPutIsInvisibleAfterRestart) {
    // The crash case. Put without Sync, drop the object, reopen: the record
    // must be gone and its id available again -- otherwise a process that
    // died mid-insert leaves an orphan behind forever.
    {
        VectorHeap vh(path, kHeapId, 8);
        (void)vh.Put(Ramp(8, 1.0f));
        (void)vh.Put(Ramp(8, 2.0f));
        // deliberately no Sync()
    }

    VectorHeap reopened(path, kHeapId, 8);
    EXPECT_EQ(reopened.Count(), 0u) << "an uncommitted Put survived a restart -- that is the leak";
    EXPECT_THROW((void)reopened.Get(0), AxolotlDB::utils::InvalidVectorIdException);

    const VectorId id = reopened.Put(Ramp(8, 7.0f));
    EXPECT_EQ(id, 0u) << "ids from an uncommitted session were not reclaimed";
}

TEST_F(VectorHeapTest, ReusedIdOverwritesTheAbandonedPagesCleanly) {
    // The subtle half of the fix. A rolled-back Put leaves its pages
    // allocated, so the file has grown even though the count rewound. Put()
    // must therefore fetch-and-overwrite those pages rather than allocating
    // fresh ones -- if it appended instead, the id would resolve to a page
    // that holds nothing.
    //
    // Large vectors make this bite hardest: each record claims whole pages,
    // so an abandoned one leaves a multi-page gap.
    constexpr uint32_t kDim = 1536;  // 2 pages per record

    VectorHeap vh(path, kHeapId, kDim);
    (void)vh.Put(Ramp(kDim, 1.0f));
    vh.Rollback();

    const VectorId reused = vh.Put(Ramp(kDim, 500.0f));
    ASSERT_EQ(reused, 0u);
    vh.Sync();

    ExpectRamp(vh.Get(reused), kDim, 500.0f, "record written onto abandoned pages");
}

TEST_F(VectorHeapTest, InterleavedCommitAndRollbackKeepsIdsDense) {
    // Ids must stay dense and gap-free across a realistic mix of committed
    // and abandoned inserts -- a gap would mean a leaked record.
    VectorHeap vh(path, kHeapId, 8);

    EXPECT_EQ(vh.Put(Ramp(8, 0.0f)), 0u);
    vh.Sync();

    EXPECT_EQ(vh.Put(Ramp(8, 1.0f)), 1u);
    vh.Rollback();  // abandoned

    EXPECT_EQ(vh.Put(Ramp(8, 2.0f)), 1u) << "id 1 was leaked";
    vh.Sync();

    EXPECT_EQ(vh.Put(Ramp(8, 3.0f)), 2u);
    vh.Rollback();  // abandoned

    EXPECT_EQ(vh.Put(Ramp(8, 4.0f)), 2u) << "id 2 was leaked";
    vh.Sync();

    EXPECT_EQ(vh.Count(), 3u);
    ExpectRamp(vh.Get(0), 8, 0.0f, "record 0");
    ExpectRamp(vh.Get(1), 8, 2.0f, "record 1");
    ExpectRamp(vh.Get(2), 8, 4.0f, "record 2");
}

TEST_F(VectorHeapTest, RepeatedAbandonedPutsGrowTheFileByAtMostOneRecord) {
    // The leak, measured on disk. Under the old commit-on-Put behaviour every
    // failed insert consumed a fresh record slot permanently, so N failures
    // cost N records. Now the first abandoned Put allocates its pages and
    // every later one reuses them, so the cost is bounded by a single record
    // no matter how many times the insert fails.
    //
    // Note the bound is one record, not zero: the pages an abandoned Put
    // allocated stay allocated (there is no truncate, and none is needed --
    // they get overwritten). What matters is that the growth does not scale
    // with the number of failures.
    constexpr uint32_t kDim =
        (PAGE_SIZE - sizeof(PageHeader)) / sizeof(float);  // exactly one page per record (1018)
    constexpr int kFailures = 20;

    VectorHeap vh(path, kHeapId, kDim);
    (void)vh.Put(Ramp(kDim, 0.0f));
    vh.Sync();
    const auto baseline = std::filesystem::file_size(path);

    for (int i = 0; i < kFailures; ++i) {
        (void)vh.Put(Ramp(kDim, static_cast<float>(i)));
        vh.Rollback();
    }
    vh.Sync();

    const auto grown = std::filesystem::file_size(path);
    EXPECT_LE(grown, baseline + PAGE_SIZE)
        << kFailures << " abandoned Puts grew the file by " << (grown - baseline)
        << " bytes; at most one record (" << PAGE_SIZE << ") is expected -- the rest leaked";
    EXPECT_EQ(vh.Count(), 1u) << "abandoned Puts were counted as stored";

    // And the surviving committed record is untouched by all that churn.
    ExpectRamp(vh.Get(0), kDim, 0.0f, "committed record after 20 rollbacks");
}

TEST_F(VectorHeapTest, SyncIsCallableAndIdempotent) {
    // Sync() was declared in the header long before it had a definition, so
    // any call was a link error that no test caught -- because no test called
    // it. This one does.
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 0.0f));

    EXPECT_NO_THROW(vh.Sync());
    EXPECT_NO_THROW(vh.Sync());

    ExpectRamp(vh.Get(0), 8, 0.0f, "record after explicit Sync");
}

// ─────────────────────────────────────────────────────────────────────────────
// Heap identity and VectorRef provenance -- audit B5, consequence 2.
//
// A row stores a VectorRef, not floats, and a reference is only trustworthy if
// it says which heap issued it. A bare index used against the wrong heap
// resolves to a real, plausible-looking vector belonging to another column,
// and nothing downstream can tell that it is wrong. These tests pin the
// mechanism that makes that mistake loud.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, InvalidHeapIdIsRejectedAndCreatesNoFile) {
    EXPECT_THROW(VectorHeap(path, INVALID_HEAP_ID, 8), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path))
        << "a rejected construction still created the heap file";
}

TEST_F(VectorHeapTest, HeapRemembersItsId) {
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_EQ(vh.Id(), kHeapId);
}

TEST_F(VectorHeapTest, ReopenWithWrongHeapIdThrows) {
    // Same fail-fast discipline as the dimension check (A6). Opening column
    // A's heap file under column B's id would make every reference this heap
    // issues or accepts name the wrong record.
    { VectorHeap vh(path, kHeapId, 8); }
    EXPECT_THROW(VectorHeap(path, kHeapId + 1, 8), std::invalid_argument);
    EXPECT_NO_THROW(VectorHeap(path, kHeapId, 8));
}

TEST_F(VectorHeapTest, ReopenPreservesTheHeapIdAlongsideTheCount) {
    {
        VectorHeap vh(path, kHeapId, 8);
        (void)vh.Put(Ramp(8, 5.0f));
        vh.Sync();
    }
    VectorHeap reopened(path, kHeapId, 8);
    EXPECT_EQ(reopened.Id(), kHeapId);
    EXPECT_EQ(reopened.Count(), 1u) << "adding heap_id to the header displaced vector_count";
    ExpectRamp(reopened.Get(0), 8, 5.0f, "record after reopen with heap id in header");
}

TEST_F(VectorHeapTest, RefCarriesTheIssuingHeapAndTheIndex) {
    VectorHeap vh(path, kHeapId, 8);
    const VectorId id = vh.Put(Ramp(8, 1.0f));

    const VectorRef ref = vh.Ref(id);
    EXPECT_EQ(ref.heap_id, kHeapId);
    EXPECT_EQ(ref.index, static_cast<uint32_t>(id));
    EXPECT_TRUE(ref.IsValid());
}

TEST_F(VectorHeapTest, RefRejectsAnIdThatWasNeverStored) {
    using AxolotlDB::utils::InvalidVectorIdException;
    VectorHeap vh(path, kHeapId, 8);
    EXPECT_THROW((void)vh.Ref(0), InvalidVectorIdException)
        << "an empty heap minted a reference to record 0";

    (void)vh.Put(Ramp(8, 0.0f));
    EXPECT_NO_THROW((void)vh.Ref(0));
    EXPECT_THROW((void)vh.Ref(1), InvalidVectorIdException);
}

TEST_F(VectorHeapTest, GetByRefAgreesWithGetById) {
    VectorHeap vh(path, kHeapId, 8);
    for (int i = 0; i < 4; ++i)
        (void)vh.Put(Ramp(8, static_cast<float>(i) * 10.0f));
    vh.Sync();

    for (VectorId id = 0; id < 4; ++id) {
        const auto by_id = vh.Get(id);
        const auto by_ref = vh.Get(vh.Ref(id));
        EXPECT_EQ(by_id, by_ref) << "the two Get overloads disagree at id " << id;
    }
}

TEST_F(VectorHeapTest, ForeignRefIsRejectedRatherThanSilentlyResolved) {
    // The failure heap_id exists to catch, staged exactly as it would happen:
    // two columns, same dimension, same index in each. Without the check the
    // wrong heap answers with a valid-looking vector.
    using AxolotlDB::utils::ForeignVectorRefException;
    const std::string other_path = path + ".other";
    std::filesystem::remove(other_path);

    {
        VectorHeap mine(path, kHeapId, 8);
        VectorHeap theirs(other_path, kHeapId + 1, 8);

        (void)mine.Put(Ramp(8, 100.0f));
        (void)theirs.Put(Ramp(8, 900.0f));
        mine.Sync();
        theirs.Sync();

        const VectorRef theirs_ref = theirs.Ref(0);
        ASSERT_NE(theirs_ref.heap_id, mine.Id());

        // Index 0 exists in both heaps, so the lookup would succeed and return
        // 900.0f-based data for a row that means 100.0f-based data.
        EXPECT_THROW((void)mine.Get(theirs_ref), ForeignVectorRefException);

        // And the right heap still answers it.
        ExpectRamp(theirs.Get(theirs_ref), 8, 900.0f, "reference resolved by its own heap");
    }

    std::filesystem::remove(other_path);
}

TEST_F(VectorHeapTest, DefaultConstructedRefIsRejected) {
    using AxolotlDB::utils::ForeignVectorRefException;
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 0.0f));

    EXPECT_THROW((void)vh.Get(VectorRef{}), ForeignVectorRefException)
        << "a reference no heap ever issued resolved to record 0";
}

TEST_F(VectorHeapTest, RefWithValidHeapButOutOfRangeIndexThrows) {
    using AxolotlDB::utils::InvalidVectorIdException;
    VectorHeap vh(path, kHeapId, 8);
    (void)vh.Put(Ramp(8, 0.0f));

    EXPECT_THROW((void)vh.Get(VectorRef{kHeapId, 5}), InvalidVectorIdException);
}

TEST_F(VectorHeapTest, RefResolvesToTheSameVectorAfterReopen) {
    // This is what makes a VectorRef safe to store in a row: it has to mean
    // the same thing after every in-process object is gone.
    VectorRef ref{};
    {
        VectorHeap vh(path, kHeapId, 300);
        (void)vh.Put(Ramp(300, 0.0f));
        const VectorId id = vh.Put(Ramp(300, 7000.0f));
        vh.Sync();
        ref = vh.Ref(id);
    }

    VectorHeap reopened(path, kHeapId, 300);
    ExpectRamp(reopened.Get(ref), 300, 7000.0f, "reference resolved after reopen");
}

// ─────────────────────────────────────────────────────────────────────────────
// N8 -- the addressing arithmetic narrowed silently at both edges.
//
// page_span is a uint16_t computed from ceil(stride / PAGE_SIZE), and the page
// number was a static_cast<PageId> of a uint64_t. Neither was checked. Both
// failures are silent wrong answers rather than errors, which is precisely the
// shape B2 designed this heap to avoid.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(VectorHeapTest, RejectsDimensionWhosePageSpanOverflows) {
    // One past the largest dimension whose span fits a uint16_t. Unchecked,
    // ceil(stride / PAGE_SIZE) wraps to a SMALLER span than the record needs,
    // so Put writes a truncated vector and Get reads one back.
    EXPECT_THROW(VectorHeap(path, 1, MAX_VECTOR_DIMENSION + 1), std::invalid_argument);
}

TEST_F(VectorHeapTest, OversizedDimensionCreatesNoFile) {
    // Same contract as ZeroDimensionDoesNotCreateTheFile: the check runs in the
    // member-init list, before FileManager can create anything.
    EXPECT_THROW(VectorHeap(path, 1, MAX_VECTOR_DIMENSION + 1), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(VectorHeapTest, MaxDimensionIsItselfAccepted) {
    // The boundary must be inclusive -- an off-by-one here would reject a
    // dimension the format handles fine. Construction only; storing a record
    // this wide would allocate 256 GB.
    EXPECT_NO_THROW(VectorHeap(path, 1, MAX_VECTOR_DIMENSION));
}
