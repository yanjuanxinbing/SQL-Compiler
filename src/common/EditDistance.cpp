#include "common/EditDistance.h"

#include <algorithm>
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

    std::vector<int> prev(rows + 1);
    std::vector<int> curr(rows + 1);
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
