// thesaurus.cpp — synonym groups.
//
// All words are stored lowercased. Lookups are case-insensitive.

#include "tsearch/thesaurus.hpp"

#include <algorithm>
#include <cctype>

namespace pgcpp::tsearch {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

void Thesaurus::AddSynonyms(std::vector<std::string> words) {
    std::vector<std::string> group;
    group.reserve(words.size());
    for (auto& w : words) {
        std::string lower = ToLower(w);
        if (lower.empty() || groups_indexed_.count(lower) > 0) {
            continue;
        }
        group.push_back(std::move(lower));
    }
    if (group.empty()) {
        return;
    }
    std::size_t index = groups_.size();
    // Index each word WITHOUT moving from `group` so the group retains its
    // contents for later lookup.
    for (const auto& w : group) {
        groups_indexed_.emplace(w, index);
    }
    groups_.push_back(std::move(group));
}

std::vector<std::string> Thesaurus::Lookup(std::string_view word) const {
    std::string lower = ToLower(word);
    auto it = groups_indexed_.find(lower);
    if (it == groups_indexed_.end()) {
        return {lower};
    }
    return groups_[it->second];
}

}  // namespace pgcpp::tsearch
