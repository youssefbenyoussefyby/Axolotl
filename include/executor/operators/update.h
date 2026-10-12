#pragma once
#include <executor/executor.h>
#include <executor/expression.h>
#include <storage/table_heap.h>

namespace AxolotlDB::executor {

class UpdateExecutor : public AbstractExecutor {
   public:
    UpdateExecutor(ExecutorContext* ctx, const optimizer::PhysicalUpdate& plan,
                   std::unique_ptr<executor::AbstractExecutor> child);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalUpdate& plan_;
    std::unique_ptr<executor::AbstractExecutor> child_;
    storage::Schema output_schema_;
    storage::TableHeap* heap_ = nullptr;
    bool done_ = false;
    storage::Schema table_schema_;
};

}  // namespace AxolotlDB::executor