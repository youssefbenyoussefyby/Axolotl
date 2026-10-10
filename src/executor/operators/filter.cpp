#include <executor/operators/filter.h>

#include <stdexcept>

namespace AxolotlDB::executor {

FilterExecutor::FilterExecutor(ExecutorContext* ctx, const optimizer::PhysicalFilter& plan,
                               std::unique_ptr<AbstractExecutor> child)
    : AbstractExecutor(ctx), plan_(plan), child_(std::move(child)) {}

void FilterExecutor::Init() {
    pending_tuples_.clear();
    pending_rids_.reset();
    pending_pos_ = 0;
    if (child_) {
        child_->Init();
    }
}

bool FilterExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                          std::optional<std::vector<storage::RID>>& ridBatch) {
    if (!tupleBatch) {
        return false;
    }
    tupleBatch->clear();
    const bool wantRids = ridBatch.has_value();
    if (wantRids) {
        ridBatch->clear();
    }
    if (!child_) {
        return false;
    }

    if (pending_pos_ < pending_tuples_.size() && wantRids && !pending_rids_.has_value()) {
        throw std::logic_error("FilterExecutor: RIDs requested but pending batch has none");
    }

    const std::size_t batchSize = context_ ? context_->GetBatchSize() : 1024;
    const auto& childSchema = child_->GetOutputSchema();

    while (tupleBatch->size() < batchSize) {
        if (pending_pos_ >= pending_tuples_.size()) {
            pending_pos_ = 0;
            pending_rids_.reset();
            if (wantRids) {
                pending_rids_ = std::vector<storage::RID>{};
            }
            if (!child_->Next(&pending_tuples_, pending_rids_)) {
                pending_tuples_.clear();
                pending_rids_.reset();
                break;
            }
            if (wantRids &&
                (!pending_rids_.has_value() || pending_rids_->size() != pending_tuples_.size())) {
                pending_tuples_.clear();
                pending_rids_.reset();
                throw std::logic_error("FilterExecutor: child did not return one RID per tuple");
            }
        }

        while (pending_pos_ < pending_tuples_.size() && tupleBatch->size() < batchSize) {
            const std::size_t i = pending_pos_++;
            bool keep = true;
            if (plan_.predicate) {
                storage::Value val = Evaluate(*plan_.predicate, pending_tuples_[i], childSchema);
                keep = IsTrue(val);
            }
            if (keep) {
                tupleBatch->push_back(std::move(pending_tuples_[i]));
                if (wantRids) {
                    ridBatch->push_back((*pending_rids_)[i]);
                }
            }
        }
    }

    return !tupleBatch->empty();
}

const storage::Schema& FilterExecutor::GetOutputSchema() const {
    return child_->GetOutputSchema();
}

}  // namespace AxolotlDB::executor