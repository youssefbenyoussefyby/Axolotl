#include <executor/operators/scan.h>

namespace AxolotlDB::executor {

SeqScanExecutor::SeqScanExecutor(ExecutorContext* ctx, const optimizer::PhysicalSeqScan& plan)
    : AbstractExecutor(ctx), plan_(plan) {
    if (plan_.schema.ColumnCount() > 0) {
        output_schema_ = plan_.schema;
    } else {
        std::vector<storage::ColumnDef> cols;
        cols.reserve(plan_.outputSchema.size());
        for (const auto& col : plan_.outputSchema) {
            cols.emplace_back(col.name, col.type);
        }
        output_schema_ = storage::Schema(std::move(cols));
    }
}

void SeqScanExecutor::Init() {
    if (!context_) {
        heap_ = nullptr;
        return;
    }
    if (!plan_.tableName.empty()) {
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableName);
    } else {
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableId);
    }
    cursor_ = heap_->begin();
    end_ = heap_->end();
}

bool SeqScanExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                           std::optional<std::vector<storage::RID>>& ridBatch) {
    if (!tupleBatch) {
        return false;
    }
    tupleBatch->clear();
    if (ridBatch.has_value()) {
        ridBatch->clear();
    }
    if (!heap_) {
        return false;
    }

    const std::size_t batchSize = context_ ? context_->GetBatchSize() : 1024;
    while (cursor_ != end_ && tupleBatch->size() < batchSize) {
        storage::RID rid = *cursor_;
        storage::ReadPageGuard guard;
        auto span = heap_->Get(rid, guard);
        std::vector<std::byte> copyBytes(span.begin(), span.end());
        tupleBatch->emplace_back(std::move(copyBytes), rid);
        if (ridBatch.has_value()) {
            ridBatch->push_back(rid);
        }
        cursor_++;
    }
    return !tupleBatch->empty();
}

const storage::Schema& SeqScanExecutor::GetOutputSchema() const {
    return output_schema_;
}

}  // namespace AxolotlDB::executor