// snowball.cpp — Porter/Snowball English stemmer implementation.
//
// Implements the classic Porter (1980) stemming algorithm, which is the
// algorithm underlying PostgreSQL's "english" Snowball stemmer. The algorithm
// applies a sequence of measure-based (m) suffix transformation steps to
// reduce inflected English words to their stem.
//
// Reference: Porter, M.F. (1980) "An algorithm for suffix stripping",
//            Program 14(3): 130-137.
//            http://snowball.tartarus.org/algorithms/porter/stemmer.html
//
// Steps:
//   1a: plurals      (sses->ss, ies->i, ss->ss, s->)
//   1b: past/gerund  (eed->ee if m>0, (*v*) ed->, (*v*) ing->)
//   1b2: cleanup     (if 1b removed ed/ing -> at, bl, iz -> +e; double
//                      consonant (not l,s,z) -> single; m=1 *o -> +e)
//   1c: y->i         ((*v*) y -> i)
//   2:  m>0 transforms (ational->ate, tional->tion, ...)
//   3:  m>0 transforms (icate->ic, ative->, ...)
//   4:  m>1 removals   (al, ance, ence, er, ic, able, ..., ion (s|t))
//   5a: final e        (m>1 -> ; m=1 not *o -> )
//   5b: double l       (m>1 *l -> l)

#include "tsearch/snowball.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace pgcpp::tsearch {

namespace {

// A working buffer for the stemmer. The Porter algorithm manipulates a
// mutable string while tracking the "stem end" (k) and a limit (j).
struct StemBuffer {
    std::string b;  // lowercased word
    int k = 0;      // index of last char (0-based); -1 means empty
};

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool IsConsonant(const std::string& b, int i) {
    char c = b[static_cast<std::size_t>(i)];
    switch (c) {
        case 'a':
        case 'e':
        case 'i':
        case 'o':
        case 'u':
            return false;
        case 'y':
            // y is consonant if first char or preceded by vowel; vowel if
            // preceded by consonant.
            return i == 0 || !IsConsonant(b, i - 1);
        default:
            return true;
    }
}

// Measure m: count of vowel-consonant sequences in b[0..j]. A VC sequence is
// a vowel followed by a consonant. m is the number of complete VC pairs.
int Measure(const std::string& b, int j) {
    int n = 0;
    int i = 0;
    while (true) {
        if (i > j)
            return n;
        if (!IsConsonant(b, i))
            break;
        ++i;
    }
    ++i;  // skip the vowel
    while (true) {
        while (true) {
            if (i > j)
                return n;
            if (IsConsonant(b, i))
                break;
            ++i;
        }
        ++i;  // skip consonants
        ++n;
        while (true) {
            if (i > j)
                return n;
            if (!IsConsonant(b, i))
                break;
            ++i;
        }
        ++i;  // skip the vowel
    }
}

// Has a vowel in b[0..j]?
bool HasVowel(const std::string& b, int j) {
    for (int i = 0; i <= j; ++i) {
        if (!IsConsonant(b, i))
            return true;
    }
    return false;
}

// Double consonant: b[j-1]==b[j] and b[j] is a consonant.
bool DoubleConsonant(const std::string& b, int j) {
    if (j < 1)
        return false;
    if (b[static_cast<std::size_t>(j)] != b[static_cast<std::size_t>(j - 1)])
        return false;
    return IsConsonant(b, j);
}

// *o: stem ends with cvc where second c is not W, X, Y (used in step 1b/5a).
bool EndsCvc(const std::string& b, int j) {
    if (j < 2)
        return false;
    if (!IsConsonant(b, j))
        return false;
    if (IsConsonant(b, j - 1))
        return false;
    if (!IsConsonant(b, j - 2))
        return false;
    char c = b[static_cast<std::size_t>(j)];
    if (c == 'w' || c == 'x' || c == 'y')
        return false;
    return true;
}

// Does b end with `s` (case-sensitive, b is lowercased)? If so set j to the
// index just before the suffix and return true; else return false and leave
// j unchanged.
bool Ends(std::string& b, int& j, std::string_view s) {
    int slen = static_cast<int>(s.size());
    if (slen > j + 1)
        return false;
    int start = j + 1 - slen;
    for (int i = 0; i < slen; ++i) {
        if (b[static_cast<std::size_t>(start + i)] != s[static_cast<std::size_t>(i)])
            return false;
    }
    j = start - 1;
    return true;
}

// Set b to end with `s` (replacing the suffix matched by Ends). j is set to
// the new last index.
void SetTo(std::string& b, int& j, std::string_view s) {
    b.resize(static_cast<std::size_t>(j + 1));
    b.append(s);
    j = static_cast<int>(b.size()) - 1;
}

// Replace suffix with `s` if the condition `cond` (m > value) holds.
void ReplaceIfM(std::string& b, int& j, std::string_view s, std::string_view suffix,
                int min_measure) {
    // Ends was already called to position j just before the suffix; recompute
    // the measure on the current stem region b[0..j].
    if (Measure(b, j) > min_measure) {
        SetTo(b, j, s);
    } else {
        // Restore the suffix.
        b.resize(static_cast<std::size_t>(j + 1));
        b.append(suffix);
        j = static_cast<int>(b.size()) - 1;
    }
}

// --- Step 1a ---
void Step1a(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    if (Ends(b, j, "sses")) {
        b.resize(static_cast<std::size_t>(j + 1));
        b.append("ss");
        k = static_cast<int>(b.size()) - 1;
    } else {
        j = k;
        if (Ends(b, j, "ies")) {
            b.resize(static_cast<std::size_t>(j + 1));
            b.append("i");
            k = static_cast<int>(b.size()) - 1;
        } else {
            j = k;
            if (Ends(b, j, "ss")) {
                // keep
            } else {
                j = k;
                if (Ends(b, j, "s")) {
                    b.resize(static_cast<std::size_t>(j + 1));
                    k = static_cast<int>(b.size()) - 1;
                }
            }
        }
    }
}

// --- Step 1b ---
void Step1b(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    if (Ends(b, j, "eed")) {
        if (Measure(b, j) > 0) {
            b.resize(static_cast<std::size_t>(j + 1));
            b.append("ee");
            k = static_cast<int>(b.size()) - 1;
        }
    } else {
        j = k;
        bool removed = false;
        if (Ends(b, j, "ed")) {
            if (HasVowel(b, j)) {
                b.resize(static_cast<std::size_t>(j + 1));
                k = static_cast<int>(b.size()) - 1;
                removed = true;
            } else {
                // restore
                b.resize(static_cast<std::size_t>(j + 1));
                b.append("ed");
                k = static_cast<int>(b.size()) - 1;
            }
        } else {
            j = k;
            if (Ends(b, j, "ing")) {
                if (HasVowel(b, j)) {
                    b.resize(static_cast<std::size_t>(j + 1));
                    k = static_cast<int>(b.size()) - 1;
                    removed = true;
                } else {
                    b.resize(static_cast<std::size_t>(j + 1));
                    b.append("ing");
                    k = static_cast<int>(b.size()) - 1;
                }
            }
        }
        if (removed) {
            // Step 1b post-cleanup: AT->ATE, BL->BLE, IZ->IZE (append 'e').
            // Check the last two chars directly to avoid Ends() side effects.
            if (b.size() >= 2) {
                std::string last2 = b.substr(b.size() - 2);
                if (last2 == "at" || last2 == "bl" || last2 == "iz") {
                    b.push_back('e');
                    k = static_cast<int>(b.size()) - 1;
                    return;
                }
            }
            // (*d and not (*L or *S or *Z)) -> single letter
            if (DoubleConsonant(b, k) &&
                !(b[static_cast<std::size_t>(k)] == 'l' || b[static_cast<std::size_t>(k)] == 's' ||
                  b[static_cast<std::size_t>(k)] == 'z')) {
                b.resize(static_cast<std::size_t>(b.size()) - 1);
                k = static_cast<int>(b.size()) - 1;
            } else if (Measure(b, k) == 1 && EndsCvc(b, k)) {
                // (m=1 and *o) -> E
                b.push_back('e');
                k = static_cast<int>(b.size()) - 1;
            }
        }
    }
}

// --- Step 1c ---
void Step1c(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    if (Ends(b, j, "y")) {
        if (HasVowel(b, j)) {
            b[static_cast<std::size_t>(k)] = 'i';
        }
    }
}

// --- Step 2 ---
void Step2(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    // Order matters; check longer suffixes first where overlap exists.
    if (Ends(b, j, "ational")) {
        ReplaceIfM(b, j, "ate", "ational", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "tional")) {
        ReplaceIfM(b, j, "tion", "tional", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "enci")) {
        ReplaceIfM(b, j, "ence", "enci", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "anci")) {
        ReplaceIfM(b, j, "ance", "anci", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "izer")) {
        ReplaceIfM(b, j, "ize", "izer", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "abli")) {
        ReplaceIfM(b, j, "able", "abli", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "alli")) {
        ReplaceIfM(b, j, "al", "alli", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "entli")) {
        ReplaceIfM(b, j, "ent", "entli", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "eli")) {
        ReplaceIfM(b, j, "e", "eli", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ousli")) {
        ReplaceIfM(b, j, "ous", "ousli", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ization")) {
        ReplaceIfM(b, j, "ize", "ization", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ation")) {
        ReplaceIfM(b, j, "ate", "ation", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ator")) {
        ReplaceIfM(b, j, "ate", "ator", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "alism")) {
        ReplaceIfM(b, j, "al", "alism", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "iveness")) {
        ReplaceIfM(b, j, "ive", "iveness", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "fulness")) {
        ReplaceIfM(b, j, "ful", "fulness", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ousness")) {
        ReplaceIfM(b, j, "ous", "ousness", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "aliti")) {
        ReplaceIfM(b, j, "al", "aliti", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "iviti")) {
        ReplaceIfM(b, j, "ive", "iviti", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "biliti")) {
        ReplaceIfM(b, j, "ble", "biliti", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
}

// --- Step 3 ---
void Step3(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    if (Ends(b, j, "icate")) {
        ReplaceIfM(b, j, "ic", "icate", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ative")) {
        ReplaceIfM(b, j, "", "ative", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "alize")) {
        ReplaceIfM(b, j, "al", "alize", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "iciti")) {
        ReplaceIfM(b, j, "ic", "iciti", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ical")) {
        ReplaceIfM(b, j, "ic", "ical", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ful")) {
        ReplaceIfM(b, j, "", "ful", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
    j = k;
    if (Ends(b, j, "ness")) {
        ReplaceIfM(b, j, "", "ness", 0);
        k = static_cast<int>(b.size()) - 1;
        return;
    }
}

// --- Step 4 ---
void Step4(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    // Each suffix is removed only if m > 1.
    static constexpr std::string_view kSuffixes[] = {
        "al",   "ance", "ence", "er",  "ic",  "able", "ible", "ant", "ement",
        "ment", "ent",  "ou",   "ism", "ate", "iti",  "ous",  "ive", "ize",
    };
    for (auto suf : kSuffixes) {
        j = k;
        if (Ends(b, j, suf)) {
            if (Measure(b, j) > 1) {
                b.resize(static_cast<std::size_t>(j + 1));
                k = static_cast<int>(b.size()) - 1;
            }
            // else: leave unchanged (Ends already restored via b unchanged)
            return;
        }
    }
    // Special: "ion" removed if m > 1 AND preceded by s or t.
    j = k;
    if (Ends(b, j, "ion")) {
        if (j >= 0 &&
            (b[static_cast<std::size_t>(j)] == 's' || b[static_cast<std::size_t>(j)] == 't')) {
            if (Measure(b, j) > 1) {
                b.resize(static_cast<std::size_t>(j + 1));
                k = static_cast<int>(b.size()) - 1;
            }
        }
    }
}

// --- Step 5a ---
void Step5a(StemBuffer& sb) {
    auto& [b, k] = sb;
    int j = k;
    if (Ends(b, j, "e")) {
        int m = Measure(b, j);
        if (m > 1 || (m == 1 && !EndsCvc(b, j))) {
            b.resize(static_cast<std::size_t>(j + 1));
            k = static_cast<int>(b.size()) - 1;
        }
    }
}

// --- Step 5b ---
void Step5b(StemBuffer& sb) {
    auto& [b, k] = sb;
    if (Measure(b, k) > 1 && DoubleConsonant(b, k) && b[static_cast<std::size_t>(k)] == 'l') {
        b.resize(static_cast<std::size_t>(b.size()) - 1);
        k = static_cast<int>(b.size()) - 1;
    }
}

bool IsAllLetters(std::string_view word) {
    for (char c : word) {
        if (!std::isalpha(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

}  // namespace

std::string SnowballStemmer::Stem(std::string_view word) const {
    if (word.empty())
        return {};
    std::string lower = ToLower(word);
    // The Porter algorithm is defined for alphabetic input only; non-letter
    // characters yield undefined behavior in the measure computation. Return
    // the lowercased word unchanged when it contains non-letters.
    if (!IsAllLetters(lower))
        return lower;
    if (lower.size() <= 2)
        return lower;  // Porter leaves words of length <= 2

    StemBuffer sb;
    sb.b = std::move(lower);
    sb.k = static_cast<int>(sb.b.size()) - 1;

    Step1a(sb);
    Step1b(sb);
    Step1c(sb);
    Step2(sb);
    Step3(sb);
    Step4(sb);
    Step5a(sb);
    Step5b(sb);

    return sb.b;
}

Lexeme SnowballDict::Lexicalize(std::string_view word) const {
    Lexeme lex;
    std::string lower = ToLower(word);
    if (!stop_words_.empty()) {
        for (const auto& sw : stop_words_) {
            if (lower == sw) {
                lex.text = lower;
                lex.is_stop = true;
                return lex;
            }
        }
    }
    SnowballStemmer stemmer;
    lex.text = stemmer.Stem(word);
    lex.is_stop = false;
    return lex;
}

}  // namespace pgcpp::tsearch
