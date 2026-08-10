#!/usr/bin/env python3
"""test_opgen.py - unit tests for the ceir_opgen validator (CEIR-2a). Every malformed *.ceirop.toml must be rejected
with a `file:line: error: message` diagnostic (the declared-contract rule). Wired as the `crd-ceir-opgen-validator`
ctest. Run: python tools/ceir_opgen/test_opgen.py  (stdlib unittest, no third-party deps)."""
import os
import re
import sys
import tomllib
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ceir_opgen as og  # noqa: E402

GOOD = """schema_version = 1
dialect = "t"
summary = "the t dialect"

[[op]]
name = "a"
summary = "op a"
version = 1
"""


def run(raw):
    """Validate `raw`; return (error_string_or_None). tomllib syntax errors propagate (a separate failure class)."""
    data = tomllib.loads(raw)
    try:
        og.validate("engine/ceir/ops/mem.ceirop.toml", raw, data)
        return None
    except og.SchemaError as e:
        return str(e)


class ValidatorTests(unittest.TestCase):
    def _assert_error(self, raw, needle, line=None):
        msg = run(raw)
        self.assertIsNotNone(msg, "expected a SchemaError, got none")
        self.assertIn(": error: ", msg)
        self.assertIn(needle, msg)
        if line is not None:
            m = re.search(r":(\d+): error:", msg)
            self.assertIsNotNone(m, "diagnostic has no line number: %s" % msg)
            self.assertEqual(int(m.group(1)), line, "wrong line in: %s" % msg)

    def test_good_schema_validates(self):
        self.assertIsNone(run(GOOD))
        model = og.validate("engine/ceir/ops/mem.ceirop.toml", GOOD, tomllib.loads(GOOD))
        self.assertEqual(model["dialect"], "t")
        self.assertEqual(model["ops"][0]["name"], "a")

    def test_native_binding_promotes_intrinsic_fields_to_register_op(self):
        # CEIR-7a: an [op.native] op must emit .intrinsic/.native_provider into the register_op OpSpec (the promotion the
        # sec 106 dep collector reads at runtime); a NON-native op's register call must stay byte-identical (no fields).
        raw = ('schema_version = 1\ndialect = "t"\nsummary = "s"\n\n'
               '[[op]]\nname = "plain"\nsummary = "s"\nversion = 1\n\n'
               '[[op]]\nname = "nat"\nsummary = "s"\nversion = 1\n'
               'native = { provider = "host", determinism = "BitExact" }\n')
        model = og.validate("engine/ceir/ops/mem.ceirop.toml", raw, tomllib.loads(raw))
        cpp = og.emit_cpp(model)
        nat_line = [ln for ln in cpp.splitlines() if 'register_op("nat"' in ln][0]
        self.assertIn(".intrinsic = true", nat_line)
        self.assertIn('.native_provider = "host"', nat_line)
        plain_line = [ln for ln in cpp.splitlines() if 'register_op("plain"' in ln][0]
        self.assertNotIn(".intrinsic", plain_line)
        self.assertNotIn(".native_provider", plain_line)

    def test_kernel_ref_promotes_to_register_op(self):
        # CEIR-13c: an [op.kernel_ref] op must emit .kernel_ref_symbol/.kernel_ref_interface into the register_op OpSpec
        # (the promotion the sec 106 collector reads); a NON-kernel-ref op's register call stays byte-identical.
        raw = ('schema_version = 1\ndialect = "t"\nsummary = "s"\n\n'
               '[[op]]\nname = "plain"\nsummary = "s"\nversion = 1\n\n'
               '[[op]]\nname = "disp"\nsummary = "s"\nversion = 1\n'
               'attributes = [ { name = "kernel", kind = "symbol" }, { name = "iface", kind = "int" } ]\n'
               'kernel_ref = { symbol = "kernel", interface = "iface" }\n')
        model = og.validate("engine/ceir/ops/mem.ceirop.toml", raw, tomllib.loads(raw))
        cpp = og.emit_cpp(model)
        disp_line = [ln for ln in cpp.splitlines() if 'register_op("disp"' in ln][0]
        self.assertIn('.kernel_ref_symbol = "kernel"', disp_line)
        self.assertIn('.kernel_ref_interface = "iface"', disp_line)
        plain_line = [ln for ln in cpp.splitlines() if 'register_op("plain"' in ln][0]
        self.assertNotIn(".kernel_ref_symbol", plain_line)

    def test_kernel_ref_interface_optional(self):
        # `interface` is OPTIONAL: a kernel_ref with only `symbol` emits .kernel_ref_symbol and NOT .kernel_ref_interface.
        raw = (GOOD + 'attributes = [ { name = "kernel", kind = "symbol" } ]\n'
               'kernel_ref = { symbol = "kernel" }\n')
        model = og.validate("engine/ceir/ops/mem.ceirop.toml", raw, tomllib.loads(raw))
        line = [ln for ln in og.emit_cpp(model).splitlines() if 'register_op("a"' in ln][0]
        self.assertIn('.kernel_ref_symbol = "kernel"', line)
        self.assertNotIn(".kernel_ref_interface", line)

    def test_kernel_ref_symbol_must_name_an_attr(self):
        self._assert_error(GOOD + 'kernel_ref = { symbol = "nope" }\n', "must name an attribute")

    def test_kernel_ref_symbol_wrong_kind(self):
        raw = (GOOD + 'attributes = [ { name = "kernel", kind = "int" } ]\n'
               'kernel_ref = { symbol = "kernel" }\n')
        self._assert_error(raw, "must be kind 'symbol'")

    def test_kernel_ref_interface_wrong_kind(self):
        raw = (GOOD + 'attributes = [ { name = "kernel", kind = "symbol" }, { name = "iface", kind = "string" } ]\n'
               'kernel_ref = { symbol = "kernel", interface = "iface" }\n')
        self._assert_error(raw, "must be kind 'int'")

    def test_kernel_ref_unknown_field(self):
        raw = (GOOD + 'attributes = [ { name = "kernel", kind = "symbol" } ]\n'
               'kernel_ref = { symbol = "kernel", bogus = "x" }\n')
        self._assert_error(raw, "unknown field")

    def test_unknown_top_field(self):
        raw = 'schema_version = 1\ndialect = "t"\nsummary = "s"\nbogus = 1\n\n[[op]]\nname = "a"\nsummary = "s"\nversion = 1\n'
        self._assert_error(raw, "unknown top-level field 'bogus'", line=4)

    def test_bad_schema_version(self):
        raw = GOOD.replace("schema_version = 1", "schema_version = 2")
        self._assert_error(raw, "schema_version must be 1", line=1)

    def test_missing_required_version(self):
        raw = 'schema_version = 1\ndialect = "t"\nsummary = "s"\n\n[[op]]\nname = "a"\nsummary = "s"\n'
        self._assert_error(raw, "needs an integer 'version'")

    def test_unknown_op_field(self):
        raw = GOOD + 'bogus = "x"\n'
        self._assert_error(raw, "unknown field 'bogus'")

    def test_bad_trait(self):
        raw = GOOD + 'traits = ["Nonsense"]\n'
        self._assert_error(raw, "unknown trait 'Nonsense'")

    def test_stateedge_trait_accepted(self):
        # CEIR-5d: StateEdge is a valid trait (core.state/delay/history); it validates and lands in the generated register.
        raw = GOOD + 'traits = ["StateEdge"]\n'
        self.assertIsNone(run(raw))
        cpp = og.emit_cpp(og.validate("t.ceirop.toml", raw, tomllib.loads(raw)))
        self.assertIn("OpTrait::StateEdge", cpp)

    def test_token_traits_accepted(self):
        # CEIR-6a: TokenProducer + TokenConsumer are valid traits (ceir.async); an op may carry both (join).
        raw = GOOD + 'traits = ["TokenProducer", "TokenConsumer"]\n'
        self.assertIsNone(run(raw))
        cpp = og.emit_cpp(og.validate("t.ceirop.toml", raw, tomllib.loads(raw)))
        self.assertIn("OpTrait::TokenProducer", cpp)
        self.assertIn("OpTrait::TokenConsumer", cpp)

    def test_bad_attr_kind(self):
        raw = GOOD + 'attributes = [ { name = "v", kind = "blah" } ]\n'
        self._assert_error(raw, "kind must be one of")

    def test_duplicate_op(self):
        raw = GOOD + '\n[[op]]\nname = "a"\nsummary = "again"\nversion = 1\n'
        self._assert_error(raw, "duplicate op name 'a'")

    def test_variadic_not_last(self):
        raw = GOOD + 'operands = [ { name = "x", variadic = true }, { name = "y" } ]\n'
        self._assert_error(raw, "only the LAST operand may be variadic")

    def test_result_variadic_only_last(self):
        raw = GOOD + 'results = [ { name = "a", variadic = true }, { name = "b" } ]\n'
        self._assert_error(raw, "only the LAST result may be variadic")

    def test_result_variadic_omits_fixed_count_check(self):
        # a lone variadic result admits any count -> the verifier emits NO num_results check (like a lone variadic operand).
        raw = GOOD + 'results = [ { name = "r", variadic = true } ]\n'
        self.assertIsNone(run(raw))
        cpp = og.emit_cpp(og.validate("t.ceirop.toml", raw, tomllib.loads(raw)))
        self.assertNotIn("num_results()", cpp)

    def test_region_unknown_field(self):
        self._assert_error(GOOD + 'regions = [ { kind = "graph", bogus = 1 } ]\n', "region: unknown field 'bogus'")

    def test_region_arg_needs_name(self):
        self._assert_error(GOOD + 'regions = [ { kind = "graph", args = [ { type_hint = "x" } ] } ]\n',
                           "each region arg needs an identifier 'name'")

    def test_region_variadic_only_last(self):
        raw = GOOD + 'regions = [ { kind = "graph", variadic = true }, { kind = "graph" } ]\n'
        self._assert_error(raw, "only the LAST region may be variadic")

    def test_region_signature_emits_argcount_check(self):
        # a positive check: a region with a declared arg gets an entry-block arg-count check in the generated verifier.
        raw = GOOD + 'regions = [ { kind = "graph", args = [ { name = "iv" } ] } ]\n'
        self.assertIsNone(run(raw))
        cpp = og.emit_cpp(og.validate("t.ceirop.toml", raw, tomllib.loads(raw)))
        self.assertIn("first_block()->num_args() != 1U", cpp)

    def test_duplicate_member_name(self):
        raw = GOOD + 'operands = [ { name = "x" } ]\nresults = [ { name = "x" } ]\n'
        self._assert_error(raw, "duplicate member name 'x'")

    def test_bad_native_provider(self):
        raw = GOOD + '\n[op.native]\nprovider = "quantum"\ndeterminism = "BitExact"\n'
        self._assert_error(raw, "provider must be one of")

    def test_native_missing_provider(self):
        raw = GOOD + '\n[op.native]\ndeterminism = "BitExact"\n'
        self._assert_error(raw, "'provider' is required")

    def test_native_bad_determinism(self):
        raw = GOOD + '\n[op.native]\nprovider = "host"\ndeterminism = "MostlyDeterministic"\n'
        self._assert_error(raw, "determinism (required) must be one of")

    def test_native_missing_determinism(self):
        raw = GOOD + '\n[op.native]\nprovider = "host"\n'
        self._assert_error(raw, "determinism (required) must be one of")

    def test_native_thread_safe_not_bool(self):
        raw = GOOD + '\n[op.native]\nprovider = "host"\ndeterminism = "BitExact"\nthread_safe = "yes"\n'
        self._assert_error(raw, "'thread_safe' must be a bool")

    def test_effects_not_array(self):
        self._assert_error(GOOD + 'effects = "io"\n', "'effects' must be an array")

    def test_effect_family_vocabulary_in_lockstep_with_cpp(self):
        # ⛔ §26's 27 families + CEIR-8c's 8 U-§19 families = 35; effect.hpp's static_assert pins the C++ side. This pins
        # the generator side so an append to one language alone is caught (a C++-only append silently makes a family
        # undeclarable from TOML; a TOML-only append breaks the ordinal lockstep the C++ static_assert relies on).
        self.assertEqual(len(og.EFFECT_FAMILIES), 35)
        self.assertEqual(og.EFFECT_FAMILIES[0], "MemoryRead")
        self.assertEqual(og.EFFECT_FAMILIES[26], "Debug")       # the §26 boundary is preserved (ordinals 0..26 unchanged)
        self.assertEqual(og.EFFECT_FAMILIES[-1], "AgentAction")

    def test_op_trait_vocabulary_in_lockstep_with_cpp(self):
        # ⛔ CEIR-8e (ADR-0115): OpTrait is a CLOSED core vocabulary (8 bits 0..7); dialect.hpp's kKnownTraitsMask +
        # static_assert pin the C++ side. This pins the generator side so a C++/TOML trait append can't drift (the
        # EFFECT_FAMILIES precedent). OP_TRAITS[i] corresponds to OpTrait bit i.
        self.assertEqual(len(og.OP_TRAITS), 8)
        self.assertEqual(og.OP_TRAITS[0], "Terminator")
        self.assertEqual(og.OP_TRAITS[-1], "TokenConsumer")

    def test_effect_unknown_family_string(self):
        self._assert_error(GOOD + 'effects = ["Bogus"]\n', "unknown effect family 'Bogus'")

    def test_effect_unknown_family_table(self):
        self._assert_error(GOOD + 'effects = [ { family = "Bogus" } ]\n', "unknown effect family 'Bogus'")

    def test_effect_table_needs_family(self):
        self._assert_error(GOOD + 'effects = [ { range = ["byte"] } ]\n', "an effect table needs a 'family'")

    def test_effect_unknown_field(self):
        self._assert_error(GOOD + 'effects = [ { family = "MemoryRead", bogus = 1 } ]\n', "unknown field 'bogus'")

    def test_effect_operand_index_out_of_range(self):
        raw = GOOD + 'operands = [ { name = "x" } ]\neffects = [ { family = "MemoryWrite", operand = 3 } ]\n'
        self._assert_error(raw, "operand index 3 out of range (op has 1 operands)")

    def test_effect_result_index_out_of_range(self):
        raw = GOOD + 'results = [ { name = "y" } ]\neffects = [ { family = "MemoryWrite", result = 3 } ]\n'
        self._assert_error(raw, "result index 3 out of range (op has 1 results)")

    def test_effect_operand_and_result(self):
        raw = (GOOD + 'operands = [ { name = "x" } ]\nresults = [ { name = "y" } ]\n'
               'effects = [ { family = "MemoryRead", operand = 0, result = 0 } ]\n')
        self._assert_error(raw, "targets an operand OR a result, not both")

    def test_effect_unknown_range(self):
        raw = GOOD + 'operands = [ { name = "x" } ]\neffects = [ { family = "MemoryRead", operand = 0, range = ["nope"] } ]\n'
        self._assert_error(raw, "unknown range 'nope'")

    def test_effect_pure_with_effects_rejected(self):
        # ⛔ Pure ⇒ zero effects (§26): a Pure op is CSE/DCE-safe, which any declared effect disqualifies.
        self._assert_error(GOOD + 'traits = ["Pure"]\neffects = ["MemoryRead"]\n', "Pure trait must declare zero effects")

    def test_determinism_tier_vocabulary_lockstep_with_cpp(self):
        # ⛔ §27 has 5 declarable tiers; semantics.hpp's static_assert pins the C++ side, this pins the generator side.
        self.assertEqual(len(og.DETERMINISM_TIERS), 5)
        self.assertEqual(og.DETERMINISM_TIERS[0], "BitExact")
        self.assertEqual(og.DETERMINISM_TIERS[-1], "ExternalNondeterminism")  # §27-verbatim, not the old short "External"

    def test_determinism_unknown_value(self):
        self._assert_error(GOOD + 'determinism = "Bogus"\n', "determinism must be one of")

    def test_determinism_native_weaker_than_op_rejected(self):
        # the op claims BitExact but its native impl is only Nondeterministic — a native impl cannot be less reproducible.
        raw = GOOD + 'determinism = "BitExact"\n\n[op.native]\nprovider = "host"\ndeterminism = "Nondeterministic"\n'
        self._assert_error(raw, "weaker than the op's declared class")

    def test_determinism_native_stronger_than_op_ok(self):
        # the inverse is fine: a specific native impl may be STRONGER than the op's abstract contract.
        raw = GOOD + 'determinism = "DeterministicWithinBackend"\n\n[op.native]\nprovider = "host"\ndeterminism = "BitExact"\n'
        self.assertIsNone(run(raw))

    def test_domain_vocabulary_lockstep_with_cpp(self):
        # ⛔ §15 has 10 domains; semantics.hpp's static_assert pins the C++ side, this pins the generator side.
        self.assertEqual(len(og.EVAL_DOMAINS), 10)
        self.assertEqual(og.EVAL_DOMAINS[0], "CompileTime")
        self.assertEqual(og.EVAL_DOMAINS[-1], "EitherHostOrDevice")

    def test_domain_unknown_value(self):
        self._assert_error(GOOD + 'domain = "gpu"\n', "domain must be one of")  # "gpu" was the old scaffold placeholder

    def test_domain_good_validates(self):
        self.assertIsNone(run(GOOD + 'domain = "DeviceTime"\n'))

    def test_effect_both_forms_validate_and_parse(self):
        # a positive check: a bare family AND an identity-bearing table both parse into the normalized model.
        raw = (GOOD + 'operands = [ { name = "x" } ]\nresults = [ { name = "y" } ]\n'
               'effects = ["Logging", { family = "MemoryWrite", result = 0, range = ["element"] }]\n')
        self.assertIsNone(run(raw))
        model = og.validate("t.ceirop.toml", raw, tomllib.loads(raw))
        eff = model["ops"][0]["effects"]
        self.assertEqual(len(eff), 2)
        self.assertEqual((eff[0]["family"], eff[0]["target"], eff[0]["range_mask"]), ("Logging", "None", 0))
        self.assertEqual((eff[1]["family"], eff[1]["target"], eff[1]["index"], eff[1]["range_mask"]),
                         ("MemoryWrite", "Result", 0, 2))  # element = ViewRange bit 1<<1

    def test_pure_variadic_verifier_has_no_tautological_check(self):
        # a lone variadic operand admits any operand count; the verifier must NOT emit `< 0U` (always-false; -Wextra/tidy)
        raw = ('schema_version = 1\ndialect = "pv"\nsummary = "s"\n\n[[op]]\nname = "p"\nsummary = "s"\nversion = 1\n'
               'operands = [ { name = "xs", variadic = true } ]\n')
        self.assertNotIn("< 0U", og.emit_cpp(og.validate("pv.ceirop.toml", raw, tomllib.loads(raw))))
        # but a fixed+variadic op DOES emit a lower-bound check on the fixed prefix
        raw2 = raw.replace('operands = [ { name = "xs", variadic = true } ]',
                           'operands = [ { name = "a" }, { name = "xs", variadic = true } ]')
        self.assertIn("< 1U", og.emit_cpp(og.validate("pv.ceirop.toml", raw2, tomllib.loads(raw2))))

    def test_docs_not_string(self):
        self._assert_error(GOOD + "docs = 5\n", "'docs' must be a string")

    def test_deprecation_bad_field_type(self):
        self._assert_error(GOOD + "deprecation = { since = 3 }\n", "deprecation.since must be a string")

    def test_deprecation_unknown_field(self):
        self._assert_error(GOOD + 'deprecation = { bogus = "x" }\n', "deprecation: unknown field 'bogus'")

    def test_good_native_validates(self):
        raw = GOOD + '\n[op.native]\nprovider = "gpu"\ndeterminism = "DeterministicWithinBackend"\nthread_safe = true\ncapabilities = ["gpu"]\n'
        self.assertIsNone(run(raw))

    def test_json_emitter_parses_and_matches_model(self):
        # the CLI/MCP JSON (section 122) is valid JSON and its op list matches the model it is derived from - it cannot
        # drift from the wrapper/verifier. Uses the real arith dialect.
        import json as _json
        model = og.load_model(os.path.join(og.OPS_DIR, "arith.ceirop.toml"))
        doc = _json.loads(og.emit_json(model))  # must parse
        self.assertEqual(doc["schema_version"], og.KNOWN_SCHEMA_VERSION)
        self.assertEqual(doc["dialect"], "arith")
        self.assertEqual([o["name"] for o in doc["ops"]], [o["name"] for o in model["ops"]])
        self.assertEqual([o["qualified"] for o in doc["ops"]], ["arith." + o["name"] for o in model["ops"]])
        const = next(o for o in doc["ops"] if o["name"] == "const")
        self.assertEqual(const["attributes"], [{"name": "value", "kind": "int", "required": True,
                                                 "doc": "the integer constant"}])
        # arith declares no deprecation/type-shape-inference/fold -> those keys are ABSENT (the key-per-declaration rule
        # that keeps the committed JSON byte-identical when the optional emitters landed).
        for o in doc["ops"]:
            for absent in ("deprecation", "type_inference", "shape_inference", "fold"):
                self.assertNotIn(absent, o)

    def test_md_emitter_renders_every_op(self):
        # the human doc (section 123) renders a heading for every op, is deterministic, and derives from the same model
        # (cannot drift). Pipe/newline escaping keeps a doc string from breaking the markdown table.
        model = og.load_model(os.path.join(og.OPS_DIR, "arith.ceirop.toml"))
        md = og.emit_md(model)
        self.assertTrue(md.startswith("<!-- GENERATED"))
        for op in model["ops"]:
            self.assertIn("## `arith.%s`" % op["name"], md)
        self.assertEqual(md, og.emit_md(model))  # deterministic
        self.assertEqual(og._md_cell("a|b\nc"), "a\\|b c")  # pipe escaped, newline flattened

    def test_optional_fields_render_in_md_and_json_when_declared(self):
        # the branches arith does NOT exercise: deprecation, effects, domain, inference hooks, native binding all render
        # in BOTH artifacts when the TOML declares them (proving they are only OMITTED when absent, not never-emitted).
        import json as _json
        raw = ('schema_version = 1\ndialect = "t"\nsummary = "s"\n\n[[op]]\nname = "a"\nsummary = "op a"\nversion = 2\n'
               'operands = [ { name = "x" } ]\n'
               'effects = ["Logging", { family = "MemoryWrite", operand = 0, range = ["byte"] }]\n'
               'domain = "DeviceTime"\ntype_inference = "same_as_operand"\n'
               'deprecation = { since = "v2", replaced_by = "t.b", note = "use b" }\n'
               '\n[op.native]\nprovider = "gpu"\ndeterminism = "DeterministicWithinBackend"\nthread_safe = true\n'
               'capabilities = ["compute"]\n')
        model = og.validate("engine/ceir/ops/t.ceirop.toml", raw, tomllib.loads(raw))
        md = og.emit_md(model)
        self.assertIn("DEPRECATED", md)
        self.assertIn("**Deprecated:** since v2 — replaced by `t.b` — use b", md)
        self.assertIn("**Effects:** `Logging`, `MemoryWrite on operand 0 [byte]`", md)
        self.assertIn("**Domain:** `DeviceTime`", md)
        self.assertIn("**Type inference:** `same_as_operand`", md)
        self.assertIn("**Native binding:** provider=`gpu`", md)
        self.assertIn("thread_safe=`true`", md)  # a bool renders lowercase, not Python's `True`
        self.assertNotIn("`True`", md)
        self.assertIn("capabilities=`compute`", md)
        op = _json.loads(og.emit_json(model))["ops"][0]
        self.assertEqual(op["type_inference"], "same_as_operand")
        self.assertEqual(op["deprecation"], {"since": "v2", "replaced_by": "t.b", "note": "use b"})
        self.assertEqual(op["native"]["provider"], "gpu")

    def test_smoke_negative_case_only_when_empty_is_malformed(self):
        # the generated "verifier rejects malformed" case must exist ONLY for ops an empty construct is malformed for:
        # a pure-variadic-operand op (min operands 0, no required attr, no region) is well-formed when empty -> no
        # CHECK_FALSE; a required-attr op gets one. (Freezes the guard against a vacuously-true verifier at 2d.)
        va = ('schema_version = 1\ndialect = "va"\nsummary = "s"\n\n[[op]]\nname = "v"\nsummary = "s"\nversion = 1\n'
              'operands = [ { name = "xs", variadic = true } ]\n')
        self.assertNotIn("CHECK_FALSE", og.emit_smoke(og.validate("va.ceirop.toml", va, tomllib.loads(va))))
        ra = ('schema_version = 1\ndialect = "ra"\nsummary = "s"\n\n[[op]]\nname = "r"\nsummary = "s"\nversion = 1\n'
              'attributes = [ { name = "a", kind = "int", required = true } ]\n')
        self.assertIn("CHECK_FALSE", og.emit_smoke(og.validate("ra.ceirop.toml", ra, tomllib.loads(ra))))


if __name__ == "__main__":
    unittest.main(verbosity=2)
