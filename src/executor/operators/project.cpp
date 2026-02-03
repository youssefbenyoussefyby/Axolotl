#include <executor/operators/project.h>

namespace AxolotlDB::executor {

ProjectionExecutor::ProjectionExecutor(ExecutorContext* ctx, const optimizer::PhysicalProject& plan,
                                       std::unique_ptr<AbstractExecutor> child)
    : AbstractExecutor(ctx), plan_(plan), child_(std::move(child)) {
    std::vector<storage::ColumnDef> cols;
    cols.reserve(plan_.projections.size());
    for (const auto& p : plan_.projections) {
        cols.emplace_back(p.name, p.expr->type);
    }
    output_schema_ = storage::Schema(std::move(cols));
}

void ProjectionExecutor::Init() {
    if (child_) {
        child_->Init();
    }
}

bool ProjectionExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                              std::optional<std::vector<storage::RID>>& ridBatch) {
    if (!tupleBatch) {
        return false;
    }
    tupleBatch->clear();
    if (ridBatch.has_value()) {
        ridBatch->clear();
    }
    if (!child_) {
        return false;
    }

    std::vector<storage::Tuple> childTuples;
    std::optional<std::vector<storage::RID>> childRids = std::nullopt;
    if (!child_->Next(&childTuples, childRids)) {
        return false;
    }

    const auto& childSchema = child_->GetOutputSchema();
    tupleBatch->reserve(childTuples.size());

    for (const auto& inTuple : childTuples) {
        std::vector<storage::Value> outVals;
        outVals.reserve(plan_.projections.size());
        for (const auto& proj : plan_.projections) {
            outVals.push_back(Evaluate(*proj.expr, inTuple, childSchema));
        }
        auto bytes = storage::Tuple::Serialize(output_schema_, outVals);
        tupleBatch->emplace_back(std::move(bytes));
    }

    return !tupleBatch->empty();
}

const storage::Schema& ProjectionExecutor::GetOutputSchema() const {
    return output_schema_;
}

}