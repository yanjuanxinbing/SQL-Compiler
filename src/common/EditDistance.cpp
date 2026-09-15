#include "common/EditDistance.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace sqlcompiler {

int LevenshteinDistance(std::string_view a, std::string_view b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return static_cast<int>(m);
    if (m == 0) return static_cast<int>(n);

    // Iterate over the shorter string in the inner dimension so that the
    // allocated row buffer is O(min(n, m)) rather than O(max(n, m)).
    std::string_view rows_owner = (m <= n) ? b : a;
    std::string_view cols_owner = (m <= n) ? a : b;
    const size_t rows = rows_owner.size();   // inner-loop length
    const size_t cols = cols_owner.size();   // outer-loop length

    // item #14: 复用跨调用分配的 DP 行缓冲，避免每个 SuggestClosestName 调用
    // 都触发一次堆分配。LevenshteinDistance 在语义错误提示路径上是热路径
    // （每条 column not found 都对所有候选列名跑一次），原始实现每次都
    // std::vector<int> 分配 + 释放，长标识符 + 多候选时 alloc 次数线性增长。
    //
    // 用 mutex 串行访问（工作段极短，10~20 个 int），对单线程语义分析器不阻塞；
    // 若日后升级为多线程分析器，可换成 thread_local。这里 mutex 是必要的——
    // 静态缓冲跨调用共享，并发调用会相互覆盖 prev/curr。
    static std::mutex dp_mutex;
    static std::vector<int> prev;
    static std::vector<int> curr;
    std::lock_guard<std::mutex> lock(dp_mutex);
    prev.assign(rows + 1, 0);
    curr.assign(rows + 1, 0);
    for (size_t j = 0; j <= rows; ++j) {
        prev[j] = static_cast<int>(j);
    }

    for (size_t i = 1; i <= cols; ++i) {
        curr[0] = static_cast<int>(i);
        char outer_c = cols_owner[i - 1];
        for (size_t j = 1; j <= rows; ++j) {
            int cost = (outer_c == rows_owner[j - 1]) ? 0 : 1;
            // deletion, insertion, substitution
            int del_cost = prev[j] + 1;
            int ins_cost = curr[j - 1] + 1;
            int sub_cost = prev[j - 1] + cost;
            int best = del_cost < ins_cost ? del_cost : ins_cost;
            if (sub_cost < best) best = sub_cost;
            curr[j] = best;
        }
        std::swap(prev, curr);
    }
    return prev[rows];
}

}  // namespace sqlcompiler
