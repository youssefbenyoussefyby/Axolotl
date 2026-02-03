#include <catalog/catalog.h>
#include <utils/errors.h>

#include <memory>
namespace AxolotlDB::catalog {

Catalog::Catalog(storage::StorageManager& sm) : storage_manager_(sm) {
    catalog_file_id_ = storage_manager_.OpenFile("catalog.db");
    if (storage_manager_.PageCount(catalog_file_id_) == 0) {
        storage::PageRef ref;
        auto guard = storage_manager_.NewPage(catalog_file_id_, ref);
        next_table_id_ = 1;
        next_heap_id_ = 1;
        PersistCatalogMetadata();
    } else {
        LoadCatalogMetadata();
    }
};
TableId Catalog::CreateTable(const std::string& name, const storage::Schema& schema) {
    if (name_to_id_.find(name) != name_to_id_.end()) {
        throw utils::StorageException("Table already exists: " + name);
    }
    TableEntry te;
    TableId table_id = next_table_id_++;
    te.id = table_id;
    te.name = name;
    te.schema = schema;
    te.file_id = storage_manager_.OpenFile("table_" + std::to_string(te.id) + ".db");
    te.table_heap = std::make_unique<storage::TableHeap>(storage_manager_, te.file_id);
    for (const auto& col : schema.columns) {
        if (col.type == storage::ColumnType::VECTOR) {
            storage::HeapId heap_id = AllocateHeapIdInternal();
            std::string vheap_filename = "vheap_" + std::to_string(heap_id) + ".db";
            te.vector_heaps[col.name] =
                std::make_unique<storage::VectorHeap>(vheap_filename, heap_id, col.dimension);
        }
    }
    name_to_id_[name] = table_id;
    tables_[table_id] = std::move(te);
    PersistCatalogMetadata();
    return table_id;
}

void Catalog::DropTable(const std::string& name) {
    TableId id = GetTableId(name);
    TableEntry& entry = tables_.at(id);
    storage_manager_.CloseFile(entry.file_id);
    tables_.erase(id);
    name_to_id_.erase(name);
    PersistCatalogMetadata();
}

const std::string& Catalog::GetTableName(TableId id) {
    auto it = tables_.find(id);
    if (it == tables_.end())
        throw utils::StorageException("There is no table with this ID : " + std::to_string(id));
    return it->second.name;
}

const TableId Catalog::GetTableId(const std::string& name) {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end())
        throw utils::StorageException("There is no Table with the name : " + name);
    if (tables_.find(it->second) == tables_.end())
        throw utils::StorageException("There is no table with this ID : " +
                                      std::to_string(it->second));
    return it->second;
}

const storage::Schema& Catalog::GetSchema(TableId id) {
    const std::string& name = GetTableName(id);
    return GetSchema(name);
}
const storage::Schema& Catalog::GetSchema(const std::string& name) {
    TableId id = GetTableId(name);
    TableEntry& entry = tables_.at(id);
    const storage::Schema& schema = entry.schema;
    return schema;
}
storage::TableHeap& Catalog::GetTableHeap(const std::string& name) {
    TableId id = GetTableId(name);
    auto it = tables_.find(id);
    if (it == tables_.end()) {
        throw utils::StorageException("There is no table with this ID : " + std::to_string(id));
    }
    return *it->second.table_heap;
}

storage::TableHeap& Catalog::GetTableHeap(TableId id) {
    const std::string& name = GetTableName(id);
    return GetTableHeap(name);
}

storage::VectorHeap& Catalog::GetVectorHeap(const std::string& name, const std::string& column) {
    TableId id = GetTableId(name);
    auto table_it = tables_.find(id);
    if (table_it == tables_.end()) {
        throw utils::StorageException("There is no table with this ID : " + std::to_string(id));
    }
    TableEntry& entry = table_it->second;
    auto it = entry.vector_heaps.find(column);
    if (it == entry.vector_heaps.end()) {
        throw utils::StorageException("There is no Column with this name : " + column);
    }
    return *it->second;
}
storage::VectorHeap& Catalog::GetVectorHeap(TableId id, const std::string& column) {
    const std::string& name = GetTableName(id);
    return GetVectorHeap(name, column);
}

void Catalog::PersistCatalogMetadata() {
    storage::PageRef ref{catalog_file_id_, 0};
    auto guard = storage_manager_.FetchWrite(ref);
    std::byte* dst = guard->RawData().data() + sizeof(storage::PageHeader);

    auto write = [&dst](const void* src, size_t size) {
        std::memcpy(dst, src, size);
        dst += size;
    };

    uint32_t magic = 0xCA7A1060;
    write(&magic, sizeof(magic));
    write(&next_table_id_, sizeof(next_table_id_));
    write(&next_heap_id_, sizeof(next_heap_id_));

    uint32_t num_tables = static_cast<uint32_t>(tables_.size());
    write(&num_tables, sizeof(num_tables));
    for (const auto& [tid, te] : tables_) {
        write(&te.id, sizeof(te.id));
        uint32_t name_len = static_cast<uint32_t>(te.name.size());
        write(&name_len, sizeof(name_len));
        write(te.name.data(), name_len);
        uint32_t col_count = static_cast<uint32_t>(te.schema.columns.size());
        write(&col_count, sizeof(col_count));
        for (const auto& col : te.schema.columns) {
            uint32_t col_name_len = static_cast<uint32_t>(col.name.size());
            write(&col_name_len, sizeof(col_name_len));
            write(col.name.data(), col_name_len);

            uint8_t type = static_cast<uint8_t>(col.type);
            write(&type, sizeof(type));
            uint8_t nullable = col.nullable ? 1 : 0;
            write(&nullable, sizeof(nullable));
            write(&col.dimension, sizeof(col.dimension));
            if (col.type == storage::ColumnType::VECTOR) {
                storage::HeapId hid = te.vector_heaps.at(col.name)->Id();
                write(&hid, sizeof(hid));
            }
        }
    }

    guard->MarkDirty();
    guard.Drop();
    storage_manager_.FlushFile(catalog_file_id_);
}

void Catalog::LoadCatalogMetadata() {
    storage::PageRef ref{catalog_file_id_, 0};
    auto guard = storage_manager_.FetchRead(ref);
    const std::byte* src = guard->RawData().data() + sizeof(storage::PageHeader);

    auto read = [&src](void* dst, size_t size) {
        std::memcpy(dst, src, size);
        src += size;
    };
    uint32_t magic = 0;
    read(&magic, sizeof(magic));
    if (magic != 0xCA7A1060) {
        throw utils::StorageException("Corrupt catalog header magic");
    }

    read(&next_table_id_, sizeof(next_table_id_));
    read(&next_heap_id_, sizeof(next_heap_id_));

    uint32_t num_tables = 0;
    read(&num_tables, sizeof(num_tables));
    tables_.clear();
    name_to_id_.clear();
    for (uint32_t i = 0; i < num_tables; ++i) {
        TableEntry te;
        read(&te.id, sizeof(te.id));

        uint32_t name_len = 0;
        read(&name_len, sizeof(name_len));
        te.name.resize(name_len);
        read(te.name.data(), name_len);
        uint32_t col_count = 0;
        read(&col_count, sizeof(col_count));
        for (uint32_t c = 0; c < col_count; ++c) {
            storage::ColumnDef col;
            uint32_t col_name_len = 0;
            read(&col_name_len, sizeof(col_name_len));
            col.name.resize(col_name_len);
            read(col.name.data(), col_name_len);

            uint8_t type = 0;
            read(&type, sizeof(type));
            col.type = static_cast<storage::ColumnType>(type);

            uint8_t nullable = 0;
            read(&nullable, sizeof(nullable));
            col.nullable = (nullable != 0);
            read(&col.dimension, sizeof(col.dimension));
            if (col.type == storage::ColumnType::VECTOR) {
                storage::HeapId hid = 0;
                read(&hid, sizeof(hid));

                std::string vheap_filename = "vheap_" + std::to_string(hid) + ".db";
                te.vector_heaps[col.name] =
                    std::make_unique<storage::VectorHeap>(vheap_filename, hid, col.dimension);
            }
            te.schema.columns.push_back(col);
        }
        std::string table_filename = "table_" + std::to_string(te.id) + ".db";
        te.file_id = storage_manager_.OpenFile(table_filename);
        te.table_heap = std::make_unique<storage::TableHeap>(storage_manager_, te.file_id);

        name_to_id_[te.name] = te.id;
        tables_[te.id] = std::move(te);
    }
}

storage::HeapId Catalog::AllocateHeapId() {
    storage::HeapId id = next_heap_id_++;
    PersistCatalogMetadata();
    return id;
}
storage::HeapId Catalog::AllocateHeapIdInternal() {
    return next_heap_id_++;
}
size_t FixedWidth(storage::ColumnType type) {
    switch (type) {
        case storage::ColumnType::INT32:
            return 4;
        case storage::ColumnType::INT64:
            return 8;
        case storage::ColumnType::FLOAT32:
            return 4;
        case storage::ColumnType::FLOAT64:
            return 8;
        case storage::ColumnType::BOOLEAN:
            return 1;
        case storage::ColumnType::VECTOR:
            return 8;
        default:
            throw utils::StorageException("Unknown type");
    }
}

}  // namespace AxolotlDB::catalog