
#include <executor/coerce.h>
#include <executor/operators/update.h>

namespace AxolotlDB::executor {

UpdateExecutor::UpdateExecutor(executor::ExecutorContext* ctx,
                               const optimizer::PhysicalUpdate& plan,
                               std::unique_ptr<AbstractExecutor> child)
    : AbstractExecutor(ctx), plan_(plan), child_(std::move(child)) {
    output_schema_ =
        storage::Schema({storage::ColumnDef{"rows_affected", storage::ColumnType::INT64}});
}

const storage::Schema& UpdateExecutor::GetOutputSchema() const {
    return output_schema_;
}
void UpdateExecutor::Init() {
    if (!plan_.tableName.empty())
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableName);
    else
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableId);

    done_ = false;
    table_schema_ = context_->GetCatalog().GetSchema(plan_.tableId);
    if (child_)
        child_->Init();
}

bool UpdateExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                          std::optional<std::vector<storage::RID>>& ridBatch) {
    tupleBatch->clear();
    if (ridBatch.has_value())
        ridBatch->clear();
    if (!heap_ || !child_ || done_)
        return false;

    size_t rows = 0;

    std::vector<storage::Tuple> child_tuples;
    std::optional<std::vector<storage::RID>> child_rid = std::vector<storage::RID>{};
    std::vector<std::pair<storage::RID, std::vector<std::byte>>> updated_rows;
    while (child_->Next(&child_tuples, child_rid)) {
        for (size_t j = 0; j < child_rid->size(); j++) {
            auto row =
                storage::Tuple::Deserialize(child_->GetOutputSchema(), child_tuples[j].Span());

            for (size_t i = 0; i < plan_.assignments.size(); i++) {
                row[plan_.assignments[i].columnIndex] = Evaluate(
                    *plan_.assignments[i].value, child_tuples[j], child_->GetOutputSchema());
            }

            for (size_t i = 0; i < table_schema_.ColumnCount(); i++) {
                row[i] = CoerceToColumn(row[i], table_schema_.columns[i]);
            }

            updated_rows.push_back(
                {child_rid->at(j), storage::Tuple::Serialize(table_schema_, row)});
        }
    }

    for (const auto& [rid, bytes] : updated_rows) {
        (void)heap_->Update(rid, bytes);
        rows++;
    }

    tupleBatch->push_back(
        storage::Tuple(storage::Tuple::Serialize(output_schema_, {storage::Value::Int64(rows)})));

    done_ = true;
    return true;
}

}  // namespace AxolotlDB::executor