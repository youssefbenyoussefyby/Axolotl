#include <executor/coerce.h>
#include <executor/operators/insert.h>

namespace AxolotlDB::executor {

InsertExecutor::InsertExecutor(ExecutorContext* ctx, const optimizer::PhysicalInsert& plan)
    : AbstractExecutor(ctx), plan_(plan) {
    output_schema_ =
        storage::Schema({storage::ColumnDef{"rows_affected", storage::ColumnType::INT64}});
}

void InsertExecutor::Init() {
    if (!context_) {
        heap_ = nullptr;
        return;
    }

    if (!plan_.tableName.empty()) {
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableName);
    } else {
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableId);
    }

    table_schema_ = context_->GetCatalog().GetSchema(plan_.tableId);
    done_ = false;
}

const storage::Schema& InsertExecutor::GetOutputSchema() const {
    return output_schema_;
}

bool InsertExecutor::Next(std::vector<storage::Tuple>* tupleBatch,
                          std::optional<std::vector<storage::RID>>& ridBatch) {
    tupleBatch->clear();
    if (ridBatch.has_value()) {
        ridBatch->clear();  // one count row out, so there are no RIDs to align with it
    }

    if (heap_ == nullptr || done_)
        return false;

    std::vector<storage::Tuple> tuple_rows;
    for (size_t j = 0; j < plan_.rows.size(); j++) {
        std::vector<storage::Value> vals;

        for (size_t i = 0; i < table_schema_.ColumnCount(); i++)
            vals.push_back(storage::Value::Null(table_schema_.columns[i].type));

        for (size_t i = 0; i < plan_.rows[j].values.size(); i++) {
            vals[plan_.columnIndices[i]] = EvaluateConstant(*plan_.rows[j].values[i]);
        }
        // Every column, listed or not: converts numeric widths, rejects a
        // non-numeric value in a numeric column (and the reverse), and rejects
        // NULL in a NOT NULL column, including one the column list left out.
        for (size_t c = 0; c < table_schema_.ColumnCount(); c++) {
            vals[c] = CoerceToColumn(vals[c], table_schema_.columns[c]);
        }
        tuple_rows.push_back(storage::Tuple(storage::Tuple::Serialize(table_schema_, vals)));
    }
    size_t rows_effected = 0;
    constexpr std::size_t max_payload =
        storage::PAGE_SIZE - sizeof(storage::PageHeader) - sizeof(storage::SlotEntry);
    for (const auto& t : tuple_rows) {
        if (t.Span().size() > max_payload) {
            throw utils::ValueTooLargeException("", t.Span().size(), max_payload);
        }
    }
    for (const storage::Tuple& t : tuple_rows) {
        (void)heap_->Insert(t.Span());
        rows_effected++;
    }

    storage::Tuple result(
        storage::Tuple::Serialize(output_schema_, {storage::Value::Int64(rows_effected)}));
    tupleBatch->push_back(result);

    done_ = true;
    return true;
}

}  // namespace AxolotlDB::executor