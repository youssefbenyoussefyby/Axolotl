#pragma once

#include <storage/page_guard.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>
#include <storage/vector_heap.h>

#include <unordered_map>
#include <vector>
namespace AxolotlDB {
using storage::HeapId;

}
namespace AxolotlDB::catalog {
using TableId = uint32_t;

struct TableEntry {
    TableId id;
    std::string name;
    storage::Schema schema;
    storage::FileId file_id;
    std::unique_ptr<storage::TableHeap> table_heap;
    std::unordered_map<std::string, std::unique_ptr<storage::VectorHeap>> vector_heaps;
};
std::size_t FixedWidth(storage::ColumnType type);

class Catalog {
   public:
    explicit Catalog(storage::StorageManager& sm);
    TableId CreateTable(const std::string& name, const storage::Schema& schema);
    void DropTable(const std::string& name);

    [[nodiscard]] const storage::Schema& GetSchema(const std::string& name);
    [[nodiscard]] const storage::Schema& GetSchema(TableId id);
    [[nodiscard]] const TableId GetTableId(const std::string& name);
    [[nodiscard]] const std::string& GetTableName(TableId id);
    [[nodiscard]] storage::TableHeap& GetTableHeap(const std::string& name);
    [[nodiscard]] storage::TableHeap& GetTableHeap(TableId id);
    [[nodiscard]] storage::VectorHeap& GetVectorHeap(const std::string& name,
                                                     const std::string& column);
    [[nodiscard]] storage::VectorHeap& GetVectorHeap(TableId id, const std::string& column);
    storage::HeapId AllocateHeapId();

   private:
    storage::HeapId AllocateHeapIdInternal();
    storage::StorageManager& storage_manager_;
    std::unordered_map<std::string, TableId> name_to_id_;
    std::unordered_map<TableId, TableEntry> tables_;
    TableId next_table_id_{1};
    storage::HeapId next_heap_id_{1};
    void PersistCatalogMetadata();
    void LoadCatalogMetadata();
    storage::FileId catalog_file_id_;
};

}  // namespace AxolotlDB::catalog