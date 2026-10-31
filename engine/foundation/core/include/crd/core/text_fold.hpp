#pragma once

// crd-core -- search folding: the form text is compared in when people's and organisations' names are matched
// (COMPLY.1's sanctions screening first; every later search reuses it). Contract: docs/design/registers.md#screening.
//
// One code point at a time, from the table scripts/gen-text-fold.py derives from the Unicode Character Database:
//
//   letters     lower case; a Latin letter with diacritics, a ligature or a fullwidth form becomes ASCII (Ş is s,
//               İ and ı are i, ø is o, ß is ss, æ is ae, Ａ is a), a Greek or Cyrillic letter loses its accent and
//               case (Ά is α, Й is и, ς is σ); digits are letters.
//   elided      combining marks and apostrophes vanish without splitting a word (O'Brien, Qaʿida, e + U+0301).
//   separators  spaces, punctuation and symbols end a word.
//
// Outside the covered blocks (Latin, Greek, Cyrillic, Armenian, Hebrew, Arabic, Georgian, the punctuation blocks and
// the fullwidth forms) a code point is a letter kept as it is. No allocation.

#include <crd/core/types.hpp>

namespace crd
{

enum class FoldKind : u8
{
    Letter,    // part of a word: `out[0..count)` is what it folds to
    Separator, // ends a word
    Elided,    // vanishes, the word continues
};

struct CodePointFold
{
    FoldKind kind = FoldKind::Letter;
    u8 count = 0U;   // 1 or 2 for a letter, 0 otherwise
    u32 out[2] = {}; // NOLINT(modernize-avoid-c-arrays): fixed storage, no heap
};

// The fold of one Unicode scalar value (a surrogate or a value past U+10FFFF is a separator).
[[nodiscard]] CodePointFold fold_code_point(u32 code_point) noexcept;

} // namespace crd
