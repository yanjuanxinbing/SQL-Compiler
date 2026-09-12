// ALTER TABLE 算子的真实实现。
//
// 四类动作的真实行为（不再是无副作用的 no-op）：
//
//   ADD COLUMN col TYPE[(N)]
//     - 构造新的 ColumnInfo 并追加到 SymbolTable 的 TableInfo 列尾。
//     - 对 TableHeap 中已有每一行：snapshot (RID, values)，用新 column_types
//       重新 Serialize 后 InsertTuple 回去（DeleteTuple + InsertTuple 模式，避开
//       UpdateTuple 的 in-place 增长边界）。
//     - 新增列的初始值统一为 NULL；DEFAULT expr 仅在语法层接收，未在执行期求值。
//     - 同步更新 sys_tables 中的元数据 blob：先删旧记录，再写新记录。
//     - 失效（并删除）该表上的所有 B+Tree 索引——索引的 key_columns 可能引用
//       新增列或被丢弃列，重建超出本期范围。
//
//   DROP COLUMN col
//     - 从 TableInfo 中移除该列；语义层保证列存在。
//     - 同样 snapshot + DeleteTuple + InsertTuple 重写所有行。
//     - keep_map[new_pos] = old_pos = (new_pos < drop_idx) ? new_pos : new_pos + 1
//       —— 必须以"新位置"为外层循环，否则会读到被删列的数据当成保留列。
//
//   RENAME TO new_name
//     - 不重写行字节（schema 未变）。
//     - SystemCatalog::UpdateTableSchema 把 table_heaps_ 的键迁移到新名，
//       SymbolTable 重新注册，sys_tables 元数据记录先删后插。
//
//   MODIFY COLUMN col TYPE[(N)]
//     - 更新 TableInfo 中该列的 data_type / char_length。
//     - 对该列的所有现有值做 best-effort CastValue：INT↔BIGINT 视为相同运行时类型，
//       VARCHAR shrink 截断（超长报错），FLOAT→INT 截断为整数（NaN/Inf 报错），
//       VARCHAR↔数值走 parse / format。NULL 值不参与 cast。
//     - 同样 snapshot + DeleteTuple + InsertTuple 重写所有行。
//
// 限制：
//   - 不支持回滚：ALTER 失败时目录与 TableHeap 可能已经部分修改；建议生产环境
//     先备份再 ALTER。
//   - 不重建索引：ADD/DROP/MODIFY 会清空该表所有 B+Tree 索引元数据与 B+Tree
//     页面。如需保留索引性能，请在 ALTER 后重新 CREATE INDEX。
//   - 列级 DEFAULT expr 当前不会被求值；新增列一律填 NULL。

#include "execution/AlterTableExecutor.h"

#include "common/Error.h"
#include "storage_engine/Value.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <vector>

namespace sqlcompiler {

namespace {

// 把声明类型串（如 "VARCHAR" / "BIGINT"）归一化为大写，便于分支判断。
std::string NormalizeType(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// 把 ColumnDefinition 转成 ColumnInfo。ADD/MODIFY 共用。
ColumnInfo MakeColumnInfo(const ColumnDefinition& cd) {
    ColumnInfo ci;
    ci.name = cd.column_name;
    ci.data_type = NormalizeType(cd.data_type);
    ci.char_length = cd.char_length;
    ci.is_primary_key = cd.is_primary_key;
    ci.is_not_null = cd.is_not_null;
    // ALTER ADD/MODIFY 也支持 CHECK / DEFAULT 子句（测试 39_ddl_extensions 用例
    // 没显式覆盖，但语法上要保留），因此把 AST 上的表达式一并搬运过去。
    ci.check_expr = cd.check_expr;
    ci.default_expr = cd.default_expr;
    // 58_constraints: 命名 CHECK 约束在 ADD/MODIFY 路径上同样保留。
    ci.constraint_name = cd.constraint_name;
    return ci;
}

// 累计把单个 Value 从 src_type 转换为 dst_type。失败返回 std::nullopt，
// 调用方应停止 ALTER 并向用户报错。
//
// 支持的转换：
//   INT  <-> BIGINT  按字面相等（运行时都是 INTEGER/4 字节）。
//   INT  -> FLOAT   不丢精度。
//   FLOAT -> INT    截断为整数；NaN/Inf 直接报错。
//   INT/FLOAT -> VARCHAR
//   VARCHAR -> INT/FLOAT  解析失败报错。
//   VARCHAR shrink  截断到 char_length；超长报错。
Value CastValue(const Value& v, const std::string& dst_type,
                int32_t dst_char_length, ValueType src_type) {
    std::string dt = NormalizeType(dst_type);
    if (v.IsNull()) return v;  // NULL 不做转换。

    auto is_int_ty = [](const std::string& t) {
        return t == "INT" || t == "INTEGER" || t == "BIGINT";
    };
    auto is_float_ty = [](const std::string& t) {
        return t == "FLOAT" || t == "DOUBLE" || t == "DECIMAL";
    };
    auto is_str_ty = [](const std::string& t) {
        return t == "VARCHAR" || t == "STRING" || t == "TEXT" || t == "CHAR";
    };

    if (is_int_ty(dt)) {
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeInt(v.AsInt());
        }
        if (v.GetType() == ValueType::FLOAT) {
            double d = v.AsFloat();
            if (std::isnan(d) || std::isinf(d)) {
                return Value();  // 信号：调用方应判定失败
            }
            return Value::MakeInt(static_cast<int32_t>(d));
        }
        if (v.GetType() == ValueType::VARCHAR) {
            const std::string& s = v.AsVarchar();
            try {
                size_t pos = 0;
                int parsed = std::stoi(s, &pos);
                if (pos != s.size()) return Value();
                return Value::MakeInt(parsed);
            } catch (...) {
                return Value();
            }
        }
    } else if (is_float_ty(dt)) {
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeFloat(static_cast<double>(v.AsInt()));
        }
        if (v.GetType() == ValueType::FLOAT) {
            return Value::MakeFloat(v.AsFloat());
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                return Value::MakeFloat(std::stod(v.AsVarchar()));
            } catch (...) {
                return Value();
            }
        }
    } else if (is_str_ty(dt)) {
        std::string s;
        if (v.GetType() == ValueType::INTEGER) s = std::to_string(v.AsInt());
        else if (v.GetType() == ValueType::FLOAT) s = v.ToString();
        else s = v.AsVarchar();
        if (dst_char_length > 0 && static_cast<int32_t>(s.size()) > dst_char_length) {
            return Value();
        }
        return Value::MakeVarchar(s);
    }
    return Value();  // 不支持的转换
}

// 返回值是否表示"转换失败"（默认构造的 NULL Value 即为信号）。
bool IsConversionFailure(const Value& v) {
    // 我们用 NULL Value 同时表示"失败"与"正常 NULL"——两种语义只能用一个哨兵
    // 区分。这里约定：若调用方传入 v.IsNull() == true，原值就是 NULL，不会走
    // CastValue 的失败分支。因此函数返回的 IsNull 一定来自失败路径。
    return v.IsNull();
}

// 把 TableInfo::columns 投影到 ValueType 列表，供 TableHeap 序列化使用。
std::vector<ValueType> SchemaToValueTypes(const TableInfo& info) {
    std::vector<ValueType> out;
    out.reserve(info.columns.size());
    for (const auto& c : info.columns) {
        out.push_back(ValueTypeFromString(c.data_type));
    }
    return out;
}

// 按 keep_map 把老行 values 投影成新行 values。keep_map[new_idx] = old_idx
// （或 -1 表示该列为新增，需从 added_values 取）。返回的新 vector 长度 ==
// new_types.size()。modified_at 是会被强制按 new_types 类型重写的列下标集合；
// 这些列即便 keep_map 指向老列，也要走一次 CastValue，保证字节布局跟 new_types 对齐。
//
// 约定：调用方在 ADD COLUMN 时构造 keep_map = {0, 1, ..., old_n-1, -1}；
// DROP 时去掉指定下标，其余 old->new 保持顺序；MODIFY 时 keep_map = identity。
std::vector<Value> ProjectRow(const std::vector<Value>& old_values,
                              const std::vector<int>& keep_map,
                              const std::vector<Value>& added_values,
                              const std::vector<std::string>& new_col_types,
                              const std::vector<int32_t>& new_char_lengths,
                              const std::unordered_set<size_t>& modified_at) {
    std::vector<Value> out;
    out.reserve(keep_map.size());
    for (size_t i = 0; i < keep_map.size(); ++i) {
        int old_idx = keep_map[i];
        if (old_idx < 0) {
            // 新增列：取 added_values[i]（已经是 NULL/DEFAULT）。
            out.push_back(added_values[i]);
            continue;
        }
        Value v = old_values[old_idx];
        if (modified_at.count(i) != 0 && !v.IsNull()) {
            // 仅对非 NULL 值做 cast：NULL 的列在 MODIFY 后依然是 NULL，不需要
            // 经 CastValue 走一遍（CastValue 把 NULL 与 "失败" 共用 NULL Value
            // 表示，无法区分；显式跳过即可避开歧义）。
            Value casted = CastValue(v, new_col_types[i], new_char_lengths[i],
                                     v.GetType());
            if (IsConversionFailure(casted)) {
                throw CompilerException(
                    ErrorStage::SEMANTIC,
                    "ALTER MODIFY: cannot convert existing value at column index "
                    + std::to_string(i));
            }
            v = casted;
        }
        out.push_back(std::move(v));
    }
    return out;
}

// 把 TableHeap 里的全部行按转换规则重写。
//   old_types  : 旧 schema 的运行时类型
//   new_types  : 新 schema 的运行时类型
//   keep_map   : new_idx -> old_idx（-1 表示新列）
//   added_values: new_idx 处为新增列时使用的默认/DEFAULT 值（其他下标忽略）
//   new_col_types / new_char_lengths: 新 schema 的声明类型串（用于 modified_at 列的 cast）
//   modified_at: 需要按 new_types 重新 cast 的 new_idx 集合
//
// 算法：先 snapshot 所有 (RID, old_values)；然后对每个 snapshot 计算 new_values，
// 用 DeleteTuple + InsertTuple 落地。这样可以避开 UpdateTuple 的复杂边界情况，
// 且不会重入 iterator。
void RewriteHeap(TableHeap* heap, const std::vector<ValueType>& old_types,
                 const std::vector<ValueType>& new_types,
                 const std::vector<int>& keep_map,
                 const std::vector<Value>& added_values,
                 const std::vector<std::string>& new_col_types,
                 const std::vector<int32_t>& new_char_lengths,
                 const std::unordered_set<size_t>& modified_at) {
    if (!heap) return;
    struct Snap {
        RID rid;
        std::vector<Value> values;
    };
    std::vector<Snap> snaps;
    auto it = heap->Begin();
    while (it.HasNext()) {
        Tuple t = it.Next(old_types);
        if (!t.GetRid().IsValid()) continue;
        Snap s;
        s.rid = t.GetRid();
        s.values.reserve(t.ColumnCount());
        for (size_t i = 0; i < t.ColumnCount(); ++i) {
            s.values.push_back(t.GetValue(i));
        }
        snaps.push_back(std::move(s));
    }
    for (const auto& s : snaps) {
        // ProjectRow 在 cast 失败时直接抛出 CompilerException，由 ExecutionEngine
        // 统一捕获并报告；不要在这里吞掉异常，否则会留下半改写的目录状态。
        std::vector<Value> nv = ProjectRow(s.values, keep_map, added_values,
                                           new_col_types, new_char_lengths,
                                           modified_at);
        Tuple nt(nv);
        // 删除旧 slot；空出来的空间由后续插入或后续访问复用。
        heap->DeleteTuple(s.rid);
        RID new_rid;
        heap->InsertTuple(nt, &new_rid, new_types);
        (void)new_rid;
    }
}

}  // namespace

AlterTableExecutor::AlterTableExecutor(ExecutionContext* context, AlterTableNode* node)
    : Executor(context), node_(node), executed_(false) {
}

void AlterTableExecutor::Init() {
    if (executed_) return;
    executed_ = true;

    if (!node_) return;
    auto* catalog = context_->GetCatalog();
    if (!catalog) {
        throw CompilerException(ErrorStage::SEMANTIC, "no catalog available");
    }
    const std::string& table_name = node_->table_name;
    if (!catalog->HasTable(table_name)) {
        throw CompilerException(ErrorStage::SEMANTIC,
                                "table not found: " + table_name);
    }
    const TableInfo* old_info = catalog->GetTable(table_name);
    if (!old_info) {
        throw CompilerException(ErrorStage::SEMANTIC,
                                "table not found: " + table_name);
    }

    // 构造新 TableInfo。各分支复用 BuildNewInfo 入口。
    TableInfo new_info = *old_info;
    std::vector<int> keep_map;
    std::vector<Value> added_values;
    std::unordered_set<size_t> modified_at;
    std::vector<ValueType> old_types = SchemaToValueTypes(*old_info);

    switch (node_->action) {
        case AlterAction::ADD_COLUMN: {
            const auto& cd = node_->column_def;
            if (!cd) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "ADD COLUMN missing column definition");
            }
            ColumnInfo ci = MakeColumnInfo(*cd);
            // 防御：同名列已被语义层挡过，这里再确认一次。
            for (const auto& c : new_info.columns) {
                if (c.name == ci.name) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                                            "column already exists: " + ci.name);
                }
            }
            // 追加新列；新增列初始默认 NULL（不支持 DEFAULT 表达式求值，语法仅接收）。
            new_info.columns.push_back(ci);
            // keep_map: 0..n-1 一一对应老列；n=-1 表示新增列。
            keep_map.resize(new_info.columns.size());
            for (size_t i = 0; i < old_info->columns.size(); ++i) {
                keep_map[i] = static_cast<int>(i);
            }
            keep_map[new_info.columns.size() - 1] = -1;
            added_values.assign(new_info.columns.size(), Value::MakeNull());
            break;
        }
        case AlterAction::RENAME_COLUMN: {
            // 53_ddl: 列重命名 —— 仅修改 schema，不重写行字节。
            // 因为 TableHeap 的行字节布局是按"位置"序列化（不含列名），
            // 修改 ColumnInfo.name 已足够让后续 SELECT 用新列名查表。
            const std::string& old_name = node_->rename_column_old_name;
            const std::string& new_name = node_->rename_column_new_name;
            if (old_name.empty() || new_name.empty()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "RENAME COLUMN requires both old and new column names");
            }
            if (old_name == new_name) {
                return;  // no-op
            }
            int target_idx = -1;
            for (size_t i = 0; i < new_info.columns.size(); ++i) {
                if (new_info.columns[i].name == old_name) {
                    target_idx = static_cast<int>(i);
                    break;
                }
            }
            if (target_idx < 0) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "column not found: " + old_name);
            }
            // 防御：new_name 已存在则报错。
            for (const auto& c : new_info.columns) {
                if (c.name == new_name) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "column already exists: " + new_name);
                }
            }
            new_info.columns[target_idx].name = new_name;
            // 不需要 keep_map/rewrite；调用方识别"无行重写"路径。
            keep_map.clear();
            added_values.clear();
            break;
        }
        case AlterAction::DROP_COLUMN: {
            const std::string& drop_name = node_->drop_column_name;
            int drop_idx = -1;
            for (size_t i = 0; i < new_info.columns.size(); ++i) {
                if (new_info.columns[i].name == drop_name) {
                    drop_idx = static_cast<int>(i);
                    break;
                }
            }
            if (drop_idx < 0) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "column not found: " + drop_name);
            }
            new_info.columns.erase(new_info.columns.begin() + drop_idx);
            // keep_map 的语义：keep_map[new_pos] = old_pos（-1 表示新增列）。
            // 删除 drop_idx 之后，新位置 j 对应的老位置是 j < drop_idx ? j : j+1。
            const size_t new_size = new_info.columns.size();
            keep_map.reserve(new_size);
            for (size_t new_pos = 0; new_pos < new_size; ++new_pos) {
                size_t old_pos = (new_pos < static_cast<size_t>(drop_idx))
                                     ? new_pos
                                     : new_pos + 1;
                keep_map.push_back(static_cast<int>(old_pos));
            }
            added_values.clear();  // 没有新增列；不会被访问。
            break;
        }
        case AlterAction::RENAME_TO: {
            const std::string& new_name = node_->new_table_name;
            if (new_name.empty()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "RENAME TO requires a new table name");
            }
            if (new_name == table_name) {
                return;  // no-op
            }
            if (catalog->HasTable(new_name)) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "table already exists: " + new_name);
            }
            // 仅修改 TableInfo.table_name；schema 不变，行字节也不变。
            new_info.table_name = new_name;
            // 不需要 keep_map/rewrite；调用方识别"无行重写"路径。
            keep_map.clear();
            added_values.clear();
            break;
        }
        case AlterAction::MODIFY_COLUMN: {
            const auto& cd = node_->column_def;
            if (!cd) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "MODIFY COLUMN missing column definition");
            }
            int target_idx = -1;
            for (size_t i = 0; i < new_info.columns.size(); ++i) {
                if (new_info.columns[i].name == cd->column_name) {
                    target_idx = static_cast<int>(i);
                    break;
                }
            }
            if (target_idx < 0) {
                throw CompilerException(ErrorStage::SEMANTIC,
                                        "column not found: " + cd->column_name);
            }
            std::string new_ty = NormalizeType(cd->data_type);
            // 更新声明类型 / char_length。
            new_info.columns[target_idx].data_type = new_ty;
            new_info.columns[target_idx].char_length = cd->char_length;
            // keep_map: 一一对应。
            keep_map.resize(new_info.columns.size());
            for (size_t i = 0; i < new_info.columns.size(); ++i) {
                keep_map[i] = static_cast<int>(i);
            }
            added_values.assign(new_info.columns.size(), Value::MakeNull());
            modified_at.insert(static_cast<size_t>(target_idx));
            break;
        }
    }

    std::vector<ValueType> new_types = SchemaToValueTypes(new_info);
    std::vector<std::string> new_col_type_strs;
    std::vector<int32_t> new_char_lengths;
    new_col_type_strs.reserve(new_info.columns.size());
    new_char_lengths.reserve(new_info.columns.size());
    for (const auto& c : new_info.columns) {
        new_col_type_strs.push_back(c.data_type);
        new_char_lengths.push_back(c.char_length);
    }

    if (node_->action == AlterAction::RENAME_TO ||
        node_->action == AlterAction::RENAME_COLUMN) {
        // RENAME 类不需要重写行字节，但仍然走同一目录更新路径。
        //   - RENAME_TO 把 table_heaps_、sys_tables 记录迁移到新键。
        //   - RENAME_COLUMN 仅修改 ColumnInfo.name；sys_tables blob 也要更新。
        // Phase B：让 catalog 内部 sys_tables 写入带上当前事务。
        catalog->SetActiveTransaction(context_->GetTransaction());
        bool ok = catalog->UpdateTableSchema(table_name, new_info);
        catalog->SetActiveTransaction(nullptr);
        if (!ok) {
            throw CompilerException(ErrorStage::SEMANTIC,
                                    "failed to update table schema in catalog");
        }
        return;
    }

    // ADD / DROP / MODIFY：先重写所有行的字节，再让目录层切到新 schema。
    TableHeap* heap = catalog->GetTableHeap(table_name);
    if (heap) {
        RewriteHeap(heap, old_types, new_types, keep_map, added_values,
                    new_col_type_strs, new_char_lengths, modified_at);
    }
    // Phase B：同上。
    catalog->SetActiveTransaction(context_->GetTransaction());
    bool ok2 = catalog->UpdateTableSchema(table_name, new_info);
    catalog->SetActiveTransaction(nullptr);
    if (!ok2) {
        throw CompilerException(ErrorStage::SEMANTIC,
                                "failed to update table schema in catalog");
    }
}

bool AlterTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler