#!/usr/bin/env python3
"""Allman braces for every C/C++ control statement (the .clang-format rule the strict tidy gate cannot see).

BreakBeforeBraces Allman, AllowShortIfStatementsOnASingleLine Never, AllowShortBlocksOnASingleLine Empty and
AllowShortLoopsOnASingleLine false: every if / else / for / while / switch body is a block whose braces stand on their
own lines, even for one statement. readability-braces-around-statements is disabled in .clang-tidy and clang-format
is never applied in place (hand formatting is kept), so this guard holds the rule.

Violations: NO_BRACE (a body without braces), ONE_LINE (`if (x) { y; }`), KR_OPEN (`if (x) {`), CUDDLED (`} else`),
MIDLINE (a control statement starting after `;`, `{`, `}` or a case label on one line) and MANUAL (one the fixer will
not rewrite; fix it by hand). Analysis runs on a mask of each file (strings, comments, raw strings and preprocessor
lines blanked), so braces inside literals and macros never count. An `else` / `#endif` / `if (...)` chain split by
the preprocessor is accepted.

  python scripts/check-allman-braces.py               # every tracked C/C++ source; nonzero on any violation
  python scripts/check-allman-braces.py <paths...>    # only these files
  python scripts/check-allman-braces.py --fix [paths] # rewrite violations in place (mechanical, token-preserving)

Generated outputs named in scripts/generated-sources.json are fixed in their generator, never here; vendored
third-party sources are listed in VENDORED.
"""
import json
import subprocess
from pathlib import Path

import re
import sys

KEYWORDS_HEAD = ("if", "for", "while", "switch")
HEAD_RE = re.compile(r"(?:else\s+)?(if|for|while|switch)\s*(?:constexpr\s*)?\(")
ELSE_RE = re.compile(r"else\b(?!\s+if\b)")
CUDDLE_RE = re.compile(r"\}\s*else\b")
WORD_RE = re.compile(r"[A-Za-z_]\w*")


class Unparseable(Exception):
    pass


def build_mask(text):
    """Return (mask, pp_lines, literal_lines). mask has the same length as text; newlines are kept.

    literal_lines: lines that begin inside a multi-line raw string (their leading whitespace is content)."""
    n = len(text)
    out = list(text)
    i = 0
    line_start = True
    pp_lines = set()
    literal_lines = set()
    line_no = 0
    in_pp = False
    while i < n:
        c = text[i]
        if c == "\n":
            line_no += 1
            in_pp = in_pp and i > 0 and text[i - 1] == "\\"
            if in_pp:
                pp_lines.add(line_no)
            line_start = True
            i += 1
            continue
        if line_start and c in " \t":
            i += 1
            continue
        if line_start:
            line_start = False
            if c == "#":
                in_pp = True
                pp_lines.add(line_no)
        if in_pp and c == "'":
            i += 1  # `#error don't ...`: an apostrophe in a directive is not a literal
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise Unparseable("unterminated block comment")
            for k in range(i, j + 2):
                if text[k] == "\n":
                    line_no += 1
                    if in_pp:
                        pp_lines.add(line_no)
                else:
                    out[k] = " "
            i = j + 2
            continue
        if c == '"' or c == "'":
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] == "_"):
                k -= 1
            word = text[k + 1:i]
            if c == '"' and word in ("R", "u8R", "uR", "UR", "LR"):
                m = re.match(r'"([^()\\\s]{0,16})\(', text[i:])
                if not m:
                    raise Unparseable("bad raw string")
                close = ")" + m.group(1) + '"'
                j = text.find(close, i + m.end())
                if j < 0:
                    raise Unparseable("unterminated raw string")
                end = j + len(close)
                for k2 in range(i + 1, end - 1):
                    if text[k2] == "\n":
                        line_no += 1
                        literal_lines.add(line_no)
                    else:
                        out[k2] = "x"
                i = end
                continue
            if c == "'" and word and word not in ("L", "u", "U", "u8"):
                i += 1  # digit separator
                continue
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "\n":
                    raise Unparseable(f"newline in literal at line {line_no + 1}")
                j += 1
            if j >= n:
                raise Unparseable("unterminated literal")
            for k in range(i + 1, j):
                out[k] = "x"
            i = j + 1
            continue
        i += 1
    lines = "".join(out).split("\n")
    for ln in pp_lines:
        if ln < len(lines):
            lines[ln] = " " * len(lines[ln])
    return "\n".join(lines), pp_lines, literal_lines


def bracket_match(mask):
    match = {}
    stack = []
    pairs = {")": "(", "]": "[", "}": "{"}
    for i, c in enumerate(mask):
        if c in "([{":
            stack.append(i)
        elif c in ")]}":
            if not stack or mask[stack[-1]] != pairs[c]:
                raise Unparseable(f"unbalanced {c} at line {mask.count(chr(10), 0, i) + 1}")
            j = stack.pop()
            match[i] = j
            match[j] = i
    if stack:
        raise Unparseable(f"unclosed bracket at line {mask.count(chr(10), 0, stack[-1]) + 1}")
    return match


def indent_of(s):
    return s[: len(s) - len(s.lstrip(" \t"))]


class FileModel:
    def __init__(self, lines):
        self.lines = lines
        self.text = "\n".join(lines)
        self.mask, self.pp, self.lit = build_mask(self.text)
        self.match = bracket_match(self.mask)
        self.starts = []
        pos = 0
        for ln in lines:
            self.starts.append(pos)
            pos += len(ln) + 1
        self.mlines = self.mask.split("\n")

    def line_of(self, pos):
        lo, hi = 0, len(self.starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.starts[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo

    def line_end(self, ln):
        return self.starts[ln] + len(self.lines[ln])

    def skip_ws(self, pos):
        n = len(self.mask)
        while pos < n and self.mask[pos] in " \t\r\n":
            pos += 1
        return pos

    def word_at(self, pos):
        m = WORD_RE.match(self.mask, pos)
        return m.group(0) if m else ""

    def skip_attributes(self, pos):
        """Position after any `[[...]]` attributes (and whitespace) starting at pos."""
        p = self.skip_ws(pos)
        while self.mask.startswith("[[", p):
            p = self.skip_ws(self.match[p] + 1)
        return p

    def stmt_end(self, pos):
        """Absolute index of the last char of the statement starting at pos (after ws), or None."""
        pos = self.skip_attributes(pos)
        if pos >= len(self.mask):
            return None
        c = self.mask[pos]
        if c == "{":
            return self.match.get(pos)
        if c in "})]#":
            return None
        w = self.word_at(pos)
        if w in KEYWORDS_HEAD:
            p = self.skip_ws(pos + len(w))
            if self.word_at(p) == "constexpr":
                p = self.skip_ws(p + len("constexpr"))
            if p >= len(self.mask) or self.mask[p] != "(":
                return None
            close = self.match[p]
            after = self.skip_ws(close + 1)
            if w == "while" and after < len(self.mask) and self.mask[after] == ";":
                return after
            e1 = self.stmt_end(close + 1)
            if e1 is None:
                return None
            if w == "if":
                nxt = self.skip_ws(e1 + 1)
                if self.word_at(nxt) == "else":
                    return self.stmt_end(nxt + 4)
            return e1
        if w == "do":
            body = self.stmt_end(pos + 2)
            if body is None:
                return None
            p = self.skip_ws(body + 1)
            if self.word_at(p) != "while":
                return None
            p = self.skip_ws(p + 5)
            if p >= len(self.mask) or self.mask[p] != "(":
                return None
            p = self.skip_ws(self.match[p] + 1)
            return p if p < len(self.mask) and self.mask[p] == ";" else None
        if w in ("try", "else", "case", "default"):
            return None
        p = pos
        n = len(self.mask)
        while p < n:
            ch = self.mask[p]
            if ch in "([{":
                p = self.match[p] + 1
                continue
            if ch in ")]}":
                return None
            if ch == ";":
                return p
            p += 1
        return None


def split_block_statements(fm, a, b):
    """Split the mask range (a, b) (the inside of a one-line block) into original-text statements."""
    pieces = []
    p = a
    start = a
    while p < b:
        ch = fm.mask[p]
        if ch in "([{":
            close = fm.match[p]
            if ch == "{":
                nxt = fm.skip_ws(close + 1)
                if nxt < b and fm.mask[nxt] not in ";,)":
                    pieces.append(fm.text[start:close + 1].strip())
                    start = close + 1
            p = close + 1
            continue
        if ch == ";":
            pieces.append(fm.text[start:p + 1].strip())
            start = p + 1
        p += 1
    tail = fm.text[start:b].strip()
    if tail:
        pieces.append(tail)
    return [x for x in pieces if x]


def split_tail(fm, start, end, ind):
    """Lines for whatever follows a converted construct on its line: '' -> ([], comment)."""
    if not fm.mask[start:end].strip():
        return [], fm.text[start:end].strip()
    out = []
    cur = ind
    p = start
    while True:
        p0 = p
        while p < end and fm.mask[p] in " \t":
            p += 1
        if p >= end:
            break
        if fm.mask[p] == "}":
            j = p + 1
            while j < end and fm.mask[j] in ");, \t":
                j += 1
            closer_ind = indent_of(fm.lines[fm.line_of(fm.match[p])])
            if not fm.mask[j:end].strip():
                out.append(closer_ind + fm.text[p:end].strip())
                break
            out.append(closer_ind + fm.text[p:j].strip())
            cur = closer_ind
            p = j
            continue
        out.append(cur + fm.text[p:end].strip())
        break
    return out, ""


def split_switch_body(fm, a, b):
    """(label?, text) items for the inside (a, b) of a one-line switch block, or None when unsure."""
    items = []
    p = fm.skip_ws(a)
    while p < b:
        w = fm.word_at(p)
        if w in ("case", "default"):
            q = p + len(w)
            while q < b:
                ch = fm.mask[q]
                if ch in "([{":
                    q = fm.match[q] + 1
                    continue
                if ch == ":" and fm.mask[q + 1] != ":" and fm.mask[q - 1] != ":":
                    break
                q += 1
            if q >= b:
                return None
            items.append((True, fm.text[p:q + 1].strip()))
            p = fm.skip_ws(q + 1)
            continue
        end = fm.stmt_end(p)
        if end is None or end >= b:
            return None
        items.append((False, fm.text[p:end + 1].strip()))
        p = fm.skip_ws(end + 1)
    return items


def shift_lines(lines, delta, floor):
    out = []
    for s in lines:
        if not s.strip() or s.lstrip().startswith("#"):
            out.append(s)
            continue
        cur = len(indent_of(s))
        new = max(floor, cur + delta)
        out.append(" " * new + s.lstrip(" \t"))
    return out


MID_RE = re.compile(r"(?<![\w.>:])(if|for|while|switch)\s*(?:constexpr\s*)?\(")
LABEL_RE = re.compile(r"^\s*(?:case\b.*|default\s*):$")


def enclosing_block(fm, pos):
    """Position of the innermost unmatched '{' before pos, or None (also None inside parentheses)."""
    p = pos - 1
    while p >= 0:
        ch = fm.mask[p]
        if ch in ")]}":
            p = fm.match[p] - 1
            continue
        if ch == "{":
            return p
        if ch in "([":
            return None
        p -= 1
    return None


def is_switch_block(fm, ob):
    p = ob - 1
    while p >= 0 and fm.mask[p] in " \t\r\n":
        p -= 1
    if p < 0 or fm.mask[p] != ")":
        return False
    p = fm.match[p] - 1
    while p >= 0 and fm.mask[p] in " \t\r\n":
        p -= 1
    return fm.mask[max(0, p - 5):p + 1] == "switch"


def midline_edit(fm, i):
    """Split a line whose first mid-line control statement follows `;`, `{`, `}` or a case label."""
    ml = fm.mlines[i]
    first = len(ml) - len(ml.lstrip())
    for m in MID_RE.finditer(ml):
        if m.start() <= first:
            continue
        before = ml[:m.start()].rstrip()
        last = before[-1:] if before else ""
        k = fm.starts[i] + m.start()
        if last == "}":
            ob = fm.match[fm.starts[i] + len(before) - 1]
            q = ob - 1
            while q >= 0 and fm.mask[q] in " \t\r\n":
                q -= 1
            if fm.mask[max(0, q - 1):q + 1] == "do" and m.group(1) == "while":
                continue  # `} while (x);` ends a do-while
        elif last == ":":
            if not LABEL_RE.match(ml[:m.start()].rstrip()):
                continue
        elif last not in (";", "{"):
            continue
        ob = enclosing_block(fm, k)
        if ob is None:
            return None
        ind = indent_of(fm.lines[i])
        if fm.line_of(ob) == i:
            col = ob - fm.starts[i]
            prefix = fm.lines[i][:col].rstrip()
            content = fm.lines[i][col + 1:].strip()
            out = ([prefix] if prefix.strip() else []) + [ind + "{", ind + "    " + content]
            return out
        base = indent_of(fm.lines[fm.line_of(ob)])
        new_ind = base + ("        " if is_switch_block(fm, ob) else "    ")
        return [fm.lines[i][:m.start()].rstrip(), new_ind + fm.lines[i][m.start():].strip()]
    return None


def analyse(fm):
    """List of (kind, first_line, last_line, replacement_lines or None)."""
    results = []
    nlines = len(fm.lines)
    for i in range(nlines):
        if i in fm.pp or i in fm.lit:
            continue
        ml = fm.mlines[i]
        ms = ml.lstrip(" \t")
        if not ms.strip():
            continue
        col0 = len(ml) - len(ms)
        ind = fm.lines[i][:col0]
        if CUDDLE_RE.match(ms):
            else_col = col0 + ms.index("else")
            results.append(("CUDDLED", i, i, [ind + "}", ind + fm.lines[i][else_col:].lstrip()]))
            continue
        hm = HEAD_RE.match(ms)
        em = None if hm else ELSE_RE.match(ms)
        if not hm and not em:
            repl = midline_edit(fm, i)
            if repl is not None:
                results.append(("MIDLINE", i, i, repl))
            continue
        if hm:
            paren = fm.starts[i] + col0 + hm.end() - 1
            close = fm.match.get(paren)
            if close is None:
                continue
            P = close + 1
            kw = hm.group(1)
        else:
            P = fm.starts[i] + col0 + 4
            kw = "else"
        # attributes such as [[unlikely]] belong to the head
        pa = fm.skip_ws(P)
        if fm.mask.startswith("[[", pa):
            P = fm.match[pa] + 1
            while fm.mask.startswith("[[", fm.skip_ws(P)) and fm.line_of(fm.skip_ws(P)) == fm.line_of(P - 1):
                P = fm.match[fm.skip_ws(P)] + 1
        h = fm.line_of(P - 1)
        if any(k in fm.pp for k in range(i, h + 1)):
            results.append(("MANUAL", i, h, None))
            continue
        h_end = fm.line_end(h)
        rest = fm.mask[P:h_end].strip()
        pcol = P - fm.starts[h]
        head_text = fm.lines[h][:pcol].rstrip()
        if rest == "":
            b = h + 1
            saw_pp = False
            while b < nlines and (not fm.mlines[b].strip()):
                saw_pp = saw_pp or b in fm.pp
                b += 1
            if b >= nlines:
                continue
            if fm.mlines[b].lstrip().startswith("{"):
                continue  # Allman already (possibly behind #if/#endif)
            if kw == "while" and fm.mlines[b].strip() == ";":
                continue
            if saw_pp and kw == "else" and fm.word_at(fm.skip_ws(fm.starts[b])) == "if":
                continue  # `else` / #endif / `if (...)`: an else-if chain split by the preprocessor
            between = [fm.lines[k].strip() for k in range(h + 1, b) if fm.lines[k].strip()]
            endif_only = saw_pp and kw == "else" and all(re.match(r"#\s*endif\b", s) for s in between)
            if saw_pp and not endif_only:
                results.append(("MANUAL", i, b, None))
                continue
            if endif_only:
                # `else` exists only under the #if; braces after the #endif are a plain block when it is off
                end = fm.stmt_end(fm.starts[b])
                if end is None or fm.mask[end + 1:fm.line_end(fm.line_of(end))].strip():
                    results.append(("MANUAL", i, b, None))
                    continue
                e = fm.line_of(end)
                body = fm.lines[b:e + 1]
                if any(s.strip() and len(indent_of(s)) <= len(ind) and not s.lstrip().startswith("#") for s in body):
                    body = shift_lines(body, 4, len(ind) + 4)
                repl = fm.lines[i:b] + [ind + "{"] + body + [ind + "}"]
                results.append(("NO_BRACE", i, e, repl))
                continue
            end = fm.stmt_end(fm.starts[b])
            if end is None:
                results.append(("MANUAL", i, b, None))
                continue
            e = fm.line_of(end)
            body_kw = fm.word_at(fm.skip_attributes(fm.starts[b]))
            structural = body_kw in KEYWORDS_HEAD or body_kw == "do" or fm.mlines[b].lstrip().startswith("{")
            # directives inside a brace-delimited body are harmless; in a plain statement they are not
            if any(k in fm.lit or (k in fm.pp and not structural) for k in range(h + 1, e + 1)):
                results.append(("MANUAL", i, e, None))
                continue
            body = fm.lines[h + 1:e + 1]
            under = any(s.strip() and len(indent_of(s)) <= len(ind) for s in body)
            if under and not structural:
                results.append(("MANUAL", i, e, None))
                continue
            if under:
                body = shift_lines(body, 4, len(ind) + 4)
            tail, comment = split_tail(fm, end + 1, fm.line_end(e), ind)
            if tail:
                last = fm.lines[e][: end + 1 - fm.starts[e]].rstrip()
                body = body[:-1] + [shift_lines([last], 4 if under else 0, 0)[0]]
            repl = fm.lines[i:h + 1] + [ind + "{"] + body + [ind + "}"] + tail
            results.append(("NO_BRACE", i, e, repl))
            continue
        q = fm.skip_ws(P)
        if fm.mask[q] == "{":
            M = fm.match[q]
            mline = fm.line_of(M)
            inner = fm.mask[q + 1:M]
            if mline == h:
                if not inner.strip():
                    continue  # empty block: allowed
                if kw == "switch":
                    items = split_switch_body(fm, q + 1, M)
                    if not items:
                        results.append(("MANUAL", i, h, None))
                        continue
                    body = [ind + ("    " if label else "        ") + t for label, t in items]
                else:
                    stmts = split_block_statements(fm, q + 1, M)
                    if not stmts:
                        continue
                    body = [ind + "    " + s for s in stmts]
                tail, comment = split_tail(fm, M + 1, h_end, ind)
                head = head_text + ((" " + comment) if comment else "")
                repl = fm.lines[i:h] + [head, ind + "{"] + body + [ind + "}"] + tail
                results.append(("ONE_LINE", i, h, repl))
                continue
            after_m = fm.mask[q + 1:h_end]
            if after_m.strip():
                # content after the K&R brace: move it to its own line inside the block
                repl = fm.lines[i:h] + [head_text, ind + "{", ind + "    " + fm.text[q + 1:h_end].strip()]
                results.append(("KR_OPEN", i, h, repl))
                continue
            comment = fm.text[q + 1:h_end].strip()
            repl = fm.lines[i:h] + [head_text, ind + "{" + ((" " + comment) if comment else "")]
            results.append(("KR_OPEN", i, h, repl))
            continue
        # same-line body without braces
        if rest == ";":
            continue
        end = fm.stmt_end(q)
        if end is None:
            results.append(("MANUAL", i, h, None))
            continue
        e = fm.line_of(end)
        if any(k in fm.pp or k in fm.lit for k in range(h + 1, e + 1)):
            results.append(("MANUAL", i, e, None))
            continue
        qcol = q - fm.starts[h]
        first = fm.text[q:fm.line_end(h) if e > h else end + 1].strip()
        body_kw = fm.word_at(q)
        if e > h:
            delta = 4 if (body_kw in KEYWORDS_HEAD or body_kw == "do") else (len(ind) + 4 - qcol)
            mid = shift_lines(fm.lines[h + 1:e], delta, len(ind) + 4)
            last_text = fm.lines[e][: end + 1 - fm.starts[e]].rstrip()
            last = shift_lines([last_text], delta, len(ind) + 4)[0]
            body = [ind + "    " + first] + mid + [last]
        else:
            body = [ind + "    " + first]
        tail, comment = split_tail(fm, end + 1, fm.line_end(e), ind)
        if kw == "else" and tail and tail[0].lstrip().startswith("else"):
            results.append(("MANUAL", i, e, None))
            continue
        head = head_text + ((" " + comment) if comment else "")
        repl = fm.lines[i:h] + [head, ind + "{"] + body + [ind + "}"] + tail
        results.append(("NO_BRACE", i, e, repl))
    return results


def read_file(path):
    raw = open(path, "rb").read()
    text = raw.decode("utf-8")
    bom = text.startswith("﻿")
    if bom:
        text = text[1:]
    crlf = text.count("\r\n")
    lf = text.count("\n")
    eol = "\r\n" if crlf * 2 > lf else "\n"
    lines = [l[:-1] if l.endswith("\r") else l for l in text.split("\n")]
    return lines, eol, bom, (crlf != 0 and crlf != lf)


def write_file(path, lines, eol, bom):
    text = eol.join(lines)
    if bom:
        text = "﻿" + text
    open(path, "wb").write(text.encode("utf-8"))


def fix_lines(lines, max_passes=60):
    counts = {}
    manual = []
    for _ in range(max_passes):
        fm = FileModel(lines)
        res = analyse(fm)
        edits = [(a, b, k, r) for k, a, b, r in res if r is not None]
        manual = [(a, b, fm.lines[a]) for k, a, b, r in res if r is None]
        if not edits:
            return lines, counts, manual
        edits.sort(key=lambda t: (t[0], -(t[1] - t[0])))
        chosen = []
        last_end = -1
        for a, b, kind, repl in edits:
            if a > last_end:
                chosen.append((a, b, kind, repl))
                last_end = b
        for a, b, kind, repl in sorted(chosen, reverse=True):
            lines[a:b + 1] = repl
            counts[kind] = counts.get(kind, 0) + 1
    raise Unparseable("no fixpoint")


ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl", ".ipp")
VENDORED = (
    "bench/reference/shewchuk-predicates.c",  # Shewchuk's public-domain predicates, kept verbatim as a reference
    "engine/numerics/hesap-tensor/include/crd/hesap/tensor/detail/dlpack.h",  # upstream DLPack header
)


def tracked_sources():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", *("*" + e for e in EXTENSIONS)],
                         capture_output=True, text=True, check=True).stdout.split()
    manifest = json.loads((ROOT / "scripts/generated-sources.json").read_text(encoding="utf-8"))
    generated = {e["path"] for e in manifest["entries"]}
    return [p for p in out if p not in generated and p not in VENDORED]


def scan(path):
    """[(line, kind, text)] for one file; raises Unparseable."""
    lines, _, _, _ = read_file(path)
    fm = FileModel(lines)
    return [(a + 1, kind, fm.lines[a].strip()) for kind, a, _, _ in analyse(fm)]


def main(argv):
    fix = "--fix" in argv
    paths = [a for a in argv if a != "--fix"]
    files = paths if paths else tracked_sources()
    violations = 0
    errors = 0
    rewritten = 0
    for rel in files:
        path = rel if Path(rel).is_absolute() else str(ROOT / rel)
        try:
            if fix:
                lines, eol, bom, _ = read_file(path)
                new, counts, _ = fix_lines(list(lines))
                FileModel(new)  # the result must still parse and balance
                if counts:
                    write_file(path, new, eol, bom)
                    rewritten += 1
            for line, kind, text in scan(path):
                violations += 1
                print(f"{rel}:{line}: {kind}: {text[:140]}")
        except (Unparseable, UnicodeDecodeError) as exc:
            errors += 1
            print(f"{rel}: ERROR: cannot analyse ({exc})")
    if fix:
        print(f"Rewrote {rewritten} file(s).")
    print(f"Checked {len(files)} C/C++ sources for Allman control-statement braces.")
    if violations or errors:
        print(f"FAIL: {violations} violation(s), {errors} unanalysable file(s)"
              + ("" if fix else "; `--fix` rewrites the mechanical ones"))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
