#pragma once

#include <catalog/catalog.h>
#include <storage/storage_manager.h>
#include <utils/log/transaction.h>

#include <cstddef>
#include <stdexcept>

namespace AxolotlDB::executor {

class ExecutorContext {
public:
    ExecutorContext(storage::StorageManager& storage, catalog::Catalog& catalog,
                    utils::log::Transaction* transaction = nullptr,
                    std::size_t batch_size = 1024)
        : storage_(storage), catalog_(catalog), transaction_(transaction), batch_size_(batch_size) {
        if (batch_size_ == 0) {
            throw std::invalid_argument("ExecutorContext: batch_size must be greater than zero");
        }
    }

    [[nodiscard]] storage::StorageManager& GetStorage() const { return storage_; }
    [[nodiscard]] catalog::Catalog& GetCatalog() const { return catalog_; }
    [[nodiscard]] utils::log::Transaction* GetTransaction() const { return transaction_; }
    [[nodiscard]] std::size_t GetBatchSize() const { return batch_size_; }
    void SetBatchSize(std::size_t batch_size) {
        if (batch_size == 0) {
            throw std::invalid_argument("ExecutorContext: batch_size must be greater than zero");
        }
        batch_size_ = batch_size;
    }

private:
    storage::StorageManager& storage_;
    catalog::Catalog& catalog_;
    utils::log::Transaction* transaction_;
    std::size_t batch_size_;
};

using ContextExecutor = ExecutorContext;

}
