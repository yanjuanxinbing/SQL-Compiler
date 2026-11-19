#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sqlcompiler {

// Case-insensitive hash + equality for ASCII identifiers.
//
// SQL keywords and identifiers are ASCII by spec; non-ASCII (e.g. Chinese)
// identifiers are compared byte-for-byte after the lexer's normalization, so
// `tolower`/`toupper` should not be applied to non-ASCII bytes.
//
// Both functors accept `std::string_view` (no allocation) AND `const std::string&`
// (delegating) — the latter overload keeps them usable directly with
// `std::unordered_map::find`/`emplace`. C++17 does not support heterogeneous
// lookup on unordered containers (C++20 only), so callers must hold a
// `std::string` when calling `find()`, but on the hot path the column /
// identifier text is already a `std::string` (parsed from the source).
struct CaseInsensitiveHash {
    std::size_t operator()(std::string_view s) const noexcept {
        // FNV-1a 64-bit; lowercased on the fly. ASCII only — non-ASCII bytes
        // pass through unchanged (consistent with the rest of the compiler).
        std::uint64_t h = 1469598103934665603ULL;
        for (char c : s) {
            char lc = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(lc));
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

struct CaseInsensitiveEq {
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char la = static_cast<char>(
                std::tolower(static_cast<unsigned char>(a[i])));
            char lb = static_cast<char>(
                std::tolower(static_cast<unsigned char>(b[i])));
            if (la != lb) return false;
        }
        return true;
    }
};

// True iff `a` and `b` are byte-equal under ASCII case-folding.
// Allocation-free; O(min(|a|, |b|)).
inline bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char la = static_cast<char>(
            std::tolower(static_cast<unsigned char>(a[i])));
        char lb = static_cast<char>(
            std::tolower(static_cast<unsigned char>(b[i])));
        if (la != lb) return false;
    }
    return true;
}

}  // namespace sqlcompiler