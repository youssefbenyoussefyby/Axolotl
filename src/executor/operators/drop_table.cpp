#include <executor/operators/drop_table.h>

namespace AxolotlDB::executor {

DropTableExecutor::DropTableExecutor(ExecutorContext* ctx, const optimizer::PhysicalDropTable& plan)
    : AbstractExecutor(ctx), plan_(plan) {}

const storage::Schema& DropTableExecutor::GetOutputSchema() const {
    return output_schema_;
}

void DropTableExecutor::Init() {
    done_ = false;
}

bool DropTableExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                             std::optional<std::vector<storage::RID>>& ridBatch) {
    tupleBatch->clear();
    if (ridBatch.has_value())
        ridBatch->clear();
    if (!context_ || done_)
        return false;

    if (!plan_.tableName.empty())
        context_->GetCatalog().DropTable(plan_.tableName);
    else
        context_->GetCatalog().DropTable(context_->GetCatalog().GetTableName(plan_.tableId));
    done_ = true;
    return false;
}

}  // namespace AxolotlDB::executor
