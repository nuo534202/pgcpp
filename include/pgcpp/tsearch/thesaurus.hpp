#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// Thesaurus (PostgreSQL src/backend/tsearch/thesaurus.c).
//
// Maps a word to a group of synonyms. When any word in a synonym group is
// looked up, the entire group (including the queried word) is returned.
// ---------------------------------------------------------------------------

class Thesaurus {
public:
    // Add a group of synonyms. All words in the list become mutual synonyms.
    // Case-insensitive: words are stored lowercased.
    void AddSynonyms(std::vector<std::string> words);

    // Look up synonyms for `word`. Returns a vector containing the word and
    // all its synonyms. If the word has no synonym group, returns a vector
    // containing only the lowercased word itself.
    std::vector<std::string> Lookup(std::string_view word) const;

    // Number of synonym groups.
    std::size_t Size() const { return groups_.size(); }

private:
    // Maps each word to the index of its synonym group in `groups_`.
    std::map<std::string, std::size_t> groups_indexed_;
    std::vector<std::vector<std::string>> groups_;
};

}  // namespace pgcpp::tsearch
