#pragma once

// crd-comply -- comparing people's and organisations' names, the matcher sanctions screening runs (COMPLY.1;
// contract: docs/design/registers.md#screening).
//
// fold_name() reads a name as words in search-folded form (crd/core/text_fold.hpp: case, diacritics, Turkish dotted
// and dotless i, ligatures, apostrophes, punctuation), joins a run of single letters that spells a legal form
// (A.Ş., L.L.C.) and drops honorifics and legal forms unless nothing else is left.
//
// name_score() rates two folded names from 0 to 1000:
//
//   words      each word pairs with at most one word of the other name, best pairs first; a pair rates by
//              Jaro-Winkler, an initial against a word it starts (0.9), and 0.9 at least when both spell the same
//              consonant skeleton (Muhammad and Mohamed, Yusuf and Yousef, Petr and Pyotr); pairs below 0.88 do
//              not pair. The score is the pairs' similarity weighted by both words' lengths over both names' total.
//   subset     when the names have different word counts and the shorter has at least two words, the shorter
//              name's covered share less 0.05 for each unpaired word of the longer one also counts (a patronymic
//              or middle name missing on one side, Oleg Deripaska against Oleg Vladimirovich Deripaska: 950).
//   joined     when the word counts differ, the Levenshtein similarity of the words written together also counts
//              (Abdulrahman against Abdul Rahman, up to 128 code points a side).
//
// The highest of the three is the score. Word order never matters (Kaya Ayşe and Ayse Kaya score 1000).

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::comply
{
namespace cont = crd::containers;

inline constexpr u32 kMaxNameWords = 16U;      // words of one name compared; later words are not read
inline constexpr u32 kMaxWordCodePoints = 64U; // code points of one word compared; the rest of a word is not read
inline constexpr u32 kMaxScore = 1000U;

// A name as words in search-folded form, each with its consonant skeleton (empty unless the word is ASCII).
struct FoldedName
{
    cont::Array<u32> text;      // the words' code points, back to back
    cont::Array<u32> ends;      // each word's end in `text`
    cont::Array<u32> skeletons; // the words' skeletons, back to back
    cont::Array<u32> skeleton_ends;
};

// Ill-formed UTF-8 bytes end a word, as a separator does.
[[nodiscard]] FoldedName fold_name(cont::StringView name);

// 0 when either name has no words.
[[nodiscard]] u32 name_score(const FoldedName& a, const FoldedName& b);
} // namespace crd::comply
