// P2 §2.6 — Catalog: schema-as-a-table with a hardcoded bootstrap, and the one
// hard obligation of this phase — HeapId that is monotonic, persisted, and
// NEVER reused (B5). A reused id makes a stale VectorRef validate against the
// wrong heap.

#include <catalog/catalog.h>
#include <gtest/gtest.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <storage/vector_heap.h>
#include <storage/vector_ref.h>
#include <utils/errors.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace AxolotlDB;
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::Schema;
namespace utils = AxolotlDB::utils;

namespace {

Schema SimpleSchema() {
    return Schema{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR}}};
}

Schema VectorSchema(uint32_t dim = 4) {
    return Schema{{ColumnDef{"id", ColumnType::INT32},
                   ColumnDef{"emb", ColumnType::VECTOR, /*nullable=*/false, dim}}};
}

Schema MultiVectorSchema(uint32_t dim1 = 4, uint32_t dim2 = 8) {
    return Schema{{ColumnDef{"id", ColumnType::INT32},
                   ColumnDef{"emb1", ColumnType::VECTOR, /*nullable=*/false, dim1},
                   ColumnDef{"emb2", ColumnType::VECTOR, /*nullable=*/false, dim2}}};
}

}  // namespace

struct CatalogTest : ::testing::Test {
    std::filesystem::path dir;
    std::filesystem::path orig_dir;
    void SetUp() override {
        orig_dir = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
              ("axo_cat_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::current_path(orig_dir, ec);
        std::filesystem::remove_all(dir, ec);
    }
    std::string catalogFile() const {
        return (dir / "catalog.db").string();
    }
};

// ─── Schema wire-format widths (§4.2). VECTOR is 8 bytes: a VectorRef. ────────

TEST(SchemaTest, FixedWidthsMatchFormatCatalogue) {
    EXPECT_EQ(catalog::FixedWidth(ColumnType::INT32), 4u);
    EXPECT_EQ(catalog::FixedWidth(ColumnType::INT64), 8u);
    EXPECT_EQ(catalog::FixedWidth(ColumnType::FLOAT32), 4u);
    EXPECT_EQ(catalog::FixedWidth(ColumnType::FLOAT64), 8u);
    EXPECT_EQ(catalog::FixedWidth(ColumnType::BOOLEAN), 1u);
    EXPECT_EQ(catalog::FixedWidth(ColumnType::VECTOR), 8u)
        << "a VECTOR column is a fixed-width VectorRef, not the embedding (B5)";
}

// ─── Bootstrap ───────────────────────────────────────────────────────────────

TEST_F(CatalogTest, BootstrapsFromHardcodedSchema) {
    storage::StorageManager sm(32);
    // Constructing a Catalog on empty storage must succeed by materialising the
    // catalog table from its hardcoded schema — nothing to read from disk yet.
    EXPECT_NO_THROW({
        catalog::Catalog cat(sm);
        (void)cat;
    });

    // Verify catalog file was allocated and created on disk
    EXPECT_TRUE(std::filesystem::exists("catalog.db"));
}

TEST_F(CatalogTest, BootstrapIsIdempotentOnReopen) {
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        cat.CreateTable("t", SimpleSchema());
        sm.FlushAll();
    }
    // A second bootstrap over the same files must reload, not re-initialise:
    // the existing table survives.
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);
    EXPECT_NO_THROW((void)cat.GetTableId("t"));

    // Multiple reloads are idempotent
    sm.FlushAll();
    storage::StorageManager sm2(32);
    catalog::Catalog cat2(sm2);
    EXPECT_EQ(cat2.GetTableId("t"), 1u);
}

// ─── CRUD round-trips ────────────────────────────────────────────────────────

TEST_F(CatalogTest, CreateThenGetRoundTrips) {
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);
    const auto id = cat.CreateTable("users", SimpleSchema());
    EXPECT_EQ(cat.GetTableId("users"), id);
    EXPECT_EQ(cat.GetTableName(id), "users");
    EXPECT_EQ(cat.GetSchema(id).ColumnCount(), 2u);
    EXPECT_EQ(cat.GetSchema("users").ColumnCount(), 2u);

    // Duplicate table name must throw StorageException
    EXPECT_THROW((void)cat.CreateTable("users", SimpleSchema()), utils::StorageException)
        << "creating a table with an already existing name must throw";
}

TEST_F(CatalogTest, UnknownTableNameThrows) {
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);

    EXPECT_THROW((void)cat.GetTableId("nope"), utils::StorageException);
    EXPECT_THROW((void)cat.GetSchema("nope"), utils::StorageException);
    EXPECT_THROW((void)cat.GetTableHeap("nope"), utils::StorageException);
    EXPECT_THROW((void)cat.GetVectorHeap("nope", "col"), utils::StorageException);
    EXPECT_THROW((void)cat.GetSchema(9999), utils::StorageException);
    EXPECT_THROW((void)cat.GetTableName(9999), utils::StorageException);
    EXPECT_THROW((void)cat.GetTableHeap(9999), utils::StorageException);
}

TEST_F(CatalogTest, SchemaSurvivesReopen) {
    const Schema full_schema{{
        ColumnDef{"c_int32", ColumnType::INT32},
        ColumnDef{"c_int64", ColumnType::INT64},
        ColumnDef{"c_f32", ColumnType::FLOAT32},
        ColumnDef{"c_f64", ColumnType::FLOAT64},
        ColumnDef{"c_bool", ColumnType::BOOLEAN},
        ColumnDef{"c_str", ColumnType::VARCHAR, /*nullable=*/true},
        ColumnDef{"c_vec", ColumnType::VECTOR, /*nullable=*/false, /*dimension=*/128},
    }};

    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        cat.CreateTable("multi", full_schema);
        sm.FlushAll();
    }
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        const auto id = cat.GetTableId("multi");
        const auto& loaded = cat.GetSchema(id);
        ASSERT_EQ(loaded.ColumnCount(), 7u);
        EXPECT_EQ(loaded.columns[0].name, "c_int32");
        EXPECT_EQ(loaded.columns[0].type, ColumnType::INT32);
        EXPECT_EQ(loaded.columns[1].name, "c_int64");
        EXPECT_EQ(loaded.columns[1].type, ColumnType::INT64);
        EXPECT_EQ(loaded.columns[2].name, "c_f32");
        EXPECT_EQ(loaded.columns[2].type, ColumnType::FLOAT32);
        EXPECT_EQ(loaded.columns[3].name, "c_f64");
        EXPECT_EQ(loaded.columns[3].type, ColumnType::FLOAT64);
        EXPECT_EQ(loaded.columns[4].name, "c_bool");
        EXPECT_EQ(loaded.columns[4].type, ColumnType::BOOLEAN);
        EXPECT_EQ(loaded.columns[5].name, "c_str");
        EXPECT_EQ(loaded.columns[5].type, ColumnType::VARCHAR);
        EXPECT_TRUE(loaded.columns[5].nullable);
        EXPECT_EQ(loaded.columns[6].name, "c_vec");
        EXPECT_EQ(loaded.columns[6].type, ColumnType::VECTOR);
        EXPECT_FALSE(loaded.columns[6].nullable);
        EXPECT_EQ(loaded.columns[6].dimension, 128u);
    }
}

// ─── The hard obligation: HeapId is never reused (B5) ────────────────────────

TEST_F(CatalogTest, HeapIdIsNeverReusedAfterColumnDrop) {
    // Actively attempt to break B5: create, drop, create again, drop again,
    // and reopen to ensure monotonic counter is preserved and dropped IDs
    // are NEVER reissued.
    HeapId first = storage::INVALID_HEAP_ID;
    HeapId second = storage::INVALID_HEAP_ID;
    HeapId third1 = storage::INVALID_HEAP_ID;
    HeapId third2 = storage::INVALID_HEAP_ID;

    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);

        const auto t1 = cat.CreateTable("t1", VectorSchema());
        first = cat.GetVectorHeap(t1, "emb").Id();
        cat.DropTable("t1");

        const auto t2 = cat.CreateTable("t2", VectorSchema());
        second = cat.GetVectorHeap(t2, "emb").Id();
        EXPECT_GT(second, first)
            << "dropping the highest heap and re-creating must NOT reissue its id";

        cat.DropTable("t2");

        const auto t3 = cat.CreateTable("t3", MultiVectorSchema(4, 8));
        third1 = cat.GetVectorHeap(t3, "emb1").Id();
        third2 = cat.GetVectorHeap(t3, "emb2").Id();
        EXPECT_GT(third1, second);
        EXPECT_GT(third2, third1);

        cat.DropTable("t3");
        sm.FlushAll();
    }

    // Reopen and create after drop: id must remain strictly greater than third2
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);

        const auto t4 = cat.CreateTable("t4", VectorSchema());
        const HeapId fourth = cat.GetVectorHeap(t4, "emb").Id();
        EXPECT_GT(fourth, third2) << "heap id counter must survive drop + reopen monotonically";
    }
}

TEST_F(CatalogTest, HeapIdCounterSurvivesReopen) {
    HeapId last = storage::INVALID_HEAP_ID;
    {
        storage::StorageManager sm(32);
        catalog::Catalog cat(sm);
        last = cat.AllocateHeapId();
        (void)cat.AllocateHeapId();
        last = cat.AllocateHeapId();
        sm.FlushAll();
    }
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);
    const HeapId next = cat.AllocateHeapId();
    EXPECT_GT(next, last) << "the heap-id counter must persist across a reopen";
}

// ─── Vector column resolution end-to-end (B5) ────────────────────────────────

TEST_F(CatalogTest, VectorColumnResolvesToItsOwnHeap) {
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);
    const auto t = cat.CreateTable("docs", VectorSchema(4));

    storage::VectorHeap& heap = cat.GetVectorHeap(t, "emb");
    const storage::VectorId vid = heap.Put(std::vector<float>{1.5f, 2.5f, 3.5f, 4.5f});
    const storage::VectorRef ref = heap.Ref(vid);

    EXPECT_EQ(ref.heap_id, heap.Id());
    const auto got = heap.Get(ref);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_FLOAT_EQ(got[0], 1.5f);
    EXPECT_FLOAT_EQ(got[1], 2.5f);
    EXPECT_FLOAT_EQ(got[2], 3.5f);
    EXPECT_FLOAT_EQ(got[3], 4.5f);
}

TEST_F(CatalogTest, ForeignVectorRefStillThrows) {
    storage::StorageManager sm(32);
    catalog::Catalog cat(sm);
    const auto a = cat.CreateTable("a", VectorSchema(4));
    const auto b = cat.CreateTable("b", VectorSchema(4));

    storage::VectorHeap& ha = cat.GetVectorHeap(a, "emb");
    storage::VectorHeap& hb = cat.GetVectorHeap(b, "emb");
    const storage::VectorId vid = hb.Put(std::vector<float>{9, 9, 9, 9});
    const storage::VectorRef foreign = hb.Ref(vid);  // stamped with hb's id

    EXPECT_THROW((void)ha.Get(foreign), utils::ForeignVectorRefException)
        << "the B5 guarantee: a ref only validates against the heap that issued it";
}
