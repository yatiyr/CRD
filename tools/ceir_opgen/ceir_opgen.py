#!/usr/bin/env python3
"""ceir_opgen.py - the CEIR table-driven op-definition generator (CEIR-2, section 8; ADR-0110 section 2.1).

Reads engine/ceir/ops/<dialect>.ceirop.toml (schema in engine/ceir/ops/README.md) and emits COMMITTED C++ into
engine/ceir/generated/crd/ceir/gen/<dialect>_ops.{hpp,cpp}: op-kind interners, typed wrappers, builders, verifier
scaffolds, and a register_<dialect>_ops(Context&) that self-registers through the CEIR-1d Dialect::register_op path
(no central enum/switch - section 7 open-world). Output is DETERMINISTIC (ops sorted by name) so a regen is
byte-identical. No third-party dependencies (stdlib tomllib, Python >= 3.11).

Usage:
  python tools/ceir_opgen/ceir_opgen.py            # validate + (re)write every generated file
  python tools/ceir_opgen/ceir_opgen.py --check    # validate + byte-compare vs committed; nonzero on drift/error
"""
import json
import os
import sys
import tomllib

KNOWN_SCHEMA_VERSION = 1
OP_TRAITS = ("Terminator", "Symbol", "SymbolTable", "Pure", "IsolatedFromAbove", "StateEdge",
             "TokenProducer", "TokenConsumer")
ATTR_KINDS = {"int": "Int", "float": "Float", "bool": "Bool", "string": "String", "symbol": "SymbolRef", "type": "Type"}
REGION_KINDS = {"graph": "Graph", "ssacfg": "SsaCfg"}
# A region entry (CEIR-5a) may declare a SIGNATURE: `kind`, its entry-block `args` (a doc-only name+type_hint list — the
# COUNT is the structural contract the verifier enforces; real arg TYPES are construction-time, per the 3a "no TOML type
# grammar" lesson), and `variadic` (only the LAST region entry — a switch/match's N case regions).
REGION_FIELDS = {"kind", "args", "variadic"}
REGION_ARG_FIELDS = {"name", "type_hint"}
NATIVE_PROVIDERS = {"host", "gpu", "npu", "media", "external"}
# ADR-0098 determinism tiers (§27), in §27 ORDER — the fixed vocabulary BOTH the op-level `determinism` field and the
# ADR-0110 §2.1 [op.native] `determinism` field draw from. ⛔ tuple index maps to DeterminismClass ordinal (index+1, since
# Unspecified=0 is the un-declarable default); keep in lockstep with engine/ceir/include/crd/ceir/semantics.hpp (a
# static_assert pins the C++ count). ⛔ verbatim §27 names — `ExternalNondeterminism`, NOT the old short `External`.
DETERMINISM_TIERS = ("BitExact", "DeterministicWithinTarget", "DeterministicWithinBackend", "Nondeterministic",
                     "ExternalNondeterminism")
# Strength rank for the op-vs-native consistency check (higher = stronger; Unspecified/absent = 0). External is a
# narrower source of nondeterminism than plain Nondeterministic, so it ranks just below it.
DETERMINISM_RANK = {"BitExact": 5, "DeterministicWithinTarget": 4, "DeterministicWithinBackend": 3,
                    "Nondeterministic": 2, "ExternalNondeterminism": 1}
# §15 evaluation domains, in §15 ORDER — the op-level `domain` field's vocabulary; tuple index maps to EvalDomain ordinal
# (index+1, Unspecified=0 is the un-declarable default). ⛔ keep in lockstep with semantics.hpp (a static_assert pins it).
EVAL_DOMAINS = ("CompileTime", "CookTime", "LoadTime", "HostFrameTime", "HostSimulationTime", "HostAudioTime",
                "DeviceTime", "OfflineTime", "DistributedTime", "EitherHostOrDevice")
DEPRECATION_FIELDS = {"since", "replaced_by", "note"}
# §26 core effect families, in §26 ORDER — the tuple index IS the EffectFamily enum value the generator emits (CEIR-4a);
# ⛔ APPEND AT END, and keep in lockstep with engine/ceir/include/crd/ceir/effect.hpp (NO subsetting — the NO-FOLLOW
# mandate keeps MemoryReadWrite a family even though it reads as composable).
# ⛔ LOCKSTEP with engine/ceir/include/crd/ceir/effect.hpp::EffectFamily (identical ORDER — ordinals are the vocabulary;
# the effect.hpp static_assert + the drift/validator enforce it). CEIR-8c (ADR-0113) appended the 8 U-§19 families.
EFFECT_FAMILIES = ("MemoryRead", "MemoryWrite", "MemoryReadWrite", "Allocate", "Deallocate", "ResourceResidency",
                   "GPUCommand", "HostStateRead", "HostStateWrite", "SceneRead", "SceneWrite", "EcsRead", "EcsWrite",
                   "PhysicsRead", "PhysicsWrite", "AudioRead", "AudioWrite", "FileIO", "NetworkIO", "DeviceIO",
                   "ExternalCall", "TimeRead", "RandomRead", "Nondeterministic", "Synchronization", "Logging", "Debug",
                   "DocumentRead", "DocumentWrite", "ConstraintRead", "ConstraintWrite", "TransactionBoundary",
                   "UIRead", "UIWrite", "AgentAction")
# CEIR-3c ViewRange bits — an effect's optional range narrowing reuses this exact vocabulary (0 = whole resource).
VIEW_RANGES = {"byte": 1, "element": 2, "mip": 4, "layer": 8, "aspect": 16}
EFFECT_FIELDS = {"family", "operand", "result", "range"}

TOP_FIELDS = {"schema_version", "dialect", "summary", "op"}
OP_FIELDS = {"name", "summary", "version", "operands", "results", "attributes", "regions", "traits", "docs",
             "deprecation", "effects", "determinism", "domain", "type_inference", "shape_inference", "fold", "native",
             "kernel_ref"}
OPERAND_FIELDS = {"name", "type", "variadic", "doc"}
RESULT_FIELDS = {"name", "type", "doc", "variadic"}
ATTR_FIELDS = {"name", "kind", "required", "doc"}
NATIVE_FIELDS = {"provider", "determinism", "thread_safe", "hot_reload_safe", "lifetime", "cost", "capabilities"}
# CEIR-13c §85 [op.kernel_ref]: names the attrs a KERNEL-REFERENCING op (compute.dispatch) carries — `symbol` (the kernel
# asset identity, a symbol attr; required) + `interface` (the expected §107 interface hash, an int attr PIN; optional). The
# generator VALIDATES the named attrs EXIST on the op with the right KINDS (the declared-words-validated discipline), then
# emits them into OpSpec conditionally (the [op.native] precedent — non-kernel dialects regen byte-identical, drift-safe).
KERNEL_REF_FIELDS = {"symbol", "interface"}

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OPS_DIR = os.path.join(REPO_ROOT, "engine", "ceir", "ops")
GEN_DIR = os.path.join(REPO_ROOT, "engine", "ceir", "generated", "crd", "ceir", "gen")
# Generated smoke tests live under the TEST tree (not GEN_DIR) — GEN_DIR is globbed into the crd-ceir LIBRARY, so a
# test translation unit there would wrongly compile into the engine. This dir is globbed into crd-ceir-tests instead.
GEN_TEST_DIR = os.path.join(REPO_ROOT, "tests", "ceir", "generated")


class SchemaError(Exception):
    pass


def _rel(path):
    return os.path.relpath(path, REPO_ROOT).replace("\\", "/")


def _line_of(raw, needle):
    """1-based line number of the first occurrence of `needle` in `raw` (best-effort diagnostic pointing)."""
    idx = raw.find(needle)
    return raw.count("\n", 0, idx) + 1 if idx >= 0 else 1


def _err(path, raw, needle, msg):
    raise SchemaError("%s:%d: error: %s" % (_rel(path), _line_of(raw, needle), msg))


def _is_ident(s):
    return isinstance(s, str) and len(s) > 0 and (s[0].isalpha() or s[0] == "_") and all(c.isalnum() or c == "_" for c in s)


# ---------------------------------------------------------------- validation ----
def validate(path, raw, data):
    """Validate one dialect TOML -> a normalized model dict. Raises SchemaError (file:line: error:) on any violation."""
    for key in data:
        if key not in TOP_FIELDS:
            _err(path, raw, key, "unknown top-level field '%s'" % key)
    if data.get("schema_version") != KNOWN_SCHEMA_VERSION:
        _err(path, raw, "schema_version", "schema_version must be %d (got %r)" % (KNOWN_SCHEMA_VERSION, data.get("schema_version")))
    dialect = data.get("dialect")
    if not _is_ident(dialect):
        _err(path, raw, "dialect", "dialect must be a non-empty identifier")
    if "summary" not in data or not isinstance(data["summary"], str):
        _err(path, raw, "dialect", "the dialect needs a top-level string 'summary'")
    ops_raw = data.get("op", [])
    if not isinstance(ops_raw, list) or len(ops_raw) == 0:
        _err(path, raw, "dialect", "a dialect must define at least one [[op]]")

    seen = set()
    ops = []
    for op in ops_raw:
        name = op.get("name")
        anchor = ('name = "%s"' % name) if isinstance(name, str) else "[[op]]"
        for key in op:
            if key not in OP_FIELDS:
                _err(path, raw, key, "op '%s': unknown field '%s'" % (name, key))
        if not _is_ident(name):
            _err(path, raw, anchor, "op name must be an identifier")
        if name in seen:
            _err(path, raw, anchor, "duplicate op name '%s'" % name)
        seen.add(name)
        if "summary" not in op or not isinstance(op["summary"], str):
            _err(path, raw, anchor, "op '%s' needs a string 'summary'" % name)
        if not isinstance(op.get("version"), int):
            _err(path, raw, anchor, "op '%s' needs an integer 'version'" % name)

        accessor_names = set()
        operands = op.get("operands", [])
        for i, o in enumerate(operands):
            for key in o:
                if key not in OPERAND_FIELDS:
                    _err(path, raw, anchor, "op '%s' operand: unknown field '%s'" % (name, key))
            if not _is_ident(o.get("name")):
                _err(path, raw, anchor, "op '%s': each operand needs an identifier 'name'" % name)
            if o["name"] in accessor_names:
                _err(path, raw, anchor, "op '%s': duplicate member name '%s'" % (name, o["name"]))
            accessor_names.add(o["name"])
            if o.get("variadic", False) and i != len(operands) - 1:
                _err(path, raw, anchor, "op '%s': only the LAST operand may be variadic" % name)
        results = op.get("results", [])
        for i, r in enumerate(results):
            for key in r:
                if key not in RESULT_FIELDS:
                    _err(path, raw, anchor, "op '%s' result: unknown field '%s'" % (name, key))
            if not _is_ident(r.get("name")):
                _err(path, raw, anchor, "op '%s': each result needs an identifier 'name'" % name)
            if r.get("variadic", False) and i != len(results) - 1:
                _err(path, raw, anchor, "op '%s': only the LAST result may be variadic" % name)
            if r["name"] in accessor_names:
                _err(path, raw, anchor, "op '%s': duplicate member name '%s'" % (name, r["name"]))
            accessor_names.add(r["name"])
        attrs = op.get("attributes", [])
        for a in attrs:
            for key in a:
                if key not in ATTR_FIELDS:
                    _err(path, raw, anchor, "op '%s' attribute: unknown field '%s'" % (name, key))
            if not _is_ident(a.get("name")):
                _err(path, raw, anchor, "op '%s': each attribute needs an identifier 'name'" % name)
            if a.get("kind") not in ATTR_KINDS:
                _err(path, raw, anchor, "op '%s' attribute '%s': kind must be one of %s" % (name, a.get("name"), sorted(ATTR_KINDS)))
            if a["name"] in accessor_names:
                _err(path, raw, anchor, "op '%s': duplicate member name '%s'" % (name, a["name"]))
            accessor_names.add(a["name"])
        for t in op.get("traits", []):
            if t not in OP_TRAITS:
                _err(path, raw, anchor, "op '%s': unknown trait '%s' (known: %s)" % (name, t, ", ".join(OP_TRAITS)))
        regions = op.get("regions", 0)
        region_sig = []  # per-region {kind (C++ enumerator), argc, variadic}; empty for the plain int form
        if isinstance(regions, list):
            for idx, rg in enumerate(regions):
                if not isinstance(rg, dict):
                    _err(path, raw, anchor, "op '%s': each region must be a table {kind, args, variadic}" % name)
                for key in rg:
                    if key not in REGION_FIELDS:
                        _err(path, raw, anchor, "op '%s' region: unknown field '%s'" % (name, key))
                kind = rg.get("kind", "graph")
                if kind not in REGION_KINDS:
                    _err(path, raw, anchor, "op '%s': region kind must be graph|ssacfg" % name)
                rargs = rg.get("args", [])
                if not isinstance(rargs, list):
                    _err(path, raw, anchor, "op '%s': region 'args' must be an array" % name)
                for a in rargs:
                    for k in a:
                        if k not in REGION_ARG_FIELDS:
                            _err(path, raw, anchor, "op '%s' region arg: unknown field '%s'" % (name, k))
                    if not _is_ident(a.get("name")):
                        _err(path, raw, anchor, "op '%s': each region arg needs an identifier 'name'" % name)
                    if "type_hint" in a and not isinstance(a["type_hint"], str):
                        _err(path, raw, anchor, "op '%s' region arg '%s': type_hint must be a string" % (name, a.get("name")))
                variadic = rg.get("variadic", False)
                if not isinstance(variadic, bool):
                    _err(path, raw, anchor, "op '%s': region 'variadic' must be a bool" % name)
                if variadic and idx != len(regions) - 1:
                    _err(path, raw, anchor, "op '%s': only the LAST region may be variadic" % name)
                region_sig.append({"kind": REGION_KINDS[kind], "argc": len(rargs), "variadic": variadic,
                                   "arg_names": [a["name"] for a in rargs]})
            num_regions = len(regions)
        elif isinstance(regions, int) and regions >= 0:
            num_regions = regions
        else:
            _err(path, raw, anchor, "op '%s': regions must be an int or a list" % name)
        # shape / vocabulary of the remaining schema fields - the validator KNOWS every field (section 8), not just its
        # key. (Codegen for the semantic-heavy ones is scaffolded until CEIR-3/4; the SCHEMA contract is validated now.)
        if "docs" in op and not isinstance(op["docs"], str):
            _err(path, raw, anchor, "op '%s': 'docs' must be a string" % name)
        for f in ("type_inference", "shape_inference", "fold"):
            if f in op and not isinstance(op[f], str):
                _err(path, raw, anchor, "op '%s': '%s' must be a string" % (name, f))
        # §15 op-level evaluation domain (CEIR-4c): an optional §15 domain string (absent ⇒ Unspecified/no affinity).
        op_dom = op.get("domain")
        if op_dom is not None and (not isinstance(op_dom, str) or op_dom not in EVAL_DOMAINS):
            _err(path, raw, anchor, "op '%s': domain must be one of %s" % (name, ", ".join(EVAL_DOMAINS)))
        # §26 effects (CEIR-4a): each entry is either a bare family STRING ("MemoryWrite") or a TABLE carrying resource/
        # range identity ({family = "MemoryWrite", operand = 0, range = ["element"]}). Validated against the typed §26
        # vocabulary + this op's own operand/result counts, and normalized into {family, target, index, range_*}.
        parsed_effects = []
        if "effects" in op:
            if not isinstance(op["effects"], list):
                _err(path, raw, anchor, "op '%s': 'effects' must be an array" % name)
            for e in op["effects"]:
                rng_names = []
                if isinstance(e, str):
                    fam, tgt, idx = e, "None", 0
                elif isinstance(e, dict):
                    for key in e:
                        if key not in EFFECT_FIELDS:
                            _err(path, raw, anchor, "op '%s' effect: unknown field '%s' (known: %s)"
                                 % (name, key, ", ".join(sorted(EFFECT_FIELDS))))
                    if "family" not in e:
                        _err(path, raw, anchor, "op '%s': an effect table needs a 'family'" % name)
                    fam = e["family"]
                    if "operand" in e and "result" in e:
                        _err(path, raw, anchor, "op '%s' effect '%s': targets an operand OR a result, not both" % (name, fam))
                    if "operand" in e:
                        tgt, idx = "Operand", e["operand"]
                        if not isinstance(idx, int) or isinstance(idx, bool) or idx < 0 or idx >= len(operands):
                            _err(path, raw, anchor, "op '%s' effect '%s': operand index %r out of range (op has %d operands)"
                                 % (name, fam, idx, len(operands)))
                    elif "result" in e:
                        tgt, idx = "Result", e["result"]
                        if not isinstance(idx, int) or isinstance(idx, bool) or idx < 0 or idx >= len(results):
                            _err(path, raw, anchor, "op '%s' effect '%s': result index %r out of range (op has %d results)"
                                 % (name, fam, idx, len(results)))
                    else:
                        tgt, idx = "None", 0
                    rng = e.get("range", [])
                    if not isinstance(rng, list):
                        _err(path, raw, anchor, "op '%s' effect '%s': 'range' must be an array of range names" % (name, fam))
                    for rn in rng:
                        if rn not in VIEW_RANGES:
                            _err(path, raw, anchor, "op '%s' effect '%s': unknown range '%s' (known: %s)"
                                 % (name, fam, rn, ", ".join(sorted(VIEW_RANGES))))
                        rng_names.append(rn)
                else:
                    _err(path, raw, anchor, "op '%s': each effect is a family string or a {family, operand/result, range} table" % name)
                if fam not in EFFECT_FAMILIES:
                    _err(path, raw, anchor, "op '%s': unknown effect family '%s' (known: %s)" % (name, fam, ", ".join(EFFECT_FAMILIES)))
                mask = 0
                for rn in rng_names:
                    mask |= VIEW_RANGES[rn]
                parsed_effects.append({"family": fam, "target": tgt, "index": idx, "range_mask": mask, "range_names": rng_names})
        # ⛔ Pure ⇒ zero effects (§26): a Pure op is CSE/DCE-safe, which ANY declared effect disqualifies (the register_op
        # arm asserts the same — the two live-arm rule).
        if parsed_effects and "Pure" in op.get("traits", []):
            _err(path, raw, anchor, "op '%s': an op with the Pure trait must declare zero effects" % name)
        # §27 op-level determinism class (CEIR-4b): an optional tier string (absent ⇒ Unspecified/no claim).
        op_det = op.get("determinism")
        if op_det is not None:
            if not isinstance(op_det, str) or op_det not in DETERMINISM_TIERS:
                _err(path, raw, anchor, "op '%s': determinism must be one of %s" % (name, ", ".join(DETERMINISM_TIERS)))
        dep = op.get("deprecation")
        if dep is not None:
            if not isinstance(dep, dict):
                _err(path, raw, anchor, "op '%s': 'deprecation' must be a table {since, replaced_by, note}" % name)
            for key in dep:
                if key not in DEPRECATION_FIELDS:
                    _err(path, raw, anchor, "op '%s' deprecation: unknown field '%s'" % (name, key))
                if not isinstance(dep[key], str):
                    _err(path, raw, anchor, "op '%s' deprecation.%s must be a string" % (name, key))

        native = op.get("native")
        if native is not None:
            for key in native:
                if key not in NATIVE_FIELDS:
                    _err(path, raw, anchor, "op '%s' [op.native]: unknown field '%s'" % (name, key))
            if "provider" not in native:
                _err(path, raw, anchor, "op '%s' [op.native]: 'provider' is required" % name)
            if native.get("provider") not in NATIVE_PROVIDERS:
                _err(path, raw, anchor, "op '%s' [op.native]: provider must be one of %s" % (name, sorted(NATIVE_PROVIDERS)))
            if native.get("determinism") not in DETERMINISM_TIERS:
                _err(path, raw, anchor, "op '%s' [op.native]: determinism (required) must be one of %s" % (name, sorted(DETERMINISM_TIERS)))
            for f in ("thread_safe", "hot_reload_safe"):
                if f in native and not isinstance(native[f], bool):
                    _err(path, raw, anchor, "op '%s' [op.native]: '%s' must be a bool" % (name, f))
            for f in ("lifetime", "cost"):
                if f in native and not isinstance(native[f], str):
                    _err(path, raw, anchor, "op '%s' [op.native]: '%s' must be a string" % (name, f))
            if "capabilities" in native and not isinstance(native["capabilities"], list):
                _err(path, raw, anchor, "op '%s' [op.native]: 'capabilities' must be an array" % name)
            # ⛔ CEIR-8f (ADR-0116): a capability NAME must be a non-empty string (an empty name hashes to the FNV offset
            # basis — a phantom id; register_op asserts the same, the declared-words-validated parallel).
            for c in native.get("capabilities", []):
                if not isinstance(c, str) or not c:
                    _err(path, raw, anchor, "op '%s' [op.native]: each capability must be a non-empty string" % name)
            # ⛔ consistency (validate-at-cook-time): when BOTH are declared, the provider's determinism claim must be
            # AT-LEAST-AS-STRONG as the op's abstract class — a native impl cannot be less reproducible than the contract.
            nd = native.get("determinism")
            if op_det is not None and nd in DETERMINISM_RANK and DETERMINISM_RANK[nd] < DETERMINISM_RANK.get(op_det, 0):
                _err(path, raw, anchor, "op '%s': [op.native] determinism '%s' is weaker than the op's declared class '%s'"
                     % (name, nd, op_det))

        # CEIR-13c §85 [op.kernel_ref]: validate the named attrs EXIST on this op with the right KINDS (declared-words-
        # validated). `symbol` (required) names a `symbol`-kind attr (the kernel identity); `interface` (optional) names an
        # `int`-kind attr (the expected interface-hash pin). Emitted into OpSpec conditionally (the [op.native] precedent).
        kref = op.get("kernel_ref")
        if kref is not None:
            if not isinstance(kref, dict):
                _err(path, raw, anchor, "op '%s': 'kernel_ref' must be a table {symbol, interface}" % name)
            for key in kref:
                if key not in KERNEL_REF_FIELDS:
                    _err(path, raw, anchor, "op '%s' [op.kernel_ref]: unknown field '%s' (known: %s)"
                         % (name, key, ", ".join(sorted(KERNEL_REF_FIELDS))))
            attr_kind = {a["name"]: a["kind"] for a in attrs}
            sym = kref.get("symbol")
            if not isinstance(sym, str) or sym not in attr_kind:
                _err(path, raw, anchor, "op '%s' [op.kernel_ref]: 'symbol' must name an attribute of this op" % name)
            if attr_kind[sym] != "symbol":
                _err(path, raw, anchor, "op '%s' [op.kernel_ref]: symbol attr '%s' must be kind 'symbol' (is '%s')"
                     % (name, sym, attr_kind[sym]))
            iface = kref.get("interface")
            if iface is not None:
                if not isinstance(iface, str) or iface not in attr_kind:
                    _err(path, raw, anchor, "op '%s' [op.kernel_ref]: 'interface' must name an attribute of this op" % name)
                if attr_kind[iface] != "int":
                    _err(path, raw, anchor, "op '%s' [op.kernel_ref]: interface attr '%s' must be kind 'int' (is '%s')"
                         % (name, iface, attr_kind[iface]))

        ops.append({"name": name, "summary": op["summary"], "version": op["version"], "operands": operands,
                    "results": results, "attrs": attrs, "traits": list(op.get("traits", [])),
                    "num_regions": num_regions, "region_sig": region_sig, "docs": op.get("docs", ""), "effects": parsed_effects,
                    "determinism": op.get("determinism", ""), "domain": op.get("domain", ""), "native": native,
                    "kernel_ref": op.get("kernel_ref"),
                    "deprecation": dep, "type_inference": op.get("type_inference", ""),
                    "shape_inference": op.get("shape_inference", ""), "fold": op.get("fold", "")})

    ops.sort(key=lambda o: o["name"])  # deterministic order (byte-identical regen)
    return {"dialect": dialect, "summary": data["summary"], "ops": ops, "toml": _rel(path)}


# ---------------------------------------------------------------- codegen ----
def _camel(name):
    return "".join(part.capitalize() for part in name.split("_"))


def _cstr(s):
    """A C++ narrow-string literal (UTF-8 body kept as-is; /utf-8 is on)."""
    body = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return '"' + body + '"'


def _traits_expr(traits):
    if not traits:
        return "0U"
    if len(traits) == 1:
        return "flags_of(OpTrait::%s)" % traits[0]
    return " | ".join("OpTrait::%s" % t for t in traits)


def _header(model, ext):
    return ("// GENERATED by ceir_opgen.py from %s - DO NOT EDIT (schema v%d). Regenerate with\n"
            "// `python tools/ceir_opgen/ceir_opgen.py`; the crd-ceir-opgen-drift ctest fails on drift.\n"
            "//\n// %s - %s\n" % (model["toml"], KNOWN_SCHEMA_VERSION, model["dialect"], model["summary"]))


def emit_hpp(model):
    d = model["dialect"]
    out = ["#pragma once\n\n", _header(model, "hpp"), "\n",
           "#include <crd/ceir/attr.hpp>\n#include <crd/ceir/context.hpp>\n#include <crd/ceir/dialect.hpp>\n"
           "#include <crd/ceir/ir.hpp>\n#include <crd/ceir/op_schema.hpp>\n#include <crd/containers/span.hpp>\n\n",
           "namespace crd::ceir::%s\n{\n" % d]
    out.append("// -- op-kind identities (interned lazily against `ctx`, idempotent) --\n")
    for op in model["ops"]:
        out.append('[[nodiscard]] inline OpId %s_kind(Context& ctx) { return ctx.intern_op("%s", "%s"); }\n'
                   % (op["name"], d, op["name"]))
    out.append("\n// -- typed op wrappers (a thin view over an Operation*; the registered verifier enforces shape) --\n")
    for op in model["ops"]:
        cls = _camel(op["name"]) + "Op"
        out.append("// %s.%s - %s\n" % (d, op["name"], op["summary"]))
        out.append("class %s\n{\npublic:\n" % cls)
        out.append("    explicit %s(Operation* op) noexcept : m_op(op) {}\n" % cls)
        out.append("    [[nodiscard]] Operation* operation() const noexcept { return m_op; }\n")
        for i, o in enumerate(op["operands"]):
            out.append("    [[nodiscard]] Value* %s() const noexcept { return m_op->operand(%dU); }\n" % (o["name"], i))
        for i, r in enumerate(op["results"]):
            out.append("    [[nodiscard]] Value* %s() const noexcept { return m_op->result(%dU); }\n" % (r["name"], i))
        for a in op["attrs"]:
            out.append('    [[nodiscard]] AttrId %s() const noexcept { return m_op->attr("%s"); }\n' % (a["name"], a["name"]))
        out.append("\nprivate:\n    Operation* m_op;\n};\n")
    out.append("\n// -- builders (through the ordinary Context factories - no privileged construction). NOTE: a builder\n"
               "// produces the MINIMUM arity on every variadic axis (operands / results / regions); build the full arity\n"
               "// (extra variadic operands, N result values, N case regions) directly with `Context::create_operation`. --\n")
    for op in model["ops"]:
        out.append("[[nodiscard]] Operation* %s;\n" % _builder_sig(op))
    out.append("\n// -- registration: self-registers the dialect + every op (traits + verifier), NO central edit (section 7) --\n")
    out.append("Dialect* register_%s_ops(Context& ctx);\n" % d)
    out.append("\n// -- reflection: the op schema as queryable data - the SAME truth the wrapper/builder/verifier emit\n"
               "// from (no parallel truth); the CR-D007 node editor + the committed %s.ops.json read it (section 122/161) --\n" % d)
    out.append("[[nodiscard]] containers::ConstSpan<OpSchema> %s_op_schemas() noexcept;\n" % d)
    out.append("} // namespace crd::ceir::%s\n" % d)
    return "".join(out)


def _builder_params(op, with_default=True):
    params = ["Context& ctx"]
    params += ["Value* %s" % o["name"] for o in op["operands"]]
    params += ["AttrId %s" % a["name"] for a in op["attrs"] if a.get("required", False)]
    # The default argument belongs ONLY on the declaration (hpp); repeating it on the definition is a redefinition error.
    params.append("TypeId result_type = {}" if with_default else "TypeId result_type")
    return params


def _builder_sig(op, with_default=True):
    return "build_%s(%s)" % (op["name"], ", ".join(_builder_params(op, with_default)))


def _effects_expr(op):
    """The `ConstSpan<EffectRecord>` argument for an op's effects — references the `k<Op>Effects` array (defined in an
    anonymous-namespace block BEFORE register_op, so it is visible to BOTH register_op and the OpSchema table below)."""
    if not op["effects"]:
        return "containers::ConstSpan<EffectRecord>{}"
    return "containers::ConstSpan<EffectRecord>(k%sEffects, %dU)" % (_camel(op["name"]), len(op["effects"]))


def _det_expr(tier):
    """A §27 tier string → the DeterminismClass enumerator (empty/absent → Unspecified, the no-claim default)."""
    return "DeterminismClass::%s" % (tier if tier else "Unspecified")


def _dom_expr(dom):
    """A §15 domain string → the EvalDomain enumerator (empty/absent → Unspecified, the no-affinity default)."""
    return "EvalDomain::%s" % (dom if dom else "Unspecified")


def emit_cpp(model):
    d = model["dialect"]
    out = [_header(model, "cpp"), "\n#include <crd/ceir/gen/%s_ops.hpp>\n\n" % d,
           "#include <crd/ceir/effect.hpp>\n#include <crd/ceir/semantics.hpp>\n#include <crd/containers/span.hpp>\n\n",
           "namespace crd::ceir::%s\n{\n" % d]
    out.append("namespace\n{\n")
    out.append("// verifiers - structural conformance (operand/result/region counts + required attrs & kinds), wired to\n")
    out.append("// Context::verify via register_op. Semantic verification (types/effects/domain) lands at CEIR-3/4.\n")
    for op in model["ops"]:
        req = [a for a in op["attrs"] if a.get("required", False)]
        ctx_used = len(req) > 0
        ctx_param = "const Context& ctx" if ctx_used else "const Context& /*ctx*/"
        out.append("[[nodiscard]] bool verify_%s(%s, const Operation& op) noexcept\n{\n" % (op["name"], ctx_param))
        variadic = op["operands"] and op["operands"][-1].get("variadic", False)
        n_ops = len(op["operands"])
        if variadic:
            min_ops = n_ops - 1  # every fixed operand before the trailing variadic one
            if min_ops > 0:
                out.append("    if (op.num_operands() < %dU) { return false; }\n" % min_ops)
            # else: a lone variadic operand admits ANY count (>= 0) — emit no check (a `< 0U` would be an always-false tautology)
        else:
            out.append("    if (op.num_operands() != %dU) { return false; }\n" % n_ops)
        res_variadic = op["results"] and op["results"][-1].get("variadic", False)
        n_res = len(op["results"])
        if res_variadic:
            if n_res - 1 > 0:
                out.append("    if (op.num_results() < %dU) { return false; }\n" % (n_res - 1)) # fixed results before the variadic
            # else: a lone variadic result admits any count (>= 0) — no check (a `< 0U` tautology)
        else:
            out.append("    if (op.num_results() != %dU) { return false; }\n" % n_res)
        sig = op["region_sig"]
        if not sig:
            out.append("    if (op.num_regions() != %dU) { return false; }\n" % op["num_regions"])
        else:
            reg_variadic = sig[-1]["variadic"]
            nfixed = len(sig) - 1 if reg_variadic else len(sig)
            if reg_variadic:
                out.append("    if (op.num_regions() < %dU) { return false; }\n" % len(sig)) # fixed + >=1 variadic region
            else:
                out.append("    if (op.num_regions() != %dU) { return false; }\n" % len(sig))
            # per-region ENTRY-BLOCK arg count — the structural half of the region signature (the arg TYPES are
            # construction-time). Checked only WHEN a block is present, so a freshly-built skeleton (empty regions) still
            # verifies; a populated entry block must match the declared arity (full block-existence is CEIR-5b's SSACFG rule).
            for i in range(nfixed):
                out.append("    if (op.region(%dU)->first_block() != nullptr && op.region(%dU)->first_block()->num_args() != %dU) { return false; }\n"
                           % (i, i, sig[i]["argc"]))
            if reg_variadic:
                out.append("    for (u32 ri = %dU; ri < op.num_regions(); ++ri) { const Block* const rb = op.region(ri)->first_block();\n" % nfixed)
                out.append("      if (rb != nullptr && rb->num_args() != %dU) { return false; } }\n" % sig[-1]["argc"])
        for a in req:
            out.append('    { const AttrId a = op.attr("%s");\n' % a["name"])
            out.append("      if (!a.valid() || ctx.attr_value(a).kind != AttrKind::%s) { return false; } }\n" % ATTR_KINDS[a["kind"]])
        out.append("    return true;\n}\n")
    out.append("} // namespace\n\n")
    for op in model["ops"]:
        out.append("Operation* %s\n{\n" % _builder_sig(op, with_default=False))
        n_ops = len(op["operands"])
        if n_ops > 0:
            out.append("    Value* operands[] = {%s};\n" % ", ".join(o["name"] for o in op["operands"]))
            span = "containers::ConstSpan<Value*>(operands, %dU)" % n_ops
        else:
            span = "{}"
        out.append("    Operation* const op = ctx.create_operation(%s_kind(ctx), %s, %dU, result_type, %dU);\n"
                   % (op["name"], span, len(op["results"]), op["num_regions"]))
        for a in op["attrs"]:
            if a.get("required", False):
                out.append('    ctx.set_attr(op, "%s", %s);\n' % (a["name"], a["name"]))
        out.append("    return op;\n}\n")
    # §26 effect tables (CEIR-4a) — emitted BEFORE register_%s_ops so the register call can reference them; an
    # anonymous-namespace member is visible across every anon-namespace block in this TU, so the OpSchema table reuses
    # the SAME arrays (no duplicate emission).
    def _op_caps(o):
        nat = o["native"]
        return nat.get("capabilities", []) if nat else []
    if any(op["effects"] or _op_caps(op) for op in model["ops"]):
        out.append("\nnamespace\n{\n")
        for op in model["ops"]:
            if op["effects"]:
                items = ", ".join("{EffectFamily::%s, EffectTarget::%s, %dU, %dU}"
                                  % (e["family"], e["target"], e["index"], e["range_mask"]) for e in op["effects"])
                out.append("constexpr EffectRecord k%sEffects[] = {%s};\n" % (_camel(op["name"]), items))
            # CEIR-8f (ADR-0116): the op's [op.native] required-capability NAMES as a constexpr StringView array; the
            # register call passes it to OpSpec.capabilities, which register_op interns to CapabilityIds.
            caps = _op_caps(op)
            if caps:
                out.append("constexpr containers::StringView k%sCaps[] = {%s};\n"
                           % (_camel(op["name"]), ", ".join(_cstr(c) for c in caps)))
        out.append("} // namespace\n")
    out.append("\nDialect* register_%s_ops(Context& ctx)\n{\n" % d)
    out.append('    Dialect* const d = ctx.register_dialect("%s");\n' % d)
    for op in model["ops"]:
        # ADR-0110 §2.1 native binding (CEIR-7a): emit .intrinsic/.native_provider ONLY when [op.native] is present, so a
        # non-native op's register call stays byte-identical (OpSpec defaults false/"" ) — drift-safe for every prior file.
        native = op["native"]
        if native:
            caps = native.get("capabilities", [])
            caps_kv = (", .capabilities = containers::ConstSpan<containers::StringView>(k%sCaps, %dU)"
                       % (_camel(op["name"]), len(caps))) if caps else ""
            native_kv = ", .intrinsic = true, .native_provider = %s%s" % (_cstr(native["provider"]), caps_kv)
        else:
            native_kv = ""
        # CEIR-13c §85: emit .kernel_ref_symbol/.kernel_ref_interface ONLY when [op.kernel_ref] is present (the [op.native]
        # drift-safe precedent — every prior op's register call stays byte-identical; OpSpec defaults to "").
        kref = op["kernel_ref"]
        if kref:
            iface = kref.get("interface")
            kref_kv = ", .kernel_ref_symbol = %s%s" % (
                _cstr(kref["symbol"]),
                (", .kernel_ref_interface = %s" % _cstr(iface)) if iface else "")
        else:
            kref_kv = ""
        out.append('    d->register_op("%s", {.traits = %s, .verify = &verify_%s, .effects = %s, .determinism = %s, .domain = %s%s%s});\n'
                   % (op["name"], _traits_expr(op["traits"]), op["name"], _effects_expr(op),
                      _det_expr(op["determinism"]), _dom_expr(op["domain"]), native_kv, kref_kv))
    out.append("    return d;\n}\n")

    # reflection - the OpSchema table (constexpr; StringView is std::string_view, so static-init-order immune)
    out.append("\nnamespace\n{\n")
    for op in model["ops"]:
        cap = _camel(op["name"])
        if op["operands"]:
            items = ", ".join("{%s, %s, %s}" % (_cstr(o["name"]), _cstr(o.get("doc", "")),
                                                "true" if o.get("variadic", False) else "false") for o in op["operands"])
            out.append("constexpr OperandInfo k%sOperands[] = {%s};\n" % (cap, items))
        if op["results"]:
            items = ", ".join("{%s, %s}" % (_cstr(r["name"]), _cstr(r.get("doc", ""))) for r in op["results"])
            out.append("constexpr ResultInfo k%sResults[] = {%s};\n" % (cap, items))
        if op["attrs"]:
            items = ", ".join("{%s, AttrKind::%s, %s, %s}" % (_cstr(a["name"]), ATTR_KINDS[a["kind"]],
                                                             "true" if a.get("required", False) else "false",
                                                             _cstr(a.get("doc", ""))) for a in op["attrs"])
            out.append("constexpr AttrInfo k%sAttrs[] = {%s};\n" % (cap, items))
        # (effect tables are emitted BEFORE register_%s_ops above — reused here, not re-emitted)
    out.append("\nconstexpr OpSchema kOpSchemas[] = {\n")
    for op in model["ops"]:
        cap = _camel(op["name"])
        operands = ("containers::ConstSpan<OperandInfo>(k%sOperands, %dU)" % (cap, len(op["operands"]))) if op["operands"] else "containers::ConstSpan<OperandInfo>{}"
        results = ("containers::ConstSpan<ResultInfo>(k%sResults, %dU)" % (cap, len(op["results"]))) if op["results"] else "containers::ConstSpan<ResultInfo>{}"
        attrs = ("containers::ConstSpan<AttrInfo>(k%sAttrs, %dU)" % (cap, len(op["attrs"]))) if op["attrs"] else "containers::ConstSpan<AttrInfo>{}"
        effects = _effects_expr(op)
        native = op["native"]
        provider = _cstr(native.get("provider", "")) if native else '""'
        det_op = _det_expr(op["determinism"])                                            # §27 op-level class (CEIR-4b)
        det_native = _det_expr(native.get("determinism", "") if native else "")            # provider claim (Unspecified if none)
        out.append("    {%s, %s, %s, %dU, %s, %s,\n     %s, %s, %s,\n     %s, %dU, %s, %s, %s, %s, %s, %s},\n" % (
            _cstr(op["name"]), _cstr("%s.%s" % (d, op["name"])), _cstr(d), op["version"], _cstr(op["summary"]),
            _cstr(op["docs"]), operands, results, attrs, _traits_expr(op["traits"]), op["num_regions"], effects,
            det_op, _dom_expr(op["domain"]), "true" if native else "false", provider, det_native))
    out.append("};\n} // namespace\n\n")
    out.append("containers::ConstSpan<OpSchema> %s_op_schemas() noexcept { return containers::ConstSpan<OpSchema>(kOpSchemas, %dU); }\n" % (d, len(model["ops"])))
    out.append("} // namespace crd::ceir::%s\n" % d)
    return "".join(out)


def _effect_json(e):
    """One §26 effect as a JSON object — family always present; target/index only when it carries operand/result
    identity; range only when narrowed. A bare-family effect renders as just {"family": ...}."""
    j = {"family": e["family"]}
    if e["target"] != "None":
        j["target"] = e["target"].lower()  # "operand" | "result"
        j["index"] = e["index"]
    if e["range_names"]:
        j["range"] = e["range_names"]
    return j


def _effect_md(e):
    """One §26 effect as a human string: `Family`, optionally `Family on operand N`, optionally `... [range,...]`."""
    s = e["family"]
    if e["target"] != "None":
        s += " on %s %d" % (e["target"].lower(), e["index"])
    if e["range_names"]:
        s += " [%s]" % ",".join(e["range_names"])
    return s


def emit_json(model):
    """The CLI/MCP agent-discovery schema (section 122) - DERIVED from the same model the C++ emitters consume, so it
    can never drift from the wrapper/verifier. Deterministic: fixed key order, ensure_ascii, trailing newline."""
    d = model["dialect"]
    ops = []
    for op in model["ops"]:
        native = None
        if op["native"] is not None:
            n = op["native"]
            native = {"provider": n.get("provider", ""), "determinism": n.get("determinism", ""),
                      "thread_safe": n.get("thread_safe"), "hot_reload_safe": n.get("hot_reload_safe"),
                      "lifetime": n.get("lifetime", ""), "cost": n.get("cost", ""),
                      "capabilities": list(n.get("capabilities", []))}
        entry = {
            "name": op["name"], "qualified": "%s.%s" % (d, op["name"]), "version": op["version"],
            "summary": op["summary"], "docs": op["docs"],
            "operands": [{"name": o["name"], "doc": o.get("doc", ""), "variadic": bool(o.get("variadic", False))}
                         for o in op["operands"]],
            "results": [{"name": r["name"], "doc": r.get("doc", "")} for r in op["results"]],
            "attributes": [{"name": a["name"], "kind": a["kind"], "required": bool(a.get("required", False)),
                            "doc": a.get("doc", "")} for a in op["attrs"]],
            "traits": op["traits"], "regions": op["num_regions"],
            "effects": [_effect_json(e) for e in op["effects"]], "determinism": op["determinism"],
            "domain": op["domain"], "native": native}
        # Optional declared fields ride the JSON only WHEN PRESENT (a key-per-declaration rule): a dialect that declares
        # none — like arith — emits byte-identical JSON, so 2c's committed artifact never churns when these emitters land.
        for f in ("type_inference", "shape_inference", "fold"):
            if op.get(f):
                entry[f] = op[f]
        if op.get("deprecation"):
            dep = op["deprecation"]
            entry["deprecation"] = {k: dep[k] for k in ("since", "replaced_by", "note") if k in dep}
        ops.append(entry)
    doc = {"schema_version": KNOWN_SCHEMA_VERSION, "dialect": d, "summary": model["summary"], "ops": ops}
    return json.dumps(doc, indent=2, ensure_ascii=True, sort_keys=False) + "\n"


# ---------------------------------------------------------------- markdown docs ----
def _fmt_scalar(v):
    """Render a TOML scalar for docs: a bool as lowercase `true`/`false` (not Python's `True`/`False`), else `str`."""
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def _md_cell(s):
    """A markdown TABLE-cell rendering of free text: pipes escaped, newlines flattened (a `|` in a doc string would
    otherwise silently break the table)."""
    return str(s).replace("\r", " ").replace("\n", " ").replace("|", "\\|").strip()


def _md_header(model):
    return ("<!-- GENERATED by ceir_opgen.py from %s - DO NOT EDIT (schema v%d). Regenerate with -->\n"
            "<!-- `python tools/ceir_opgen/ceir_opgen.py`; the crd-ceir-opgen-drift ctest fails on drift. -->\n"
            % (model["toml"], KNOWN_SCHEMA_VERSION))


def emit_md(model):
    """The human-readable per-dialect op reference (section 123) - DERIVED from the same model as the C++/JSON, so it
    can never drift. Deterministic (ops sorted by name); EVERY validated field renders (no field silently dropped)."""
    d = model["dialect"]
    out = [_md_header(model), "\n# `%s` dialect\n\n" % d, model["summary"].strip(), "\n\n"]
    n = len(model["ops"])
    out.append("**%d op%s:** %s\n" % (n, "" if n == 1 else "s", ", ".join("`%s`" % op["name"] for op in model["ops"])))
    for op in model["ops"]:
        dep = op.get("deprecation")
        out.append("\n## `%s.%s`%s\n\n" % (d, op["name"], "  — **DEPRECATED**" if dep else ""))
        out.append(op["summary"].strip() + "\n")
        if op["docs"]:
            out.append("\n" + op["docs"].strip() + "\n")
        out.append("\n- **Version:** %d\n" % op["version"])
        out.append("- **Traits:** %s\n" % (", ".join("`%s`" % t for t in op["traits"]) if op["traits"] else "_none_"))
        out.append("- **Regions:** %d\n" % op["num_regions"])
        if op["domain"]:
            out.append("- **Domain:** `%s`\n" % _md_cell(op["domain"]))
        if op["effects"]:
            out.append("- **Effects:** %s\n" % ", ".join("`%s`" % _md_cell(_effect_md(e)) for e in op["effects"]))
        if op["determinism"]:
            out.append("- **Determinism:** `%s`\n" % _md_cell(op["determinism"]))
        for label, key in (("Type inference", "type_inference"), ("Shape inference", "shape_inference"), ("Fold", "fold")):
            if op.get(key):
                out.append("- **%s:** `%s`\n" % (label, _md_cell(op[key])))
        if dep:
            parts = []
            if dep.get("since"):
                parts.append("since %s" % _md_cell(dep["since"]))
            if dep.get("replaced_by"):
                parts.append("replaced by `%s`" % _md_cell(dep["replaced_by"]))
            if dep.get("note"):
                parts.append(_md_cell(dep["note"]))
            out.append("- **Deprecated:** %s\n" % " — ".join(parts))
        native = op["native"]
        if native:
            bits = ["provider=`%s`" % _md_cell(native.get("provider", "")),
                    "determinism=`%s`" % _md_cell(native.get("determinism", ""))]
            for f in ("thread_safe", "hot_reload_safe", "lifetime", "cost"):
                if f in native:
                    bits.append("%s=`%s`" % (f, _md_cell(_fmt_scalar(native[f]))))
            if native.get("capabilities"):
                bits.append("capabilities=%s" % ", ".join("`%s`" % _md_cell(str(c)) for c in native["capabilities"]))
            out.append("- **Native binding:** %s\n" % ", ".join(bits))
        out.append("\n**Operands:**")
        if op["operands"]:
            out.append("\n\n| name | type | variadic | doc |\n| --- | --- | --- | --- |\n")
            for o in op["operands"]:
                out.append("| `%s` | %s | %s | %s |\n" % (o["name"], _md_cell(o.get("type", "")),
                           "yes" if o.get("variadic", False) else "no", _md_cell(o.get("doc", ""))))
        else:
            out.append(" _none_\n")
        out.append("\n**Results:**")
        if op["results"]:
            out.append("\n\n| name | type | doc |\n| --- | --- | --- |\n")
            for r in op["results"]:
                out.append("| `%s` | %s | %s |\n" % (r["name"], _md_cell(r.get("type", "")), _md_cell(r.get("doc", ""))))
        else:
            out.append(" _none_\n")
        out.append("\n**Attributes:**")
        if op["attrs"]:
            out.append("\n\n| name | kind | required | doc |\n| --- | --- | --- | --- |\n")
            for a in op["attrs"]:
                out.append("| `%s` | `%s` | %s | %s |\n" % (a["name"], a["kind"],
                           "yes" if a.get("required", False) else "no", _md_cell(a.get("doc", ""))))
        else:
            out.append(" _none_\n")
    return "".join(out)


# ---------------------------------------------------------------- generated smoke test ----
_ATTR_FACTORY = {"int": "ctx.attr_int(0)", "float": "ctx.attr_float(0.0)", "bool": "ctx.attr_bool(false)",
                 "string": 'ctx.attr_string("x")', "symbol": 'ctx.attr_symbol("x")', "type": "ctx.attr_type(ctx.type_i32())"}


def _smoke_header(model):
    d = model["dialect"]
    return ("// GENERATED by ceir_opgen.py from %s - DO NOT EDIT (schema v%d). Regenerate with\n"
            "// `python tools/ceir_opgen/ceir_opgen.py`; the crd-ceir-opgen-drift ctest fails on drift.\n"
            "//\n// CEIR-2d smoke gate for the `%s` dialect: every op self-registers, reflects a coherent schema, and\n"
            "// its generated builder yields a verifier-accepted op - proven WITHOUT a hand-written line per op (a new\n"
            "// op in the TOML grows this test on the next regen). Rich semantic tests live beside it (test_%s_gen.cpp).\n"
            % (model["toml"], KNOWN_SCHEMA_VERSION, d, d))


def _smoke_build_block(op, dialect):
    """One C++ block: build `op` via its generated builder (dummy operand values from an unregistered source op's
    results; required attrs by kind) and assert the registered verifier accepts the well-formed result."""
    n_ops = len(op["operands"])
    lines = ["    {\n"]
    args = []
    if n_ops > 0:
        lines.append('        Operation* const src = ctx.create_operation(ctx.intern_op("smoke", "src"), {}, %dU, ctx.type_i32());\n' % n_ops)
        args += ["src->result(%dU)" % i for i in range(n_ops)]
    args += [_ATTR_FACTORY[a["kind"]] for a in op["attrs"] if a.get("required", False)]
    args.append("ctx.type_i32()")
    lines.append("        Operation* const op = %s::build_%s(ctx, %s);\n" % (dialect, op["name"], ", ".join(args)))
    lines.append("        CHECK(op != nullptr);\n")
    lines.append("        CHECK(ctx.verify(*op));\n")
    lines.append("    }\n")
    return "".join(lines)


def _smoke_min_operands(op):
    n = len(op["operands"])
    return n - 1 if (n > 0 and op["operands"][-1].get("variadic", False)) else n


def _smoke_op_rejects_empty(op):
    """True iff an EMPTY construct (0 operands, 0 attrs, correct result count, 0 regions) is malformed for this op — the
    only ops for which a CHECK_FALSE is meaningful (an op with min-operands 0, no required attr and no region is
    well-formed when empty, so it CANNOT be negative-tested)."""
    return _smoke_min_operands(op) > 0 or any(a.get("required", False) for a in op["attrs"]) or op["num_regions"] > 0


def _smoke_negative_block(op, dialect):
    """One C++ block: create a MALFORMED `op` (no operands/attrs/regions, correct result count) and assert the generated
    verifier REJECTS it — the guard against a verifier that regresses to a vacuous `return true`."""
    return ("    {\n"
            "        Operation* const bad = ctx.create_operation(%s::%s_kind(ctx), {}, %dU, ctx.type_i32());\n"
            "        CHECK_FALSE(ctx.verify(*bad));\n"
            "    }\n" % (dialect, op["name"], len(op["results"])))


def emit_smoke(model):
    """A COMMITTED, drift-guarded Catch2 smoke test that GROWS with the dialect's TOML - the artifact CEIR-2z cites when
    it asserts a new op's test skeleton appears from a TOML edit alone. Compiled into crd-ceir-tests via the generated
    dir glob (no per-dialect CMake edit). Host-only; ASCII test names; anonymous namespace (internal linkage)."""
    d = model["dialect"]
    out = [_smoke_header(model), "\n",
           "#include <crd/ceir/ceir.hpp>\n#include <crd/ceir/gen/%s_ops.hpp>\n\n" % d,
           "#include <crd/memory/allocators/malloc_allocator.hpp>\n\n",
           "#include <catch2/catch_test_macros.hpp>\n\n",
           "namespace\n{\nusing namespace crd::ceir;\n\n"]
    out.append('TEST_CASE("ceir %s gen smoke: the dialect self-registers and reflects a coherent schema",\n'
               '          "[ceir][gen][smoke][%s]")\n{\n' % (d, d))
    out.append("    crd::memory::MallocAllocator root;\n    Context                      ctx(&root);\n")
    out.append("    Dialect* const               dlt = %s::register_%s_ops(ctx);\n" % (d, d))
    out.append("    REQUIRE(dlt != nullptr);\n")
    out.append('    CHECK(dlt->name() == crd::containers::StringView("%s"));\n\n' % d)
    out.append("    const crd::containers::ConstSpan<OpSchema> schemas = %s::%s_op_schemas();\n" % (d, d))
    out.append("    REQUIRE(schemas.size() == %dU);\n\n" % len(model["ops"]))
    out.append("    crd::containers::StringView prev;\n")
    out.append("    for (const OpSchema& s : schemas)\n    {\n")
    out.append("        if (!prev.empty()) { CHECK(prev < s.name); } // reflection is emitted sorted by name\n")
    out.append("        prev = s.name;\n")
    out.append("        const OpId reflected = ctx.intern_op(s.dialect, s.name);\n")
    out.append("        CHECK(ctx.intern_op(s.dialect, s.name) == reflected); // interning is idempotent\n")
    out.append("        CHECK(ctx.dialect_of(reflected) == dlt); // a reflected op is a registered kind of this dialect\n")
    out.append("        CHECK(s.qualified.size() == s.dialect.size() + 1U + s.name.size());\n")
    out.append("    }\n}\n\n")
    out.append('TEST_CASE("ceir %s gen smoke: every op builds through its generated builder and the verifier accepts it",\n'
               '          "[ceir][gen][smoke][%s]")\n{\n' % (d, d))
    out.append("    crd::memory::MallocAllocator root;\n    Context                      ctx(&root);\n")
    out.append("    (void)%s::register_%s_ops(ctx);\n\n" % (d, d))
    for i, op in enumerate(model["ops"]):
        if i > 0:
            out.append("\n")
        out.append("    // %s.%s\n" % (d, op["name"]))
        out.append(_smoke_build_block(op, d))
    out.append("}\n")
    # Negative direction: prove the verifier actually REJECTS malformed ops (else a vacuous `return true` would pass the
    # whole generated suite). Emitted only for ops an empty construct is malformed for; the whole case is omitted when
    # none qualify (an assertion-free TEST_CASE would pass vacuously — the very failure mode this guards against).
    rejects = [op for op in model["ops"] if _smoke_op_rejects_empty(op)]
    if rejects:
        out.append('\nTEST_CASE("ceir %s gen smoke: the generated verifier rejects a malformed construction",\n'
                   '          "[ceir][gen][smoke][%s]")\n{\n' % (d, d))
        out.append("    crd::memory::MallocAllocator root;\n    Context                      ctx(&root);\n")
        out.append("    (void)%s::register_%s_ops(ctx);\n\n" % (d, d))
        for i, op in enumerate(rejects):
            if i > 0:
                out.append("\n")
            out.append("    // %s.%s\n" % (d, op["name"]))
            out.append(_smoke_negative_block(op, d))
        out.append("}\n")
    out.append("} // namespace\n")
    return "".join(out)


# ---------------------------------------------------------------- driver ----
def load_model(path):
    with open(path, "rb") as f:
        data = tomllib.load(f)
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    return validate(path, raw, data)


def outputs(model):
    d = model["dialect"]
    return {os.path.join(GEN_DIR, d + "_ops.hpp"): emit_hpp(model),
            os.path.join(GEN_DIR, d + "_ops.cpp"): emit_cpp(model),
            os.path.join(GEN_DIR, d + ".ops.json"): emit_json(model),
            os.path.join(GEN_DIR, d + ".ops.md"): emit_md(model),
            os.path.join(GEN_TEST_DIR, "test_" + d + "_gen_smoke.cpp"): emit_smoke(model)}


def main(argv):
    check = "--check" in argv
    toml_paths = sorted(os.path.join(OPS_DIR, f) for f in os.listdir(OPS_DIR) if f.endswith(".ceirop.toml"))
    if not toml_paths:
        sys.stderr.write("error: no *.ceirop.toml under %s\n" % _rel(OPS_DIR))
        return 1
    drift = False
    try:
        for path in toml_paths:
            model = load_model(path)
            for out_path, text in outputs(model).items():
                os.makedirs(os.path.dirname(out_path), exist_ok=True)  # GEN_DIR and GEN_TEST_DIR both created on demand
                if check:
                    existing = None
                    if os.path.exists(out_path):
                        with open(out_path, "r", encoding="utf-8", newline="") as f:
                            # normalize CRLF so a git/editor line-ending rewrite never fires drift on a byte-invisible diff
                            existing = f.read().replace("\r\n", "\n")
                    if existing != text:
                        sys.stderr.write("error: %s is out of date - run ceir_opgen.py\n" % _rel(out_path))
                        drift = True
                else:
                    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
                        f.write(text)
    except SchemaError as e:
        sys.stderr.write(str(e) + "\n")
        return 1
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
