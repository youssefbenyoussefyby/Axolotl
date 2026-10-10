#include <parser/ast/printer.h>

#include <iomanip>
#include <sstream>

namespace AxolotlDB::parser::ast {
std::string ToString(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::ADD:
            return "+";
        case BinaryOperator::SUBTRACT:
            return "-";
        case BinaryOperator::MULTIPLY:
            return "*";
        case BinaryOperator::DIVIDE:
            return "/";
        case BinaryOperator::EQUAL:
            return "=";
        case BinaryOperator::NOT_EQUAL:
            return "!=";
        case BinaryOperator::LESS:
            return "<";
        case BinaryOperator::LESS_EQUAL:
            return "<=";
        case BinaryOperator::GREATER:
            return ">";
        case BinaryOperator::GREATER_EQUAL:
            return ">=";
        case BinaryOperator::AND:
            return "AND";
        case BinaryOperator::OR:
            return "OR";
        case BinaryOperator::VECTOR_L2:
            return "<->";
        case BinaryOperator::VECTOR_COSINE:
            return "<=>";
    }
    return "<?>";
}

std::string ToString(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::NOT:
            return "NOT";
        case UnaryOperator::NEGATE:
            return "-";
    }
    return "<?>";
}

std::string ToString(JoinType jt) {
    switch (jt) {
        case JoinType::INNER:
            return "JOIN";
        case JoinType::LEFT:
            return "LEFT JOIN";
        case JoinType::RIGHT:
            return "RIGHT JOIN";
        case JoinType::FULL:
            return "FULL JOIN";
        case JoinType::CROSS:
            return "CROSS JOIN";
    }
    return "JOIN";
}

std::string ToString(const Expr& expr) {
    return std::visit(
        overloaded{[](const Literal& l) -> std::string {
                       return std::visit(
                           overloaded{[](int64_t i) { return std::to_string(i); },
                                      [](double d) {
                                          std::ostringstream ss;
                                          ss << d;
                                          return ss.str();
                                      },
                                      [](const std::string& s) { return "'" + s + "'"; },
                                      [](bool b) {
                                          return b ? std::string("true") : std::string("false");
                                      },
                                      [](std::nullptr_t) { return std::string("NULL"); }},
                           l.value);
                   },
                   [](const ColumnRef& col) -> std::string {
                       if (col.table.has_value()) {
                           return *col.table + "." + col.column;
                       }
                       return col.column;
                   },
                   [](const BinaryOp& bi) -> std::string {
                       return ToString(*bi.left) + " " + ToString(bi.biop) + " " +
                              ToString(*bi.right);
                   },
                   [](const UnaryOp& uo) -> std::string {
                       if (uo.uop == UnaryOperator::NOT) {
                           return "NOT " + ToString(*uo.expr);
                       }
                       return "-" + ToString(*uo.expr);
                   },
                   [](const FunctionCall& fn) -> std::string {
                       std::string res = fn.name + "(";
                       for (size_t i = 0; i < fn.args.size(); i++) {
                           if (i > 0)
                               res += ", ";
                           res += ToString(*fn.args[i]);
                       }
                       res += ")";
                       return res;
                   },
                   [](const Star& st) -> std::string {
                       if (st.qualifier.has_value()) {
                           return *st.qualifier + ".*";
                       }
                       return "*";
                   }},
        expr.node);
}

std::string ToString(const SelectStmt& stmt) {
    std::string sql = "SELECT ";
    if (stmt.isDistinct)
        sql += "DISTINCT ";
    for (size_t i = 0; i < stmt.projection.size(); i++) {
        if (i > 0)
            sql += ", ";
        sql += ToString(*stmt.projection[i].expr);
        if (stmt.projection[i].alias.has_value()) {
            sql += " AS " + *stmt.projection[i].alias;
        }
    }

    if (stmt.fromTable.has_value()) {
        sql += " FROM " + stmt.fromTable->table.name;
        if (stmt.fromTable->alias.has_value()) {
            sql += " AS " + *stmt.fromTable->alias;
        }
    }

    for (const auto& join : stmt.joins) {
        sql += " " + ToString(join.type) + " " + join.table.table.name;
        if (join.table.alias.has_value()) {
            sql += " AS " + *join.table.alias;
        }
        if (join.onCondition) {
            sql += " ON " + ToString(*join.onCondition);
        }
    }

    if (stmt.where) {
        sql += " WHERE " + ToString(*stmt.where);
    }

    if (!stmt.groupBy.empty()) {
        sql += " GROUP BY ";
        for (size_t i = 0; i < stmt.groupBy.size(); i++) {
            if (i > 0)
                sql += ", ";
            sql += ToString(*stmt.groupBy[i]);
        }
    }

    if (stmt.having) {
        sql += " HAVING " + ToString(*stmt.having);
    }

    if (!stmt.orderBy.empty()) {
        sql += " ORDER BY ";
        for (size_t i = 0; i < stmt.orderBy.size(); i++) {
            if (i > 0)
                sql += ", ";
            sql += ToString(*stmt.orderBy[i].expr);
            sql += stmt.orderBy[i].isDescending ? " DESC" : " ASC";
        }
    }

    if (stmt.limit) {
        sql += " LIMIT " + ToString(*stmt.limit);
    }

    return sql;
}

std::string ToString(const InsertStmt& stmt) {
    std::string sql = "INSERT INTO " + stmt.table.name;
    if (stmt.columns.has_value()) {
        sql += " (";
        for (size_t i = 0; i < stmt.columns->size(); i++) {
            if (i > 0)
                sql += ", ";
            sql += (*stmt.columns)[i];
        }
        sql += ")";
    }
    sql += " VALUES ";
    for (size_t r = 0; r < stmt.valueRows.size(); r++) {
        if (r > 0)
            sql += ", ";
        sql += "(";
        for (size_t c = 0; c < stmt.valueRows[r].size(); c++) {
            if (c > 0)
                sql += ", ";
            sql += ToString(*stmt.valueRows[r][c]);
        }
        sql += ")";
    }
    return sql;
}

std::string ToString(const UpdateStmt& stmt) {
    std::string sql = "UPDATE " + stmt.table.name + " SET ";
    for (size_t i = 0; i < stmt.assignments.size(); i++) {
        if (i > 0)
            sql += ", ";
        sql += stmt.assignments[i].first + " = " + ToString(*stmt.assignments[i].second);
    }
    if (stmt.where) {
        sql += " WHERE " + ToString(*stmt.where);
    }
    return sql;
}

std::string ToString(const DeleteStmt& stmt) {
    std::string sql = "DELETE FROM " + stmt.table.name;
    if (stmt.where) {
        sql += " WHERE " + ToString(*stmt.where);
    }
    return sql;
}

std::string ToString(const CreateTableStmt& stmt) {
    std::string sql = "CREATE TABLE " + stmt.table.name + " (";
    for (size_t i = 0; i < stmt.columns.size(); i++) {
        if (i > 0)
            sql += ", ";
        sql += stmt.columns[i].name + " " + stmt.columns[i].typeName;
        if (stmt.columns[i].typeSize.has_value()) {
            sql += "(" + std::to_string(*stmt.columns[i].typeSize) + ")";
        }
        if (stmt.columns[i].isPrimaryKey) {
            sql += " PRIMARY KEY";
        }
        if (stmt.columns[i].isNotNull) {
            sql += " NOT NULL";
        }
    }
    sql += ")";
    return sql;
}

std::string ToString(const DropTableStmt& stmt) {
    return "DROP TABLE " + stmt.table.name;
}

std::string ToString(const Stmt& stmt) {
    return std::visit([](const auto& s) { return ToString(s); }, stmt.node);
}

}  // namespace AxolotlDB::parser::ast
