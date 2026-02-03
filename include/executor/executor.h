#pragma once

#include <executor/executor_context.h>
#include <optimizer/physical_plan.h>
#include <storage/tuple.h>

#include <memory>
#include <optional>
#include <vector>

namespace AxolotlDB::executor {

class AbstractExecutor {
public:
    explicit AbstractExecutor(ExecutorContext* ctx) : context_(ctx) {}
    virtual ~AbstractExecutor() = default;

    virtual void Init() = 0;

    /// Produce the next batch of output rows.
    ///
    /// CONTRACT:
    ///  1. CLEAR ON ENTRY: Next() empties the output batch before filling it.
    ///  2. RETURN VALUE: true -> at least one row in batch; false -> exhausted.
    ///  3. BATCH SIZE: at most context_->GetBatchSize() items.
    ///  4. OWNERSHIP (N5 rule): Tuples are copies that own their bytes.
    ///  5. RIDS: Scans/DML populate ridBatch; other operators leave it empty or std::nullopt.
    virtual bool Next(std::vector<storage::Tuple>* tupleBatch,
                      std::optional<std::vector<storage::RID>>& ridBatch) = 0;

    virtual const storage::Schema& GetOutputSchema() const = 0;
    [[nodiscard]] const storage::Schema& OutputSchema() const { return GetOutputSchema(); }
    [[nodiscard]] const ExecutorContext* GetContext() const { return context_; }

protected:
    ExecutorContext* context_;
};

[[nodiscard]] std::unique_ptr<AbstractExecutor> CreateExecutor(const optimizer::PhysicalNode& plan, ExecutorContext* ctx);

}
