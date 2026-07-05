#!/usr/bin/env python3
"""fma_rewrite_hier.py — FMA rewrite of the surviving hand-tracked hier_codelets.hpp (2026-07-04 crush).

Transforms the two generated complex-multiply statement shapes into single-rounded FMA form
(the same forms scripts/gen_fft_batched.py now emits):

  A) const V {a}r = {C} * {x}r - {D} * {x}i, {a}i = {C} * {x}i + {D} * {x}r;
     -> const V {a}r = simd::fnmadd({D}, {x}i, {C} * {x}r), {a}i = simd::fma({D}, {x}r, {C} * {x}i);
     ({C}/{D} = V(...) constant broadcasts, possibly static_cast<crd::f32>(...) inside)

  B) [const ]V {o1} = {z}r * {w}r - {z}i * {w}i, {o2} = {z}r * {w}i + {z}i * {w}r;
     -> ... = simd::fnmadd({w}i, {z}i, {z}r * {w}r), ... = simd::fma({w}i, {z}r, {z}i * {w}r);
     (runtime twiddle registers)

Shape-STRICT: only full statements matching these exact forms are touched; everything else is left
byte-identical. Idempotent (rewritten lines no longer match). Run:
    python3 scripts/fma_rewrite_hier.py engine/hesap-fft/include/crd/hesap/fft/detail/hier_codelets.hpp
"""
import re
import sys

VC = r"V\((?:static_cast<crd::f32>\()?-?[0-9][0-9.eE+-]*[fF]?\)?\)"  # V(c) / V(static_cast<crd::f32>(c))
ID = r"[A-Za-z_][A-Za-z0-9_]*"

# A) constant cmul: groups: pre, a, C1, x1, D1, x2, a2, C2, x3, D2, x4
RE_A = re.compile(
    rf"^(\s*)const V ({ID})r = ({VC}) \* ({ID})r - ({VC}) \* \4i, \2i = ({VC}) \* \4i \+ ({VC}) \* \4r;$"
)

# B) runtime twiddle: [const ]V o1 = zr * wr - zi * wi, o2 = zr * wi + zi * wr;
RE_B = re.compile(
    rf"^(\s*)(const V|V) ({ID}) = ({ID})r \* ({ID})r - \4i \* \5i, ({ID}) = \4r \* \5i \+ \4i \* \5r;$"
)


def rewrite(text):
    out = []
    n_a = n_b = 0
    for line in text.splitlines():
        m = RE_A.match(line)
        if m:
            ind, a, c1, x, d1, c2, d2 = m.groups()
            if c1 == c2 and d1 == d2:  # the two halves must share the same constants
                out.append(
                    f"{ind}const V {a}r = simd::fnmadd({d1}, {x}i, {c1} * {x}r), "
                    f"{a}i = simd::fma({d1}, {x}r, {c1} * {x}i);"
                )
                n_a += 1
                continue
        m = RE_B.match(line)
        if m:
            ind, kw, o1, z, w, o2 = m.groups()
            out.append(
                f"{ind}{kw} {o1} = simd::fnmadd({w}i, {z}i, {z}r * {w}r), "
                f"{o2} = simd::fma({w}i, {z}r, {z}i * {w}r);"
            )
            n_b += 1
            continue
        out.append(line)
    return "\n".join(out) + "\n", n_a, n_b


def main():
    path = sys.argv[1]
    with open(path, encoding="utf-8") as f:
        text = f.read()
    new, n_a, n_b = rewrite(text)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    print(f"cmul-const rewritten: {n_a}; twiddle-var rewritten: {n_b}")


if __name__ == "__main__":
    main()
