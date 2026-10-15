#pragma once
#include <executor/executor.h>

namespace AxolotlDB::executor {

// DDL: no child, no output rows. The table is created by the first Next(),
// never by Init(), like every other operator with a side effect.
class CreateTableExecutor : public AbstractExecutor {
   public:
    CreateTableExecutor(ExecutorContext* ctx, const optimizer::PhysicalCreateTable& plan);

    void Init() override;
    bool Next(std::vector<storage::Tuple>* tupleBatch,
              std::optional<std::vector<storage::RID>>& ridBatch) override;
    [[nodiscard]] const storage::Schema& GetOutputSchema() const override;

   private:
    const optimizer::PhysicalCreateTable& plan_;
    storage::Schema output_schema_;
    bool done_ = false;
};

}  // namespace AxolotlDB::executor
