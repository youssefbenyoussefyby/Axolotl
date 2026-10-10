#include <gtest/gtest.h>
#include <storage/buffer_pool.h>
#include <storage/file_manager.h>
#include <storage/page.h>
#include <storage/page_guard.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace AxolotlDB::storage;

class StorageEngineTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Ensure clean state. One file per test: ctest runs each TEST_F as its own
        // process in a shared working directory, so a fixed name collides under
        // `ctest --parallel`.
        test_db_path = std::string("test_axolot_") +
                       ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
        if (std::filesystem::exists(test_db_path)) {
            std::filesystem::remove(test_db_path);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(test_db_path)) {
            std::filesystem::remove(test_db_path);
        }
    }

    std::string test_db_path;
};

// ── 1. Page Tests ──────────────────────────────────────────────────────────

TEST_F(StorageEngineTest, PageInsertAndRead) {
    Page page;
    page.SetPageId(1);

    std::string text = "Hello World";
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(text.data()), text.size());

    SlotIdx slot = page.InsertTuple(data);

    EXPECT_EQ(slot, 0);
    EXPECT_EQ(page.SlotCount(), 1);
    EXPECT_TRUE(page.IsDirty());

    auto retrieved = page.GetTuple(slot);
    std::string retrieved_text(reinterpret_cast<const char*>(retrieved.data()), retrieved.size());

    EXPECT_EQ(text, retrieved_text);
}

// ── 2. Tuple Serialization Tests ───────────────────────────────────────────

TEST_F(StorageEngineTest, TupleSerializationWithVectors) {
    Schema schema;
    schema.columns.push_back({"id", ColumnType::INT32, false, 0});
    schema.columns.push_back({"embedding", ColumnType::VECTOR, false, 3});
    schema.columns.push_back({"name", ColumnType::VARCHAR, true, 0});

    // Since B5 the row carries a VectorRef, not the embedding itself; the
    // floats live in a VectorHeap. See persistence_test.cpp for the end-to-end
    // version that actually stores and resolves one.
    const VectorRef ref{5, 17};
    std::vector<Value> values = {Value::Int32(42), Value::Vector(ref), Value::Varchar("axolotl")};

    auto serialized = Tuple::Serialize(schema, values);
    Tuple tuple(std::move(serialized));

    auto deserialized = Tuple::Deserialize(schema, tuple.Span());

    EXPECT_EQ(deserialized.size(), 3);
    EXPECT_EQ(deserialized[0].AsInt32(), 42);
    EXPECT_EQ(deserialized[1].AsVector(), ref);
    EXPECT_EQ(deserialized[2].AsVarchar(), "axolotl");
}

// ── 3. FileManager + BufferPool Integration ────────────────────────────────

TEST_F(StorageEngineTest, BufferPoolNewAndFetch) {
    StorageManager sm(3);
    const FileId fid = sm.OpenFile(test_db_path);

    PageRef p0{}, p1{}, p2{}, p3{};

    auto write_marker = [](WritePageGuard& g, const std::string& text) {
        std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                         text.size());
        (void)(*g).InsertTuple(bytes);
    };
    auto read_marker = [](ReadPageGuard& g) {
        auto t = (*g).GetTuple(0);
        return std::string(reinterpret_cast<const char*>(t.data()), t.size());
    };

    // Create 3 pages (fills the pool), each with its own marker.
    {
        auto g0 = sm.NewPage(fid, p0);
        EXPECT_EQ((*g0).GetPageId(), p0.page_no);
        write_marker(g0, "page-zero");
    }
    {
        auto g1 = sm.NewPage(fid, p1);
        write_marker(g1, "page-one");
    }
    {
        auto g2 = sm.NewPage(fid, p2);
        write_marker(g2, "page-two");
    }

    // Create a 4th page. This forces eviction of p0 (LRU).
    {
        auto g3 = sm.NewPage(fid, p3);
        write_marker(g3, "page-three");
    }

    // p0 was evicted to disk. Fetching it back must produce the bytes it was
    // evicted with, not a zero-filled page and not a neighbour's contents.
    {
        auto fetched_p0 = sm.FetchRead(p0);
        EXPECT_EQ((*fetched_p0).GetPageId(), p0.page_no);
        EXPECT_EQ(read_marker(fetched_p0), "page-zero")
            << "evicted page came back without its contents";
    }

    // Every other page must still be intact too, whether it stayed resident
    // or was evicted in turn by the fetch above.
    for (const auto& [ref, want] : std::vector<std::pair<PageRef, std::string>>{
             {p1, "page-one"}, {p2, "page-two"}, {p3, "page-three"}}) {
        auto p = sm.FetchRead(ref);
        EXPECT_EQ(read_marker(p), want) << "page " << ref.page_no << " lost its contents";
    }
}

TEST_F(StorageEngineTest, BufferPoolPinningPreventsEviction) {
    StorageManager sm(2);
    const FileId fid = sm.OpenFile(test_db_path);

    PageRef p0{}, p1{};
    auto g0 = sm.NewPage(fid, p0);
    auto g1 = sm.NewPage(fid, p1);

    // Try to create a new page when pool is full of pinned pages.
    PageRef p2{};
    EXPECT_THROW((void)sm.NewPage(fid, p2), AxolotlDB::utils::BufferPoolExhaustedException);
}