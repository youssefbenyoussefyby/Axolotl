#pragma once

#include <executor/executor.h>
#include <executor/expression.h>
#include <optimizer/physical_plan.h>

namespace AxolotlDB::executor {

class ProjectionExecutor : public AbstractExecutor {
   public:
    ProjectionExecutor(ExecutorContext* ctx, const optimizer::PhysicalProject& plan,
                       std::unique_ptr<AbstractExecutor> child);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalProject& plan_;
    std::unique_ptr<AbstractExecutor> child_;
    storage::Schema output_schema_;
};

}  // namespace AxolotlDB::executor
