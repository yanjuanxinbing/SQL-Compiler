#include "execution/Executor.h"

namespace sqlcompiler {

ExecutionContext::ExecutionContext(SystemCatalog* catalog) : catalog_(catalog) {
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

Executor::Executor(ExecutionContext* context) : context_(context) {
}

}  // namespace sqlcompiler