#include "sqlconduit/sql_builder.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <sstream>

namespace sqlconduit::sql {
    namespace {
        bool isNull(const common::Value &value) {
            return std::holds_alternative<std::nullptr_t>(value);
        }

        common::Status error(std::string message) {
            return common::Status::error(common::ErrorCode::QueryError,
                                         "sql builder: " + std::move(message));
        }

        bool validIdentifier(const std::string &identifier) {
            if (identifier.empty() || identifier.find('\0') != std::string::npos) return false;
            std::size_t begin = 0;
            while (begin < identifier.size()) {
                const auto dot = identifier.find('.', begin);
                const auto end = dot == std::string::npos ? identifier.size() : dot;
                if (end == begin) return false;
                if (identifier.compare(begin, end - begin, "*") == 0) return false;
                for (std::size_t i = begin; i < end; ++i)
                    if (static_cast<unsigned char>(identifier[i]) < 0x20) return false;
                begin = end + 1;
            }
            return identifier.back() != '.';
        }

        bool validDialect(const common::util::Dialect dialect) {
            using common::util::Dialect;
            return dialect == Dialect::Auto || dialect == Dialect::MySQL ||
                   dialect == Dialect::Postgres || dialect == Dialect::SqlServer ||
                   dialect == Dialect::Oracle;
        }

        std::string quote(const std::string &identifier, const common::util::Dialect dialect) {
            return common::util::quoteIdent(identifier, dialect);
        }

        const char *compareText(const CompareOperator op) {
            switch (op) {
                case CompareOperator::Equal: return "=";
                case CompareOperator::NotEqual: return "<>";
                case CompareOperator::Less: return "<";
                case CompareOperator::LessOrEqual: return "<=";
                case CompareOperator::Greater: return ">";
                case CompareOperator::GreaterOrEqual: return ">=";
                case CompareOperator::Like: return "LIKE";
            }
            return "";
        }

        common::Status renderCondition(const Condition &condition,
                                       const common::util::Dialect dialect,
                                       common::Params &params, std::string &out,
                                       const bool nested = false) {
            using Kind = Condition::Kind;
            const auto requireField = [&]() -> common::Status {
                return validIdentifier(condition.field())
                           ? common::Status::OK()
                           : error("condition field is empty or invalid");
            };

            switch (condition.kind()) {
                case Kind::Compare: {
                    if (const auto status = requireField(); !status.ok()) return status;
                    if (condition.values().size() != 1)
                        return error("comparison requires exactly one value");
                    const auto &value = condition.values().front();
                    if (isNull(value)) {
                        if (condition.compareOperator() == CompareOperator::Equal) {
                            out += quote(condition.field(), dialect) + " IS NULL";
                            return common::Status::OK();
                        }
                        if (condition.compareOperator() == CompareOperator::NotEqual) {
                            out += quote(condition.field(), dialect) + " IS NOT NULL";
                            return common::Status::OK();
                        }
                        return error("NULL supports only equality and inequality comparisons");
                    }
                    if (compareText(condition.compareOperator())[0] == '\0')
                        return error("unknown comparison operator");
                    out += quote(condition.field(), dialect) + " " +
                           compareText(condition.compareOperator()) + " ?";
                    params.push_back(value);
                    return common::Status::OK();
                }
                case Kind::In:
                case Kind::NotIn: {
                    if (const auto status = requireField(); !status.ok()) return status;
                    const bool negate = condition.kind() == Kind::NotIn;
                    if (condition.values().empty()) {
                        out += negate ? "1 = 1" : "1 = 0";
                        return common::Status::OK();
                    }
                    out += quote(condition.field(), dialect) + (negate ? " NOT IN (" : " IN (");
                    for (std::size_t i = 0; i < condition.values().size(); ++i) {
                        if (i) out += ", ";
                        out += '?';
                        params.push_back(condition.values()[i]);
                    }
                    out += ')';
                    return common::Status::OK();
                }
                case Kind::Between: {
                    if (const auto status = requireField(); !status.ok()) return status;
                    if (condition.values().size() != 2 || isNull(condition.values()[0]) ||
                        isNull(condition.values()[1]))
                        return error("BETWEEN requires two non-NULL values");
                    out += quote(condition.field(), dialect) + " BETWEEN ? AND ?";
                    params.push_back(condition.values()[0]);
                    params.push_back(condition.values()[1]);
                    return common::Status::OK();
                }
                case Kind::IsNull:
                case Kind::IsNotNull: {
                    if (const auto status = requireField(); !status.ok()) return status;
                    out += quote(condition.field(), dialect);
                    out += condition.kind() == Kind::IsNull ? " IS NULL" : " IS NOT NULL";
                    return common::Status::OK();
                }
                case Kind::All:
                case Kind::Any: {
                    if (condition.children().empty())
                        return error("condition group cannot be empty");
                    if (nested) out += '(';
                    const char *separator = condition.kind() == Kind::All ? " AND " : " OR ";
                    for (std::size_t i = 0; i < condition.children().size(); ++i) {
                        if (i) out += separator;
                        if (const auto status = renderCondition(condition.children()[i], dialect,
                                                                params, out, true);
                            !status.ok())
                            return status;
                    }
                    if (nested) out += ')';
                    return common::Status::OK();
                }
                case Kind::Not: {
                    if (condition.children().size() != 1)
                        return error("NOT requires exactly one condition");
                    out += "NOT (";
                    if (const auto status = renderCondition(condition.children().front(), dialect,
                                                            params, out, false);
                        !status.ok())
                        return status;
                    out += ')';
                    return common::Status::OK();
                }
                case Kind::Invalid:
                    return error("condition is empty");
            }
            return error("unknown condition kind");
        }

        common::Status appendWhere(const std::vector<Condition> &conditions,
                                   const common::util::Dialect dialect,
                                   common::Params &params, std::string &sql) {
            if (conditions.empty()) return common::Status::OK();
            sql += " WHERE ";
            return renderCondition(Condition::all(conditions), dialect, params, sql);
        }

        common::Status validateUniqueFields(const std::vector<FieldValue> &values) {
            std::set<std::string> seen;
            for (const auto &entry: values) {
                if (!validIdentifier(entry.field)) return error("value field is empty or invalid");
                if (!seen.insert(entry.field).second)
                    return error("duplicate value field '" + entry.field + "'");
            }
            return common::Status::OK();
        }
    }

    Condition Condition::compare(std::string field, const CompareOperator op,
                                 common::Value value) {
        Condition condition(Kind::Compare);
        condition.field_ = std::move(field);
        condition.compareOperator_ = op;
        condition.values_.push_back(std::move(value));
        return condition;
    }

    Condition Condition::in(std::string field, common::Params values) {
        Condition condition(Kind::In);
        condition.field_ = std::move(field);
        condition.values_ = std::move(values);
        return condition;
    }

    Condition Condition::notIn(std::string field, common::Params values) {
        Condition condition(Kind::NotIn);
        condition.field_ = std::move(field);
        condition.values_ = std::move(values);
        return condition;
    }

    Condition Condition::between(std::string field, common::Value lower,
                                 common::Value upper) {
        Condition condition(Kind::Between);
        condition.field_ = std::move(field);
        condition.values_.push_back(std::move(lower));
        condition.values_.push_back(std::move(upper));
        return condition;
    }

    Condition Condition::isNull(std::string field) {
        Condition condition(Kind::IsNull);
        condition.field_ = std::move(field);
        return condition;
    }

    Condition Condition::isNotNull(std::string field) {
        Condition condition(Kind::IsNotNull);
        condition.field_ = std::move(field);
        return condition;
    }

    Condition Condition::all(std::vector<Condition> conditions) {
        Condition condition(Kind::All);
        condition.children_ = std::move(conditions);
        return condition;
    }

    Condition Condition::any(std::vector<Condition> conditions) {
        Condition condition(Kind::Any);
        condition.children_ = std::move(conditions);
        return condition;
    }

    Condition Condition::negate(Condition child) {
        Condition condition(Kind::Not);
        condition.children_.push_back(std::move(child));
        return condition;
    }

    Builder::Builder(const Operation operation, std::string table,
                     const common::util::Dialect dialect)
        : operation_(operation), table_(std::move(table)), dialect_(dialect) {}

    Builder Builder::select(std::string table, const common::util::Dialect dialect) {
        return Builder(Operation::Select, std::move(table), dialect);
    }

    Builder Builder::insert(std::string table, const common::util::Dialect dialect) {
        return Builder(Operation::Insert, std::move(table), dialect);
    }

    Builder Builder::update(std::string table, const common::util::Dialect dialect) {
        return Builder(Operation::Update, std::move(table), dialect);
    }

    Builder Builder::deleteFrom(std::string table, const common::util::Dialect dialect) {
        return Builder(Operation::Delete, std::move(table), dialect);
    }

    Builder &Builder::columns(std::vector<std::string> fields) {
        columns_ = std::move(fields);
        return *this;
    }

    Builder &Builder::value(std::string field, common::Value value) {
        values_.emplace_back(std::move(field), std::move(value));
        return *this;
    }

    Builder &Builder::values(std::vector<FieldValue> values) {
        values_.insert(values_.end(), std::make_move_iterator(values.begin()),
                       std::make_move_iterator(values.end()));
        return *this;
    }

    Builder &Builder::values(std::vector<std::string> fields, common::Params params) {
        if (fields.size() != params.size()) {
            specificationError_ = "field count does not match parameter count";
            return *this;
        }
        values_.reserve(values_.size() + fields.size());
        for (std::size_t i = 0; i < fields.size(); ++i)
            values_.emplace_back(std::move(fields[i]), std::move(params[i]));
        return *this;
    }

    Builder &Builder::where(Condition condition) {
        conditions_.push_back(std::move(condition));
        return *this;
    }

    Builder &Builder::orderBy(std::string field, const SortDirection direction) {
        orderings_.push_back({std::move(field), direction});
        return *this;
    }

    Builder &Builder::allowAllRows(const bool allow) noexcept {
        allowAllRows_ = allow;
        return *this;
    }

    BuildResult Builder::build() const {
        BuildResult result;
        if (!specificationError_.empty()) {
            result.status = error(specificationError_);
            return result;
        }
        if (!validDialect(dialect_)) {
            result.status = error("unknown identifier dialect");
            return result;
        }
        if (!validIdentifier(table_)) {
            result.status = error("table is empty or invalid");
            return result;
        }
        if (const auto status = validateUniqueFields(values_); !status.ok()) {
            result.status = status;
            return result;
        }

        const std::string table = quote(table_, dialect_);
        switch (operation_) {
            case Operation::Select: {
                if (!values_.empty() || allowAllRows_) {
                    result.status = error("SELECT does not accept value assignments or allowAllRows");
                    return result;
                }
                result.statement.sql = "SELECT ";
                if (columns_.empty()) result.statement.sql += '*';
                else {
                    std::set<std::string> seen;
                    for (std::size_t i = 0; i < columns_.size(); ++i) {
                        if (!validIdentifier(columns_[i]) || columns_[i] == "*") {
                            result.status = error("SELECT column is empty or invalid");
                            return result;
                        }
                        if (!seen.insert(columns_[i]).second) {
                            result.status = error("duplicate SELECT column '" + columns_[i] + "'");
                            return result;
                        }
                        if (i) result.statement.sql += ", ";
                        result.statement.sql += quote(columns_[i], dialect_);
                    }
                }
                result.statement.sql += " FROM " + table;
                if (const auto status = appendWhere(conditions_, dialect_, result.statement.params,
                                                    result.statement.sql);
                    !status.ok()) {
                    result.status = status;
                    return result;
                }
                if (!orderings_.empty()) {
                    result.statement.sql += " ORDER BY ";
                    for (std::size_t i = 0; i < orderings_.size(); ++i) {
                        if (!validIdentifier(orderings_[i].field)) {
                            result.status = error("ORDER BY field is empty or invalid");
                            return result;
                        }
                        if (orderings_[i].direction != SortDirection::Ascending &&
                            orderings_[i].direction != SortDirection::Descending) {
                            result.status = error("unknown ORDER BY direction");
                            return result;
                        }
                        if (i) result.statement.sql += ", ";
                        result.statement.sql += quote(orderings_[i].field, dialect_);
                        result.statement.sql += orderings_[i].direction == SortDirection::Ascending
                                                    ? " ASC"
                                                    : " DESC";
                    }
                }
                break;
            }
            case Operation::Insert: {
                if (!columns_.empty() || !conditions_.empty() || !orderings_.empty() ||
                    allowAllRows_) {
                    result.status = error("INSERT accepts values only");
                    return result;
                }
                if (values_.empty()) {
                    result.status = error("INSERT requires at least one value");
                    return result;
                }
                result.statement.sql = "INSERT INTO " + table + " (";
                for (std::size_t i = 0; i < values_.size(); ++i) {
                    if (i) result.statement.sql += ", ";
                    result.statement.sql += quote(values_[i].field, dialect_);
                }
                result.statement.sql += ") VALUES (";
                for (std::size_t i = 0; i < values_.size(); ++i) {
                    if (i) result.statement.sql += ", ";
                    result.statement.sql += '?';
                    result.statement.params.push_back(values_[i].value);
                }
                result.statement.sql += ')';
                break;
            }
            case Operation::Update: {
                if (!columns_.empty() || !orderings_.empty()) {
                    result.status = error("UPDATE does not accept columns or ordering");
                    return result;
                }
                if (values_.empty()) {
                    result.status = error("UPDATE requires at least one value");
                    return result;
                }
                if (conditions_.empty() && !allowAllRows_) {
                    result.status = error("UPDATE without WHERE requires allowAllRows()");
                    return result;
                }
                result.statement.sql = "UPDATE " + table + " SET ";
                for (std::size_t i = 0; i < values_.size(); ++i) {
                    if (i) result.statement.sql += ", ";
                    result.statement.sql += quote(values_[i].field, dialect_) + " = ?";
                    result.statement.params.push_back(values_[i].value);
                }
                if (const auto status = appendWhere(conditions_, dialect_, result.statement.params,
                                                    result.statement.sql);
                    !status.ok()) {
                    result.status = status;
                    return result;
                }
                break;
            }
            case Operation::Delete: {
                if (!columns_.empty() || !values_.empty() || !orderings_.empty()) {
                    result.status = error("DELETE accepts conditions only");
                    return result;
                }
                if (conditions_.empty() && !allowAllRows_) {
                    result.status = error("DELETE without WHERE requires allowAllRows()");
                    return result;
                }
                result.statement.sql = "DELETE FROM " + table;
                if (const auto status = appendWhere(conditions_, dialect_, result.statement.params,
                                                    result.statement.sql);
                    !status.ok()) {
                    result.status = status;
                    return result;
                }
                break;
            }
            default:
                result.status = error("unknown operation");
                return result;
        }
        result.status = common::Status::OK();
        return result;
    }
}
