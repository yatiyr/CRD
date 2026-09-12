#!/usr/bin/env python3
"""gen_matrix.py -- CEIR-0g §4 step-4: emit the §174 capability MATRIX from the
machine-readable manifest, and run the manifest's health CHECKS.

Reads  docs/capabilities/gpu-platform-capabilities.toml  (schema=2, the two-axis model).
Writes docs/generated/gpu-platform-capability-matrix.md   (a table + a CHECKS report).

Stdlib only (tomllib, Python >= 3.11) -- the tools/ceir_opgen convention. Deterministic
LF/UTF-8 output so a drift check can guard the committed matrix.

CHECK TIERS (a field's tier is decided by what a wrong/absent value COSTS):
  HARD (exit 2)  -- load-bearing fields + vocab/range/invariant/stale-path: a future CI gate fails.
  documentary    -- present-iff-meaningful (references, subcategory, ...): counted, NEVER flagged.
  review         -- actionable-but-not-fatal shapes (over-claims): listed, informational.

Usage:
  python tools/ceir_capability_matrix/gen_matrix.py            # regenerate the matrix
  python tools/ceir_capability_matrix/gen_matrix.py --check    # verify committed == generated
                                                               # AND that no HARD error fired
Exit codes: 0 ok; 1 drift (committed != generated); 2 a HARD manifest error; 3 usage/IO.
"""
import os
import re
import sys
import tomllib

MANIFEST = "docs/capabilities/gpu-platform-capabilities.toml"
OUT = "docs/generated/gpu-platform-capability-matrix.md"

# Vocabularies the manifest header pins (kept in sync with semantics.hpp enums).
DETERMINISM = {"", "BitExact", "DeterministicWithinTarget", "DeterministicWithinBackend",
               "Nondeterministic", "ExternalNondeterminism"}
PROVIDER_CLASSES = {"host", "gpu", "npu", "media", "external"}

# HARD: load-bearing fields -- a missing one is a manifest error.
LOAD_BEARING = ("category", "raf_level", "ceir_level", "providers", "determinism_tier",
                "classification", "status", "band", "assets", "tests_vulkan", "tests_dx12",
                "tests_cross_backend", "hot_reload_tests")
# Documentary: present-iff-meaningful (the seed's observable practice) -- counted, not flagged.
DOCUMENTARY = ("subcategory", "definition", "dependencies", "ckir_capabilities", "command_families",
               "executors", "runtime_systems", "backend_capabilities", "editor_support",
               "quality_perf_gates", "fallback", "references", "limitations")
# Fields whose entries may embed a repo-relative path we can existence-check (stale-ref).
PATH_FIELDS = ("assets", "tests_vulkan", "tests_dx12", "tests_cross_backend", "hot_reload_tests",
               "quality_perf_gates", "references")
# A path-shaped token: a repo dir + a file with an extension. `engine://` URIs (with "://") never match.
PATH_RE = re.compile(r"\b(?:assets|tests|docs|scripts|tools|engine)/[A-Za-z0-9_./-]+\.[A-Za-z0-9]+\b")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(root):
    with open(os.path.join(root, MANIFEST), "rb") as f:
        return tomllib.load(f)


def _is_level(v, lo, hi):
    return v == "n/a" or (isinstance(v, int) and lo <= v <= hi)


def _lvl_int(v):
    return v if isinstance(v, int) else -1  # "n/a" sorts as -1 for numeric predicates


def check(features, root):
    """Return (errors, reviews): HARD schema/invariant/stale-path errors, and SOFT review flags."""
    errors, reviews = [], []
    for fid, f in features.items():
        where = f"feature.{fid}"
        # -- HARD: load-bearing field presence --
        for k in LOAD_BEARING:
            if k not in f:
                errors.append(f"{where}: missing load-bearing field '{k}'")
        cl = f.get("ceir_level")
        rl = f.get("raf_level")
        prov = f.get("providers", [])
        det = f.get("determinism_tier", "")
        # -- HARD: level ranges + vocabularies --
        if not _is_level(cl, 0, 8):
            errors.append(f"{where}: ceir_level {cl!r} not in 0..8 or \"n/a\"")
        if not _is_level(rl, 0, 7):
            errors.append(f"{where}: raf_level {rl!r} not in 0..7 or \"n/a\"")
        if not isinstance(prov, list) or any(p not in PROVIDER_CLASSES for p in prov):
            errors.append(f"{where}: providers {prov!r} not a subset of {sorted(PROVIDER_CLASSES)}")
        if det not in DETERMINISM:
            errors.append(f"{where}: determinism_tier {det!r} not a DeterminismClass")
        # -- HARD: the providers invariant (ceir_level in {0,n/a} => providers == []) --
        if (cl == 0 or cl == "n/a") and prov != []:
            errors.append(f"{where}: ceir_level {cl!r} but providers {prov!r} (must be [] -- a "
                          "non-CEIR-program has no CEIR provider)")
        # -- HARD: path-shaped tokens in any PATH_FIELD must exist (stale-ref) --
        for k in PATH_FIELDS:
            for entry in f.get(k, []):
                if not isinstance(entry, str):
                    continue
                for p in PATH_RE.findall(entry):
                    if not os.path.exists(os.path.join(root, p)):
                        errors.append(f"{where}.{k}: path '{p}' does not exist (stale ref)")
        # -- REVIEW (actionable, not fatal) --
        tests = (f.get("tests_vulkan", []) + f.get("tests_dx12", [])
                 + f.get("tests_cross_backend", []))
        # a TARGET legitimately declares planned assets with no tests yet -- only a non-target is actionable
        if f.get("assets") and not tests and not f.get("hot_reload_tests") and f.get("status") != "target":
            reviews.append(f"{where}: assets present but no tests (assets-without-tests)")
        if _lvl_int(cl) >= 6 and not f.get("hot_reload_tests") and f.get("editor_support") in ("none", ""):
            reviews.append(f"{where}: ceir_level>=6 but no hot_reload_tests and editor_support="
                           f"{f.get('editor_support')!r} (L6+ w/o reload/editor)")
        if _lvl_int(cl) >= 3 and prov == []:
            reviews.append(f"{where}: ceir_level {cl} (execution rung) but providers=[] "
                           "(compile-time frontends are L2; review)")
        # a determinism CLAIM on a pre-execution rung is an over-claim shape
        if det != "" and 0 <= _lvl_int(cl) < 3:
            reviews.append(f"{where}: determinism_tier {det!r} on ceir_level {cl} "
                           "(a determinism claim with no execution rung -- over-claim shape?)")
    return errors, reviews


def _cell(v):
    if isinstance(v, list):
        return ", ".join(str(x) for x in v) if v else "-"
    return str(v) if v not in ("", None) else "-"


def _backends(f):
    b = []
    if f.get("tests_vulkan"):
        b.append("vk")
    if f.get("tests_dx12"):
        b.append("dx12")
    return "+".join(b) if b else "-"


def render(data, errors, reviews):
    features = data.get("feature", {})
    lines = []
    lines.append("# GPU-platform capability matrix (GENERATED -- do not edit)")
    lines.append("")
    lines.append("> Emitted by `tools/ceir_capability_matrix/gen_matrix.py` from "
                 "`docs/capabilities/gpu-platform-capabilities.toml` (CEIR-0g §4 step-4). "
                 "The manifest is the source of truth; this file is regenerated. "
                 "Run `python tools/ceir_capability_matrix/gen_matrix.py` to refresh.")
    lines.append("")
    lines.append(f"- schema: `{data.get('schema')}`  ·  audit_date: `{data.get('audit_date')}`  "
                 f"·  features: **{len(features)}**")
    ch = {}
    for f in features.values():
        ch[f.get("ceir_level")] = ch.get(f.get("ceir_level"), 0) + 1
    hist = " · ".join((f"L{k}×{ch[k]}" if k != "n/a" else f"n/a×{ch[k]}")
                      for k in sorted(ch, key=lambda x: (x == "n/a", x)))
    lines.append(f"- ceir_level distribution: {hist}")
    # documentary field coverage (present-iff-meaningful; count, don't flag)
    cov = {k: sum(1 for f in features.values() if f.get(k)) for k in DOCUMENTARY}
    lines.append("- documentary field coverage (present-iff-meaningful): "
                 + " · ".join(f"{k} {cov[k]}/{len(features)}" for k in DOCUMENTARY))
    lines.append("")
    lines.append("## Health checks")
    lines.append("")
    lines.append(f"- HARD errors: **{len(errors)}**  ·  review flags: **{len(reviews)}**")
    if errors:
        lines.append("")
        lines.append("### HARD errors (a future CI gate fails on these)")
        for e in errors:
            lines.append(f"- {e}")
    if reviews:
        lines.append("")
        lines.append("### Review flags (informational; may be honest defaults)")
        for r in reviews:
            lines.append(f"- {r}")
    lines.append("")
    lines.append("## Matrix")
    lines.append("")
    cols = ["id", "category", "class", "raf", "ceir", "providers", "determinism",
            "backends", "assets", "status"]
    lines.append("| " + " | ".join(cols) + " |")
    lines.append("|" + "|".join(["---"] * len(cols)) + "|")
    for fid in sorted(features):
        f = features[fid]
        row = [
            f"`{fid}`",
            _cell(f.get("category")),
            _cell(f.get("classification")),
            _cell(f.get("raf_level")),
            _cell(f.get("ceir_level")),
            _cell(f.get("providers")),
            _cell(f.get("determinism_tier")),
            _backends(f),
            str(len(f.get("assets", []))),
            _cell(f.get("status")),
        ]
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")
    return "\n".join(lines) + "\n"


def main(argv):
    check_only = "--check" in argv
    root = repo_root()
    try:
        data = load(root)
    except (OSError, tomllib.TOMLDecodeError) as e:
        sys.stderr.write(f"gen_matrix: cannot read manifest: {e}\n")
        return 3
    features = data.get("feature", {})
    errors, reviews = check(features, root)
    text = render(data, errors, reviews)
    out_path = os.path.join(root, OUT)
    if check_only:
        try:
            with open(out_path, "r", encoding="utf-8", newline="") as f:
                committed = f.read().replace("\r\n", "\n")
        except OSError:
            committed = None
        if committed != text:
            sys.stderr.write(f"gen_matrix: DRIFT -- {OUT} is stale; run gen_matrix.py to refresh\n")
            return 1
        if errors:
            sys.stderr.write(f"gen_matrix: {len(errors)} HARD manifest error(s) -- see the matrix\n")
            return 2
        return 0
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    sys.stderr.write(f"gen_matrix: wrote {OUT} ({len(features)} features, {len(errors)} hard "
                     f"errors, {len(reviews)} reviews)\n")
    return 2 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
