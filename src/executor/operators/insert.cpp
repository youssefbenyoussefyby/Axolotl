#include <executor/operators/insert.h>

namespace AxolotlDB::executor {

InsertExecutor::InsertExecutor(ExecutorContext* ctx, const optimizer::PhysicalInsert& plan): AbstractExecutor(ctx), plan_(plan) {

        output_schema_ = storage::Schema({storage::ColumnDef{"rows_affected",storage::ColumnType::INT64}});


}

void InsertExecutor::Init() {

    if(!context_)
    {
        heap_ = nullptr;
        return ;
    }

    if(!plan_.tableName.empty()){
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableName);
    }else{
        heap_ = &context_->GetCatalog().GetTableHeap(plan_.tableId);
    }


    table_schema_ = context_->GetCatalog().GetSchema(plan_.tableId);
    done_ = false;
    
}


const storage::Schema& InsertExecutor::GetOutputSchema() const {return output_schema_;}

bool InsertExecutor::Next(std::vector<storage::Tuple>* tupleBatch,std::optional<std::vector<storage::RID>>& ridBatch ){
    tupleBatch->clear();

    if(heap_ == nullptr || done_)
        return false;

    std::vector<storage::Tuple> tuple_rows;
    for (size_t j = 0; j < plan_.rows.size(); j++)
    {
        std::vector<storage::Value>vals; 

        for (size_t i = 0; i < table_schema_.ColumnCount(); i++)
            vals.push_back(storage::Value::Null(table_schema_.columns[i].type));

        for (size_t i = 0; i < plan_.rows[j].values.size(); i++)
        {
           storage::Value val =  EvaluateConstant(*plan_.rows[j].values[i]);
            if (val.IsNull() && !table_schema_.columns[plan_.columnIndices[i]].nullable) {
                throw utils::AxolotException("Column '" + table_schema_.columns[plan_.columnIndices[i]].name + "' is NOT NULL");
            }
            if (val.Type() > table_schema_.columns[plan_.columnIndices[i]].type) {
                if(val.Type() == storage::ColumnType::INT64)vals[plan_.columnIndices[i]] = storage::Value::Int32(static_cast<int32_t>(val.AsInt64()));
                if(val.Type() == storage::ColumnType::FLOAT64)vals[plan_.columnIndices[i]] = storage::Value::Float64(static_cast<float>(val.AsFloat64()));
            }
            else
                vals[plan_.columnIndices[i]] = val;
        }
       tuple_rows.push_back(storage::Tuple(storage::Tuple::Serialize(table_schema_,vals)));
    }
    size_t rows_effected = 0;
    constexpr std::size_t max_payload = storage::PAGE_SIZE - sizeof(storage::PageHeader) - sizeof(storage::SlotEntry);
    for (const auto& t : tuple_rows) {
        if (t.Span().size() > max_payload) {
            throw utils::ValueTooLargeException("", t.Span().size(), max_payload);
        }
    }
    for (storage::Tuple t:tuple_rows)
    {
        heap_->Insert(t.Span());
        rows_effected++;
    }

    storage::Tuple result(storage::Tuple::Serialize(output_schema_,{storage::Value::Int64(rows_effected)}));
    tupleBatch->push_back(result);


    

    done_ = true;
    return true;
    

}

}  // namespace AxolotlDB::executor