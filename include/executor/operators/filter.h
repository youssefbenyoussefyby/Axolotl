#pragma once

#include <executor/executor.h>
#include <executor/expression.h>
#include <optimizer/physical_plan.h>

namespace AxolotlDB::executor {

class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(ExecutorContext* ctx, const optimizer::PhysicalFilter& plan,
                   std::unique_ptr<AbstractExecutor> child);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

private:
    std::vector<storage::Tuple> pending_tuples_;
    std::optional<std::vector<storage::RID>> pending_rids_;
    std::size_t pending_pos_ = 0;  
    
    const optimizer::PhysicalFilter& plan_;
    std::unique_ptr<AbstractExecutor> child_;
};

}
