#!/usr/bin/env python3
"""Generate crd-core's search-folding table from the Unicode Character Database Python carries.

Name screening (COMPLY.1's sanctions lists) and every later search compare text after folding it: letters lower case,
Latin letters with diacritics, ligatures and fullwidth forms reduced to ASCII, combining marks and apostrophes
elided, punctuation and spaces turned into token separators. engine/foundation/core/src/text_fold.cpp applies the
table this script writes, engine/foundation/core/src/text_fold_table.inc:

  ranges    the runs of code points that are not letters or digits, each a separator or elided, inside the covered
            blocks (outside them every code point is a letter kept as it is).
  mappings  each letter of the covered blocks that folds to something else: up to two code points, ASCII for Latin.

A letter folds by the first rule that gives a result:
  1. MANUAL below: letters with no decomposition whose reading is fixed by convention (sharp s, thorn, eth, eng,
     schwa, ...).
  2. NFKD with the combining marks dropped and case-folded, when that leaves only ASCII letters and digits.
  3. A Latin letter's UCD name, LATIN [CAPITAL|SMALL] LETTER [modifiers] X: X lower-cased (o with stroke is o,
     dotless i is i, small capital a is a).
  4. NFKD with the marks dropped and case-folded, when that leaves one or two code points other than the letter
     itself (Greek and Cyrillic lose their accents and case); one letter that folds further is folded again.

The covered blocks (COVERED) were fully assigned before Unicode 15.0 and hold the scripts sanctions lists print
names in; the output is the same on every Python from 3.12 (UCD 15.0) on, which the drift test holds on the hosted
lanes and the developer hosts alike.

  python scripts/gen-text-fold.py           # write the table
  python scripts/gen-text-fold.py --check   # byte-compare with the committed table; nonzero on drift

The crd-core-text-fold-drift CTest runs --check; scripts/generated-sources.json records the file's bytes.
"""
import argparse
import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = 'engine/foundation/core/src/text_fold_table.inc'
GENERATOR = 'scripts/gen-text-fold.py'

# Basic Latin to Cyrillic Supplement, Armenian, Hebrew, Arabic, Georgian, Latin Extended Additional and Greek
# Extended, General Punctuation, CJK Symbols and Punctuation, Halfwidth and Fullwidth Forms.
COVERED = [(0x0000, 0x058F), (0x0590, 0x05FF), (0x0600, 0x06FF), (0x10A0, 0x10FF), (0x1E00, 0x1FFF),
           (0x2000, 0x206F), (0x3000, 0x303F), (0xFF00, 0xFFEF)]

# Apostrophes and transliteration's ayn and hamza: elided without splitting a token (O'Brien is obrien, Qa'ida and
# Qaʿida are qaida). The spacing modifier letters (U+02B0 to U+02FF) are elided whole.
ELIDED = {0x0027, 0x0060, 0x00B4, 0x2018, 0x2019, 0x201B, 0x2032}
MODIFIER_LETTERS = (0x02B0, 0x02FF)

MANUAL = {
    0x00DF: 'ss',  # sharp s
    0x1E9E: 'ss',  # capital sharp s
    0x00DE: 'th', 0x00FE: 'th',  # thorn
    0x00D0: 'd', 0x00F0: 'd',  # eth
    0x014A: 'n', 0x014B: 'n',  # eng
    0x0138: 'k',  # kra
    0x01F7: 'w', 0x01BF: 'w',  # wynn
    0x021C: 'y', 0x021D: 'y',  # yogh
    0x01B7: 'z', 0x0292: 'z',  # ezh
    0x018F: 'a', 0x0259: 'a',  # schwa: Azerbaijani names romanise it as a (Əliyev, Aliyev)
    0x0152: 'oe', 0x0153: 'oe',  # the oe ligature, named LIGATURE rather than LETTER
}

NAME = re.compile(r'^LATIN (?:CAPITAL |SMALL )?LETTER '
                  r'(?:(?:SMALL CAPITAL|TURNED|REVERSED|SCRIPT|CLOSED|OPEN|DOTLESS|BARRED|LONG|SIDEWAYS|INVERTED)'
                  r' )*([A-Z]{1,2})(?: |$)')


def covered(cp):
    return any(first <= cp <= last for first, last in COVERED)


def kind_of(cp):
    """'letter', 'separator' or 'elided'."""
    if cp in ELIDED or MODIFIER_LETTERS[0] <= cp <= MODIFIER_LETTERS[1]:
        return 'elided'
    category = unicodedata.category(chr(cp))
    if category[0] in 'LN':
        return 'letter'
    if category[0] == 'M':
        return 'elided'
    return 'separator'


def stripped(cp):
    """NFKD without the combining marks, case-folded (final sigma is sigma)."""
    decomposed = unicodedata.normalize('NFKD', chr(cp))
    return ''.join(c for c in decomposed if unicodedata.category(c)[0] != 'M').casefold()


def fold_of(cp):
    """The letter's fold, a string of one or two code points, or None when it stays as it is."""
    if cp in MANUAL:
        return MANUAL[cp]
    text = stripped(cp)
    if text and all(c.isascii() and c.isalnum() for c in text):
        return text if text != chr(cp) else None
    match = NAME.match(unicodedata.name(chr(cp), ''))
    if match:
        return match.group(1).lower()
    if 1 <= len(text) <= 2 and text != chr(cp) and all(kind_of(ord(c)) == 'letter' for c in text):
        # One letter that itself folds further (capital ezh with caron is small ezh, which is z).
        return (fold_of(ord(text)) or text) if len(text) == 1 else text
    return None


def tables():
    ranges = []
    mappings = []
    for first, last in COVERED:
        for cp in range(first, last + 1):
            if 0xD800 <= cp <= 0xDFFF:
                continue
            kind = kind_of(cp)
            if kind != 'letter':
                if ranges and ranges[-1][2] == kind and ranges[-1][1] == cp - 1:
                    ranges[-1][1] = cp
                else:
                    ranges.append([cp, cp, kind])
                continue
            fold = fold_of(cp)
            if fold is not None:
                mappings.append((cp, [ord(c) for c in fold]))
    # Folding twice is folding once: every code point a letter folds to is a letter that folds to itself.
    folded = dict(mappings)
    for cp, out in mappings:
        for target in out:
            if (covered(target) and kind_of(target) != 'letter') or folded.get(target, [target]) != [target]:
                raise SystemExit(f'FAIL: U+{cp:04X} folds to U+{target:04X}, which is no fixed point')
    return ranges, mappings


def render():
    ranges, mappings = tables()
    kinds = {'separator': 'FoldKind::Separator', 'elided': 'FoldKind::Elided'}
    lines = [
        f'// Generated by {GENERATOR} from the Unicode Character Database; never edit by hand.',
        '// Regenerate with: python scripts/gen-text-fold.py',
        '',
        '// The runs of code points that are not letters or digits inside the covered blocks.',
        'constexpr FoldRange kFoldRanges[] = { // NOLINT(modernize-avoid-c-arrays)',
    ]
    for first, last, kind in ranges:
        lines.append(f'    {{0x{first:04X}U, 0x{last:04X}U, {kinds[kind]}}},')
    lines += ['};', '', '// Each covered letter that folds: the code point and up to two code points it folds to.',
              'constexpr FoldMapping kFoldMappings[] = { // NOLINT(modernize-avoid-c-arrays)']
    for cp, out in mappings:
        second = out[1] if len(out) > 1 else 0
        lines.append(f'    {{0x{cp:04X}U, 0x{out[0]:04X}U, 0x{second:04X}U}},')
    lines += ['};', '']
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Generate crd-core\'s search-folding table')
    parser.add_argument('--check', action='store_true', help='compare with the committed table instead of writing')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    args = parser.parse_args(argv)
    path = Path(args.root).resolve() / OUT
    text = render().encode('utf-8')
    if args.check:
        if not path.exists() or path.read_bytes() != text:
            print(f'FAIL: {OUT} differs from {GENERATOR}\'s output; run python {GENERATOR}')
            return 1
        print(f'search-folding table current (Unicode {unicodedata.unidata_version} on this host)')
        return 0
    path.write_bytes(text)
    print(f'wrote {OUT}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
