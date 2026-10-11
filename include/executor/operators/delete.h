#pragma once
#include <executor/executor.h>
#include <executor/expression.h>
#include <storage/table_heap.h>

namespace AxolotlDB::executor {

class DeleteExecutor : public AbstractExecutor {
   public:
    DeleteExecutor(ExecutorContext* ctx, const optimizer::PhysicalDelete& plan,
                   std::unique_ptr<executor::AbstractExecutor> child);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalDelete& plan_;
    std::unique_ptr<executor::AbstractExecutor> child_;
    storage::Schema output_schema_;
    storage::TableHeap* heap_ = nullptr;
    bool done_ = false;
};

}  // namespace AxolotlDB::executor