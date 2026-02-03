// The Phase 1 gate test -- docs/audit.md section D.
//
// Every other storage test isolates one layer or one bug (A1-A8, B1). This one
// does not: it drives FileManager, BufferPool, Page, Tuple *and* VectorHeap
// together, the way real usage would, and is the only test in the suite that
// proves they hold together rather than each in its own narrow harness.
//
// The scenario, verbatim from the audit's own definition of the gate:
//   create DB -> insert 500 multi-column tuples with NULLs across many pages
//   -> FlushAllPages() + Sync() -> destroy every object -> reopen from the
//   same file -> read all 500 back byte-identical.
//
// Schema deliberately interleaves a fixed-length column between two nullable
// columns so that adjacent-NULL columns -- the exact shape of the A1
// corruption -- occur at scale across many pages and many buffer-pool eviction
// cycles, not just in the single-tuple unit tests that already cover it.
//
// Since B2/B5 the gate also spans two files. The embedding column is 1536-dim
// -- 6144 bytes, larger than an entire 4096-byte page -- so before the wiring
// these rows were physically impossible to insert. Now the row stores an
// 8-byte VectorRef and the floats live in a VectorHeap, and the gate proves
// the reference still resolves to the right embedding after both files have
// been closed and reopened.

#include <gtest/gtest.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <storage/vector_heap.h>
#include <storage/vector_ref.h>
#include <utils/errors.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace AxolotlDB::storage;

namespace {

// 1536 floats = 6144 bytes: deliberately larger than a page, so a row storing
// this inline could never be inserted at all. Each record spans two heap pages.
constexpr uint32_t kEmbeddingDim = 1536;

// Stand-in for a catalog-assigned heap identity.
constexpr HeapId kEmbeddingHeapId = 1;

Schema MakeSchema(std::vector<ColumnDef> cols) {
    Schema s;
    s.columns = std::move(cols);
    return s;
}

ColumnDef Int32Col(const char* name) {
    return ColumnDef{name, ColumnType::INT32, false, 0};
}
ColumnDef Int64Col(const char* name) {
    return ColumnDef{name, ColumnType::INT64, true, 0};
}
ColumnDef BoolCol(const char* name) {
    return ColumnDef{name, ColumnType::BOOLEAN, false, 0};
}
ColumnDef Varchar(const char* name) {
    return ColumnDef{name, ColumnType::VARCHAR, true, 0};
}
ColumnDef VectorCol(const char* name, uint32_t dim) {
    return ColumnDef{name, ColumnType::VECTOR, true, dim};
}

// id | name (varchar, nullable) | embedding (vector<1536>, nullable) | big
// (int64, nullable) | active (bool). name and embedding sit next to each other
// so a NULL in one followed by data in the other reproduces A1's exact shape.
Schema GateSchema() {
    return MakeSchema({
        Int32Col("id"),
        Varchar("name"),
        VectorCol("embedding", kEmbeddingDim),
        Int64Col("big"),
        BoolCol("active"),
    });
}

// v[k] = i*1000 + k. Positional and row-specific, so an embedding that comes
// back as its neighbour's, or shifted by a few floats, fails loudly instead of
// looking plausible. Every value is under 2^24, so float holds it exactly.
std::vector<float> Embedding(int i) {
    std::vector<float> v(kEmbeddingDim);
    for (uint32_t k = 0; k < kEmbeddingDim; ++k) {
        v[k] = static_cast<float>(i) * 1000.0f + static_cast<float>(k);
    }
    return v;
}

bool RowHasEmbedding(int i) {
    return i % 11 != 0;
}

// Deterministic, varied NULL placement: name is null on multiples of 7,
// embedding on multiples of 11, big on multiples of 5 -- so multiples of 77
// hit adjacent-NULL name+embedding, and multiples of 385 (5*7*11, well under
// 500) null every nullable column on the row at once.
std::vector<Value> RowValues(int i, std::optional<VectorRef> ref) {
    std::vector<Value> v;
    v.push_back(Value::Int32(i));
    v.push_back((i % 7 == 0) ? Value::Null(ColumnType::VARCHAR)
                             : Value::Varchar("row_" + std::to_string(i)));
    v.push_back(ref.has_value() ? Value::Vector(*ref) : Value::Null(ColumnType::VECTOR));
    v.push_back((i % 5 == 0) ? Value::Null(ColumnType::INT64)
                             : Value::Int64(static_cast<int64_t>(i) * 1000003));
    v.push_back(Value::Bool(i % 2 == 0));
    return v;
}

}  // namespace

class PersistenceGateTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        db_path = std::string("test_gate_") + info->name() + ".db";
        heap_path = db_path + ".vec";
        std::filesystem::remove(db_path);
        std::filesystem::remove(heap_path);
    }

    void TearDown() override {
        std::filesystem::remove(db_path);
        std::filesystem::remove(heap_path);
    }

    std::string db_path;
    std::string heap_path;
};

TEST_F(PersistenceGateTest, FiveHundredMultiColumnTuplesWithNullsSurviveReopenByteIdentical) {
    constexpr int kRowCount = 500;
    const Schema schema = GateSchema();

    // page_id -> ordered (slot, expected serialized bytes) written to it.
    std::map<PageId, std::vector<std::pair<SlotIdx, std::vector<std::byte>>>> expected;
    // row id -> the reference that row stored, for the resolve-after-reopen pass.
    std::map<int, VectorRef> expected_refs;

    {
        StorageManager sm(8);
        const FileId fid = sm.OpenFile(db_path);
        VectorHeap heap(heap_path, kEmbeddingHeapId, kEmbeddingDim);

        PageRef pref;
        WritePageGuard guard = sm.NewPage(fid, pref);
        PageId page_id = pref.page_no;

        for (int i = 0; i < kRowCount; ++i) {
            std::optional<VectorRef> ref;
            if (RowHasEmbedding(i)) {
                const auto embedding = Embedding(i);
                const VectorId vid = heap.Put(embedding);
                ref = heap.Ref(vid);
                expected_refs[i] = *ref;
            }

            auto bytes = Tuple::Serialize(schema, RowValues(i, ref));

            SlotIdx slot;
            try {
                slot = guard->InsertTuple(bytes);
            } catch (const AxolotlDB::utils::PageFullException&) {
                guard.Drop();
                guard = sm.NewPage(fid, pref);
                page_id = pref.page_no;
                slot = guard->InsertTuple(bytes);
            }
            expected[page_id].emplace_back(slot, std::move(bytes));
        }
        guard.Drop();

        // Order matters. Committing the heap first can at worst leave vectors
        // that no row references -- bounded, reclaimable waste. The reverse
        // order would leave rows pointing at vectors that never reached disk,
        // which is unrecoverable corruption.
        heap.Sync();
        sm.FlushAll();
    }  // Every in-process object destroyed -- no state left to cheat with.

    ASSERT_GT(expected.size(), 1u)
        << "500 rows landed on a single page -- this run proves nothing about "
           "multi-page persistence; the row payload size needs to shrink or "
           "kRowCount needs to grow";

    StorageManager reopened_sm(8);
    const FileId reopened_fid = reopened_sm.OpenFile(db_path);
    ASSERT_EQ(reopened_sm.PageCount(reopened_fid), static_cast<PageId>(expected.size()));

    VectorHeap reopened_heap(heap_path, kEmbeddingHeapId, kEmbeddingDim);

    ASSERT_EQ(reopened_heap.Count(), expected_refs.size())
        << "the heap forgot how many embeddings it committed";

    std::size_t rows_verified = 0;
    std::size_t vectors_verified = 0;
    for (const auto& [page_id, slots] : expected) {
        ReadPageGuard rguard = reopened_sm.FetchRead(PageRef{reopened_fid, page_id});

        for (const auto& [slot, want_bytes] : slots) {
            auto got = rguard->GetTuple(slot);
            ASSERT_EQ(got.size(), want_bytes.size())
                << "page " << page_id << " slot " << slot << " length mismatch";
            EXPECT_TRUE(std::equal(got.begin(), got.end(), want_bytes.begin()))
                << "page " << page_id << " slot " << slot
                << " bytes are not identical after reopen";

            // Not just byte-identical -- also must still decode cleanly.
            std::vector<Value> row;
            ASSERT_NO_THROW(row = Tuple::Deserialize(schema, got))
                << "page " << page_id << " slot " << slot
                << " round-tripped bytes but failed to deserialize";

            const int id = row[0].AsInt32();
            ASSERT_EQ(row.size(), schema.ColumnCount());

            if (!RowHasEmbedding(id)) {
                EXPECT_TRUE(row[2].IsNull()) << "row " << id << " grew an embedding";
                continue;
            }

            ASSERT_FALSE(row[2].IsNull()) << "row " << id << " lost its embedding";
            const VectorRef ref = row[2].AsVector();
            EXPECT_EQ(ref, expected_refs.at(id))
                << "row " << id << " came back with a different reference";

            // The reference has to still mean something in a heap that was
            // closed and reopened -- this is the part a byte-comparison of the
            // row alone would never catch.
            const auto embedding = reopened_heap.Get(ref);
            EXPECT_EQ(embedding, Embedding(id))
                << "row " << id << "'s reference resolved to the wrong embedding";
            ++vectors_verified;
        }
        rows_verified += slots.size();
    }

    EXPECT_EQ(rows_verified, static_cast<std::size_t>(kRowCount));
    EXPECT_EQ(vectors_verified, expected_refs.size());
    EXPECT_GT(vectors_verified, 0u);
}

// A row whose embedding is written but never committed must not survive as a
// dangling reference. Rollback() rewinds the heap, so the row is abandoned too
// -- the two have to be abandoned together or the row store ends up pointing
// at a record that does not exist.
TEST_F(PersistenceGateTest, RolledBackEmbeddingLeavesNoRowBehind) {
    const Schema schema = GateSchema();

    VectorHeap heap(heap_path, kEmbeddingHeapId, kEmbeddingDim);

    const VectorId committed_id = heap.Put(Embedding(1));
    const VectorRef committed_ref = heap.Ref(committed_id);
    heap.Sync();

    // Second insert: the vector lands, then the row fails to.
    (void)heap.Put(Embedding(2));
    ASSERT_EQ(heap.PendingCount(), 1u);
    heap.Rollback();

    EXPECT_EQ(heap.Count(), 1u) << "the abandoned embedding was still counted";
    EXPECT_EQ(heap.Get(committed_ref), Embedding(1))
        << "rolling back the second Put damaged the first";

    // The id the abandoned Put consumed is handed out again, and the next
    // committed vector takes its place rather than leaking one.
    const VectorId reused = heap.Put(Embedding(3));
    EXPECT_EQ(reused, committed_id + 1) << "the rolled-back id was not reused";
    heap.Sync();
    EXPECT_EQ(heap.Get(heap.Ref(reused)), Embedding(3));
}
