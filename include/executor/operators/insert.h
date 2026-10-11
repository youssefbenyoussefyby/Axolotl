#pragma once
#include <executor/executor.h>
#include <storage/table_heap.h>
#include <executor/expression.h>


namespace AxolotlDB::executor {

class InsertExecutor : public AbstractExecutor {
   public:
    InsertExecutor(ExecutorContext* ctx, const optimizer::PhysicalInsert& plan);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalInsert& plan_;
    storage::Schema output_schema_;
    storage::TableHeap* heap_ = nullptr;
    bool done_ = false;
    storage::Schema table_schema_;

};

}  // namespace AxolotlDB::executor
