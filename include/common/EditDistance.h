#pragma once

#include <string>
#include <string_view>

namespace sqlcompiler {

// Standard Wagner-Fischer dynamic programming algorithm for Levenshtein
// (edit) distance. Cached two-row buffer to keep memory O(min(|a|, |b|)) per
// call; suitable for the very short identifier strings used in SQL names.
//
// Distance is computed in the case the caller passes — the caller (e.g.
// SemanticAnalyzer) is responsible for normalizing case if needed, so that
// this utility stays a pure primitive with no implicit policy.
int LevenshteinDistance(std::string_view a, std::string_view b);

}  // namespace sqlcompiler
