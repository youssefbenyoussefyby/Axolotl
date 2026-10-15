#pragma once
#include <executor/executor.h>

namespace AxolotlDB::executor {

// DDL: no child, no output rows. The table is dropped by the first Next().
class DropTableExecutor : public AbstractExecutor {
   public:
    DropTableExecutor(ExecutorContext* ctx, const optimizer::PhysicalDropTable& plan);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalDropTable& plan_;
    storage::Schema output_schema_;
    bool done_ = false;
};

}  // namespace AxolotlDB::executor
