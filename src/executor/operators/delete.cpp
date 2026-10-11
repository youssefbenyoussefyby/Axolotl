
#include <executor/operators/delete.h>

namespace AxolotlDB::executor {

DeleteExecutor::DeleteExecutor(executor::ExecutorContext* ctx,
                               const optimizer::PhysicalDelete& plan,
                               std::unique_ptr<AbstractExecutor> child)
    : AbstractExecutor(ctx), plan_(plan), child_(std::move(child)) {
    output_schema_ =
        storage::Schema({storage::ColumnDef{"rows_affected", storage::ColumnType::INT64}});
}

const storage::Schema& DeleteExecutor::GetOutputSchema() const {
    return output_schema_;
}
void DeleteExecutor::Init() {
    if (!plan_.tableName.empty())
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableName);
    else
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableId);

    done_ = false;
    if (child_)
        child_->Init();
}

bool DeleteExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                          std::optional<std::vector<storage::RID>>& ridBatch) {
    tupleBatch->clear();
    if (ridBatch.has_value())
        ridBatch->clear();
    if (!heap_ || !child_ || done_)
        return false;

    size_t rows = 0;

    std::vector<storage::Tuple> child_tuples;
    std::optional<std::vector<storage::RID>> child_rid = std::vector<storage::RID>{};
    while (child_->Next(&child_tuples, child_rid)) {
        if (!child_rid.has_value())
            return false;
        for (size_t i = 0; i < child_rid->size(); i++) {
            heap_->Delete(child_rid->at(i));
            rows++;
        }
    }

    tupleBatch->push_back(
        storage::Tuple(storage::Tuple::Serialize(output_schema_, {storage::Value::Int64(rows)})));

    done_ = true;
    return true;
}

}  // namespace AxolotlDB::executor