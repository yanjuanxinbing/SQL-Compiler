#include "catalog/IndexInfo.h"

#include <cctype>

namespace sqlcompiler {

namespace {

const char kPkIndexPrefix[] = "__pk_";

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool IsVariableLengthType(const std::string& data_type) {
    const std::string up = ToUpper(data_type);
    return up == "VARCHAR" || up == "CHAR" || up == "TEXT" || up == "STRING";
}

}  // namespace

bool IndexInfo::IsPrimaryKeyIndex() const {
    return index_name.compare(0, sizeof(kPkIndexPrefix) - 1, kPkIndexPrefix) == 0;
}

std::string MakePrimaryKeyIndexName(const std::string& table_name,
                                    size_t group_index) {
    return std::string(kPkIndexPrefix) + table_name + "_" +
           std::to_string(group_index);
}

bool BuildIndexKeyTypes(const TableInfo& table_info,
                        const std::vector<std::string>& key_columns,
                        std::vector<ValueType>* out) {
    if (out == nullptr) return false;
    out->clear();
    out->reserve(key_columns.size());
    for (const auto& name : key_columns) {
        const ColumnInfo* col = table_info.GetColumn(name);
        if (col == nullptr) return false;
        out->push_back(ValueTypeFromString(col->data_type));
    }
    return true;
}

bool ValidateIndexableColumns(const TableInfo& table_info,
                              const std::vector<std::string>& key_columns,
                              std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    if (key_columns.empty()) return fail("index must have at least one column");

    for (const auto& name : key_columns) {
        const ColumnInfo* col = table_info.GetColumn(name);
        if (col == nullptr) {
            return fail("unknown column in index: " + name);
        }
        if (IsVariableLengthType(col->data_type)) {
            if (col->char_length <= 0) {
                return fail("cannot index unbounded column '" + name +
                            "': declare it as " + col->data_type +
                            "(n) so the index key has a bounded size");
            }
            if (col->char_length > kMaxIndexedCharLength) {
                return fail("cannot index column '" + name + "': length " +
                            std::to_string(col->char_length) + " exceeds the " +
                            std::to_string(kMaxIndexedCharLength) +
                            " character limit for index keys");
            }
        }
        // NULL values are permitted in index keys: standard SQL engines
        // (PostgreSQL, MySQL/InnoDB, Oracle) all store NULLs as a separate
        // entry in B-tree indexes. We only restrict variable-length sizes
        // above; nullability is intentionally not enforced here.
    }
    return true;
}

}  // namespace sqlcompiler
