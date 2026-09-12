#include "execution/Executor.h"

#include "txn/TransactionManager.h"

namespace sqlcompiler {

ExecutionContext::ExecutionContext(SystemCatalog* catalog,
                                   TransactionManager* txn_manager)
    : catalog_(catalog), txn_manager_(txn_manager) {
}

SystemCatalog* ExecutionContext::GetCatalog() const {
    return catalog_;
}

void ExecutionContext::RegisterCte(const std::string& name, std::vector<Tuple> rows) {
    cte_results_[name] = CteMaterialization{name, std::move(rows)};
}

void ExecutionContext::AppendCteRows(const std::string& name,
                                     const std::vector<Tuple>& rows) {
    auto it = cte_results_.find(name);
    if (it == cte_results_.end()) {
        cte_results_[name] = CteMaterialization{name, rows};
    } else {
        it->second.rows.insert(it->second.rows.end(), rows.begin(), rows.end());
    }
}

const std::vector<Tuple>* ExecutionContext::GetCteRows(const std::string& name) const {
    // 递归 CTE 迭代期间：先看 override 栈顶（最新工作集）。
    auto oit = cte_overrides_.find(name);
    if (oit != cte_overrides_.end() && !oit->second.empty()) {
        return &oit->second.back();
    }
    auto it = cte_results_.find(name);
    if (it == cte_results_.end()) return nullptr;
    return &it->second.rows;
}

bool ExecutionContext::HasCte(const std::string& name) const {
    if (cte_overrides_.count(name) && !cte_overrides_.at(name).empty()) return true;
    return cte_results_.count(name) > 0;
}

void ExecutionContext::PushCteOverride(const std::string& name, std::vector<Tuple> rows) {
    cte_overrides_[name].push_back(std::move(rows));
}

void ExecutionContext::PopCteOverride(const std::string& name) {
    auto it = cte_overrides_.find(name);
    if (it == cte_overrides_.end() || it->second.empty()) return;
    it->second.pop_back();
}

Value ExecutionContext::GetSessionVar(const std::string& name) const {
    // 71_proc_out_params：session_log_ 现在是非所有权指针，指向 Database 的
    // session_vars_。没注入时（罕见，例如直接构造 ctx 的内部单元测试）退化为
    // 空 map，行为与 V1 之前一致（所有 @var 视为 NULL）。
    if (session_log_ == nullptr) return Value::MakeNull();
    auto it = session_log_->find(name);
    if (it == session_log_->end()) return Value::MakeNull();
    return it->second;
}

void ExecutionContext::SetSessionVar(const std::string& name, Value v) {
    if (session_log_ == nullptr) return;
    (*session_log_)[name] = std::move(v);
}

const std::unordered_map<std::string, Value>&
ExecutionContext::GetSessionLog() const {
    static const std::unordered_map<std::string, Value> kEmpty;
    if (session_log_ == nullptr) return kEmpty;
    return *session_log_;
}

Executor::Executor(ExecutionContext* context) : context_(context) {
}

}  // namespace sqlcompiler