#include <optimizer/physical_plan.h>

#include <stdexcept>
#include <utility>

namespace AxolotlDB::optimizer
{
    namespace
    {
        PhysicalNodePtr MapNode(LogicalNode& n)
        {
            switch (n.Type()) {
            case LogicalNodeType::Scan: {
                auto& l = static_cast<LogicalScan&>(n);
                auto p = std::make_unique<PhysicalSeqScan>();
                p->tableId = l.tableId;
                p->tableName = std::move(l.tableName);
                p->alias = std::move(l.alias);
                p->schema = std::move(l.schema);
                return p;
            }
            case LogicalNodeType::Filter: {
                auto& l = static_cast<LogicalFilter&>(n);
                auto p = std::make_unique<PhysicalFilter>();
                p->predicate = std::move(l.predicate);
                return p;
            }
            case LogicalNodeType::Project: {
                auto& l = static_cast<LogicalProject&>(n);
                auto p = std::make_unique<PhysicalProject>();
                p->projections = std::move(l.projections);
                return p;
            }
            case LogicalNodeType::NestedLoopJoin: {
                auto& l = static_cast<LogicalNestedLoopJoin&>(n);
                PhysicalNodePtr result;
                PhysicalJoin* join = nullptr;
                if (SelectJoinAlgorithm(l) == JoinAlgorithm::Hash) {
                    auto p = std::make_unique<PhysicalHashJoin>();
                    join = p.get();
                    result = std::move(p);
                } else {
                    auto p = std::make_unique<PhysicalNestedLoopJoin>();
                    join = p.get();
                    result = std::move(p);
                }
                join->joinType = l.joinType;
                join->condition = std::move(l.condition);
                return result;
            }
            case LogicalNodeType::Aggregate: {
                auto& l = static_cast<LogicalAggregate&>(n);
                auto p = std::make_unique<PhysicalHashAggregate>();
                p->groupBy = std::move(l.groupBy);
                p->aggregates = std::move(l.aggregates);
                p->having = std::move(l.having);
                return p;
            }
            case LogicalNodeType::Sort: {
                auto& l = static_cast<LogicalSort&>(n);
                auto p = std::make_unique<PhysicalSort>();
                p->keys = std::move(l.keys);
                return p;
            }
            case LogicalNodeType::Limit: {
                auto& l = static_cast<LogicalLimit&>(n);
                auto p = std::make_unique<PhysicalLimit>();
                p->limit = std::move(l.limit);
                p->offset = std::move(l.offset);
                return p;
            }
            case LogicalNodeType::Insert: {
                auto& l = static_cast<LogicalInsert&>(n);
                auto p = std::make_unique<PhysicalInsert>();
                p->tableId = l.tableId;
                p->tableName = std::move(l.tableName);
                p->columnIndices = std::move(l.columnIndices);
                p->targetColumns = std::move(l.targetColumns);
                p->rows = std::move(l.rows);
                return p;
            }
            case LogicalNodeType::Update: {
                auto& l = static_cast<LogicalUpdate&>(n);
                auto p = std::make_unique<PhysicalUpdate>();
                p->tableId = l.tableId;
                p->tableName = std::move(l.tableName);
                p->assignments = std::move(l.assignments);
                return p;
            }
            case LogicalNodeType::Delete: {
                auto& l = static_cast<LogicalDelete&>(n);
                auto p = std::make_unique<PhysicalDelete>();
                p->tableId = l.tableId;
                p->tableName = std::move(l.tableName);
                return p;
            }
            case LogicalNodeType::CreateTable: {
                auto& l = static_cast<LogicalCreateTable&>(n);
                auto p = std::make_unique<PhysicalCreateTable>();
                p->tableName = std::move(l.tableName);
                p->schema = std::move(l.schema);
                return p;
            }
            case LogicalNodeType::DropTable: {
                auto& l = static_cast<LogicalDropTable&>(n);
                auto p = std::make_unique<PhysicalDropTable>();
                p->tableId = l.tableId;
                p->tableName = std::move(l.tableName);
                return p;
            }
            }

            throw std::logic_error("ToPhysicalPlan: no physical mapping for logical node type");
        }

        const char* NodeName(PhysicalNodeType t)
        {
            switch (t) {
            case PhysicalNodeType::SeqScan:        return "SeqScan";
            case PhysicalNodeType::Filter:         return "Filter";
            case PhysicalNodeType::Project:        return "Project";
            case PhysicalNodeType::NestedLoopJoin: return "NestedLoopJoin";
            case PhysicalNodeType::HashJoin:       return "HashJoin";
            case PhysicalNodeType::HashAggregate:  return "HashAggregate";
            case PhysicalNodeType::Sort:           return "Sort";
            case PhysicalNodeType::Limit:          return "Limit";
            case PhysicalNodeType::Insert:         return "Insert";
            case PhysicalNodeType::Update:         return "Update";
            case PhysicalNodeType::Delete:         return "Delete";
            case PhysicalNodeType::CreateTable:    return "CreateTable";
            case PhysicalNodeType::DropTable:      return "DropTable";
            }
            return "Unknown";
        }

        void Print(const PhysicalNode& n, int depth, std::string& out)
        {
            out.append(static_cast<std::size_t>(depth) * 2, ' ');
            out += "Physical";
            out += NodeName(n.Type());

            switch (n.Type()) {
            case PhysicalNodeType::SeqScan:
                out += "(" + static_cast<const PhysicalSeqScan&>(n).tableName + ")";
                break;
            case PhysicalNodeType::Insert:
                out += "(" + static_cast<const PhysicalInsert&>(n).tableName + ")";
                break;
            case PhysicalNodeType::Update:
                out += "(" + static_cast<const PhysicalUpdate&>(n).tableName + ")";
                break;
            case PhysicalNodeType::Delete:
                out += "(" + static_cast<const PhysicalDelete&>(n).tableName + ")";
                break;
            default:
                break;
            }
            out += '\n';

            for (const auto& child : n.children) {
                Print(*child, depth + 1, out);
            }
        }
    } // namespace

    PhysicalNodePtr ToPhysicalPlan(LogicalNodePtr logical)
    {
        if (!logical) {
            throw std::invalid_argument("ToPhysicalPlan: null logical node");
        }


        auto schema = logical->OutputSchema();

        PhysicalNodePtr out = MapNode(*logical);
        out->outputSchema = std::move(schema);

        for (auto& child : logical->children) {
            out->children.push_back(ToPhysicalPlan(std::move(child)));
        }
        return out;
    }

    std::string PlanToString(const PhysicalNode& root)
    {
        std::string out;
        Print(root, 0, out);
        return out;
    }
}