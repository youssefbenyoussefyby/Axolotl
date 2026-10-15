#include <executor/operators/create_table.h>

namespace AxolotlDB::executor {

CreateTableExecutor::CreateTableExecutor(ExecutorContext* ctx,
                                         const optimizer::PhysicalCreateTable& plan)
    : AbstractExecutor(ctx), plan_(plan) {}

const storage::Schema& CreateTableExecutor::GetOutputSchema() const {
    return output_schema_;
}

void CreateTableExecutor::Init() {
    done_ = false;
}

bool CreateTableExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                               std::optional<std::vector<storage::RID>>& ridBatch) {
    tupleBatch->clear();
    if (ridBatch.has_value())
        ridBatch->clear();
    if (!context_ || done_)
        return false;

    (void)context_->GetCatalog().CreateTable(plan_.tableName, plan_.schema);
    done_ = true;
    return false;
}

}  // namespace AxolotlDB::executor
