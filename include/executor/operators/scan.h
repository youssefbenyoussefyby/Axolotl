#pragma once

#include <executor/executor.h>
#include <optimizer/physical_plan.h>
#include <storage/table_heap.h>

namespace AxolotlDB::executor {

class SeqScanExecutor : public AbstractExecutor {
   public:
    SeqScanExecutor(ExecutorContext* ctx, const optimizer::PhysicalSeqScan& plan);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalSeqScan& plan_;
    storage::Schema output_schema_;
    storage::TableHeap* heap_ = nullptr;
    storage::TableHeap::Iterator cursor_;
    storage::TableHeap::Iterator end_;
};

}  // namespace AxolotlDB::executor
