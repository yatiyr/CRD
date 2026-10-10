#!/usr/bin/env python3
"""DIAG.9c: verdict-logic tests for reverse-debug-recipes.py.

The live routes run as CTests where their tools exist (the gdb routes on every Linux lane). These tests cover every
verdict from recorded and synthetic debugger output, including the ones a healthy host never produces: a gdb internal
error, a recording that lost the write, a trace over its byte bound, an observer effect, a clean run with a writer, a
refused TTD launch and an rr host without hardware counters. They need no debugger and run on every host.
Run: python scripts/test-reverse-debug-recipes.py
"""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("reverse_debug_recipes", ROOT / "scripts" / "reverse-debug-recipes.py")
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

SOURCE = ROOT / "tests" / "support" / "diag" / "specimens" / "corruption_specimen.cpp"
SOURCE_NAME = SOURCE.name
LOCATION = mod.writer_location(SOURCE.read_text(encoding="utf-8"))
SPECIMEN_FILE = "/mnt/d/Dev/cerid/tests/support/diag/specimens/corruption_specimen.cpp"
NATURAL_CORRUPT = {"exit": 3, "detected": True}
NATURAL_CLEAN = {"exit": 0, "detected": False}
CHANGED = mod.SENTINEL + 7


def frame(name, line, file=SPECIMEN_FILE):
    return {"name": name, "file": file, "line": line}


def driver(**fields):
    return "noise before\nCRD_RD_JSON " + json.dumps(dict({"steps": []}, **fields)) + "\n"


def live(mode="corrupt", **overrides):
    fields = {
        "begin_stop": frame(mod.WINDOW_BEGIN, LOCATION["begin"] - 16),
        "end_stop": frame(mod.WINDOW_END, LOCATION["begin"] - 4),
        "end_sentinel": CHANGED if mode == "corrupt" else mod.SENTINEL,
        "bounds": {"lowest": 1, "highest": 34124, "count": 34123},
        "saved": True,
        "reverse_error": None,
        "no_history": mode == "clean",
        "writer": frame(mod.WRITER, LOCATION["write"]) if mode == "corrupt" else None,
    }
    fields.update(overrides)
    return driver(**fields)


def offline(mode="corrupt", **overrides):
    fields = {"restored": True, "bounds": {"lowest": 1, "highest": 34124, "count": 34123},
              "first_value": mod.SENTINEL, "last_value": CHANGED if mode == "corrupt" else mod.SENTINEL}
    if mode == "corrupt":
        fields.update({"first_changed": 79, "writer": frame(mod.WRITER, LOCATION["write"]),
                       "before_value": mod.SENTINEL, "after_value": CHANGED})
    fields.update(overrides)
    return driver(**fields)


def record(mode="corrupt", live_text=None, offline_text=None, natural=None, trace_bytes=2066232,
           trace_max=mod.DEFAULT_TRACE_MAX_BYTES):
    natural = natural or (NATURAL_CORRUPT if mode == "corrupt" else NATURAL_CLEAN)
    return mod.decide_gdb_record(mode, natural, live(mode) if live_text is None else live_text,
                                 offline(mode) if offline_text is None else offline_text, LOCATION, SOURCE_NAME,
                                 trace_bytes, trace_max)[0]


# A cdb-watch transcript as cdb 10.0.29547 printed it for the win-debug specimen (trimmed to the stops).
CDB_HEADER = ("0:000> cdb: Reading initial command 'g @$exentry; ba7 w8 m!crd_diag_corruption_block+0x20; bp8 m; g; "
              ".echo CRD_RD_STOP1; kn 3; .echo CRD_RD_STOP2; q'\n")
CDB_WRITER_STOP = ("Breakpoint 7 hit\nCRD_RD_STOP1\n # Child-SP          RetAddr               Call Site\n"
                   "00 000000d6`b69cfc70 00007ff7`ca1117ae     crd_diag_corruption_specimen!crd_diag_corrupt_neighbour"
                   "+0x53 [D:\\Dev\\cerid\\tests\\support\\diag\\specimens\\corruption_specimen.cpp @ %d]\n"
                   "01 000000d6`b69cfc90 00007ff7`ca112279     crd_diag_corruption_specimen!main+0x10e "
                   "[D:\\Dev\\cerid\\tests\\support\\diag\\specimens\\corruption_specimen.cpp @ 114]\n"
                   "00007ff7`ca11c020  5eed5eed`c0ffee18\n")
CDB_END_STOP = ("Breakpoint 8 hit\nCRD_RD_STOP%d\n # Child-SP          RetAddr               Call Site\n"
                "00 000000d6`b69cfc88 00007ff7`ca111806     crd_diag_corruption_specimen!crd_diag_corruption_window_end "
                "[D:\\Dev\\cerid\\tests\\support\\diag\\specimens\\corruption_specimen.cpp @ 67]\n"
                "00007ff7`ca11c020  %s\n")
CDB_EXIT_STOP = ("CRD_RD_STOP2\n # Child-SP          RetAddr               Call Site\n"
                 "00 000000d6`b69cfa28 00007ffa`aa30f0e4     ntdll!NtTerminateProcess+0x14\n")


def cdb_corrupt(line=None):
    return (CDB_HEADER + CDB_WRITER_STOP % (line or LOCATION["write"] + 1) + CDB_END_STOP % (2, "5eed5eed`c0ffee18"))


def cdb_clean():
    return CDB_HEADER + CDB_END_STOP % (1, "5eed5eed`c0ffee11") + CDB_EXIT_STOP


class SpecimenContractTests(unittest.TestCase):
    def test_markers_locate_the_writer(self):
        self.assertIsNotNone(LOCATION)
        self.assertLess(LOCATION["begin"], LOCATION["write"])
        self.assertLess(LOCATION["write"], LOCATION["end"])
        lines = SOURCE.read_text(encoding="utf-8").splitlines()
        self.assertIn("block.words[i]", lines[LOCATION["write"] - 1])

    def test_missing_markers_are_refused(self):
        self.assertIsNone(mod.writer_location("int main() { return 0; }\n"))

    def test_specimen_symbols_match_the_runbook(self):
        text = SOURCE.read_text(encoding="utf-8")
        for symbol in (mod.WRITER, mod.WINDOW_BEGIN, mod.WINDOW_END, mod.BLOCK):
            self.assertIn(symbol, text)
        self.assertIn("0x5EED5EEDC0FFEE11ULL", text)
        self.assertEqual(mod.SENTINEL, 0x5EED5EEDC0FFEE11)

    def test_a_specimen_that_no_longer_corrupts_is_invalid(self):
        # The off-by-one repaired: the corrupt run exits 0 and every route must refuse it, never report CLEAN.
        verdict = mod.judge_natural("corrupt", {"exit": 0, "detected": False})
        self.assertEqual(verdict[0], "SPECIMEN_INVALID")
        self.assertIsNone(mod.judge_natural("corrupt", NATURAL_CORRUPT))
        self.assertIsNone(mod.judge_natural("clean", NATURAL_CLEAN))
        self.assertEqual(record(natural={"exit": 0, "detected": False}), "SPECIMEN_INVALID")


class WriterJudgementTests(unittest.TestCase):
    def test_exact_line_for_reverse_routes(self):
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, LOCATION["write"]), LOCATION, SOURCE_NAME, True)[0],
                         "FOUND")
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, LOCATION["write"] - 2), LOCATION, SOURCE_NAME, True)[0],
                         "NOT_FOUND")

    def test_body_range_for_forward_watchpoints(self):
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, LOCATION["write"] + 1), LOCATION, SOURCE_NAME, False)[0],
                         "FOUND")
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, LOCATION["end"] + 3), LOCATION, SOURCE_NAME, False)[0],
                         "NOT_FOUND")

    def test_missing_symbols(self):
        self.assertEqual(mod.judge_writer({"name": None}, LOCATION, SOURCE_NAME, True)[0], "SYMBOLS_MISSING")
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, 0, None), LOCATION, SOURCE_NAME, True)[0],
                         "SYMBOLS_MISSING")

    def test_another_function_or_file_is_not_the_writer(self):
        self.assertEqual(mod.judge_writer(frame("main", LOCATION["write"]), LOCATION, SOURCE_NAME, True)[0],
                         "NOT_FOUND")
        self.assertEqual(mod.judge_writer(frame(mod.WRITER, LOCATION["write"], "/x/other.cpp"), LOCATION,
                                          SOURCE_NAME, True)[0], "NOT_FOUND")


class GdbRecordVerdictTests(unittest.TestCase):
    def test_found_live_and_offline(self):
        self.assertEqual(record(), "FOUND")

    def test_clean_control(self):
        self.assertEqual(record("clean"), "CLEAN")

    def test_clean_run_with_a_writer_is_a_false_find(self):
        self.assertEqual(record("clean", live_text=live("clean", no_history=False,
                                                        writer=frame(mod.WRITER, LOCATION["write"]))), "FALSE_FIND")
        self.assertEqual(record("clean", offline_text=offline("clean", first_changed=12)), "FALSE_FIND")

    def test_write_out_of_a_truncated_log_is_incomplete(self):
        truncated = {"lowest": 32124, "highest": 34124, "count": 2000}
        text = live(no_history=True, writer=None, bounds=truncated)
        self.assertEqual(record(live_text=text), "INCOMPLETE_RECORDING")

    def test_no_history_without_truncation_is_not_found(self):
        self.assertEqual(record(live_text=live(no_history=True, writer=None)), "NOT_FOUND")

    def test_gdb_internal_error_is_incomplete(self):
        crash = "./gdb/inferior.c:405: internal-error: find_inferior_pid: Assertion `pid != 0' failed.\n"
        self.assertEqual(record(offline_text=crash), "INCOMPLETE_RECORDING")
        self.assertEqual(record(live_text=crash), "INCOMPLETE_RECORDING")

    def test_unsupported_instruction_is_incomplete(self):
        text = live() + "Process record does not support instruction 0xc5 at address 0x7ffff7e1a2b0.\n"
        self.assertEqual(record(live_text=text), "INCOMPLETE_RECORDING")

    def test_trace_over_its_bound_or_unsaved_is_incomplete(self):
        self.assertEqual(record(trace_bytes=5000, trace_max=4096), "INCOMPLETE_RECORDING")
        self.assertEqual(record(trace_bytes=None), "INCOMPLETE_RECORDING")
        self.assertEqual(record(live_text=live(saved=False)), "INCOMPLETE_RECORDING")

    def test_offline_replay_must_agree(self):
        self.assertEqual(record(offline_text=offline(restored=False)), "INCOMPLETE_RECORDING")
        self.assertEqual(record(offline_text=offline(goto_error="Target insn not found.")), "INCOMPLETE_RECORDING")
        self.assertEqual(record(offline_text=offline(writer=frame(mod.WRITER, LOCATION["write"] - 2))), "NOT_FOUND")
        self.assertEqual(record(offline_text=offline(first_changed=None, last_value=mod.SENTINEL)), "NOT_FOUND")
        self.assertEqual(record(offline_text=offline(before_value=CHANGED)), "NOT_FOUND")

    def test_observer_effect(self):
        # The uninstrumented run detected the corruption; the recorded run did not (or the reverse).
        self.assertEqual(record(live_text=live(end_sentinel=mod.SENTINEL)), "OBSERVER_EFFECT")
        self.assertEqual(record("clean", live_text=live("clean", end_sentinel=CHANGED)), "OBSERVER_EFFECT")

    def test_symbols(self):
        self.assertEqual(record(live_text=driver(missing_symbol=mod.WINDOW_BEGIN)), "SYMBOLS_MISSING")
        self.assertEqual(record(live_text=live(writer={"name": None})), "SYMBOLS_MISSING")
        self.assertEqual(record(live_text=live(end_sentinel=None)), "SYMBOLS_MISSING")

    def test_a_recording_that_never_reached_the_window_end(self):
        self.assertEqual(record(live_text=live(end_stop=frame("main", 120))), "INCOMPLETE_RECORDING")
        self.assertEqual(record(live_text=live(reverse_error="Target exec does not support this command.")),
                         "INCOMPLETE_RECORDING")


class GdbWatchVerdictTests(unittest.TestCase):
    def watch(self, mode="corrupt", **overrides):
        fields = {"hardware": True, "end_stop": frame(mod.WINDOW_END, 68),
                  "end_sentinel": CHANGED if mode == "corrupt" else mod.SENTINEL,
                  "writer": frame(mod.WRITER, LOCATION["write"] - 2) if mode == "corrupt" else None}
        fields.update(overrides)
        natural = NATURAL_CORRUPT if mode == "corrupt" else NATURAL_CLEAN
        return mod.decide_gdb_watch(mode, natural, driver(**fields), LOCATION, SOURCE_NAME)[0]

    def test_found_and_clean(self):
        self.assertEqual(self.watch(), "FOUND")
        self.assertEqual(self.watch("clean"), "CLEAN")

    def test_software_watchpoint_is_not_this_route(self):
        self.assertEqual(self.watch(hardware=False), "HOST_INELIGIBLE")

    def test_failures(self):
        self.assertEqual(self.watch(writer=None), "NOT_FOUND")
        self.assertEqual(self.watch("clean", writer=frame(mod.WRITER, LOCATION["write"])), "FALSE_FIND")
        self.assertEqual(self.watch(end_sentinel=mod.SENTINEL), "OBSERVER_EFFECT")
        self.assertEqual(self.watch(end_stop=frame(mod.WRITER, 76)), "INCOMPLETE_RECORDING")


class CdbVerdictTests(unittest.TestCase):
    def test_parse_stops_ignores_the_echoed_command(self):
        stops = mod.parse_cdb_stops(cdb_corrupt())
        self.assertEqual([stop["breakpoint"] for stop in stops], [mod.CDB_WATCH_ID, mod.CDB_END_ID])
        self.assertEqual(stops[0]["frame"]["name"], mod.WRITER)
        self.assertEqual(stops[1]["sentinel"], CHANGED)

    def test_found_and_clean(self):
        self.assertEqual(mod.decide_cdb("corrupt", NATURAL_CORRUPT, cdb_corrupt(), LOCATION, SOURCE_NAME)[0], "FOUND")
        # The clean run's second stop is the process exit, not a writer.
        self.assertEqual(mod.decide_cdb("clean", NATURAL_CLEAN, cdb_clean(), LOCATION, SOURCE_NAME)[0], "CLEAN")

    def test_failures(self):
        natural = NATURAL_CORRUPT
        self.assertEqual(mod.decide_cdb("corrupt", natural, cdb_corrupt(LOCATION["end"] + 5), LOCATION,
                                        SOURCE_NAME)[0], "NOT_FOUND")
        self.assertEqual(mod.decide_cdb("corrupt", natural, CDB_HEADER + CDB_END_STOP % (1, "5eed5eed`c0ffee18"),
                                        LOCATION, SOURCE_NAME)[0], "NOT_FOUND")
        self.assertEqual(mod.decide_cdb("clean", NATURAL_CLEAN, cdb_corrupt(), LOCATION, SOURCE_NAME)[0],
                         "OBSERVER_EFFECT")
        self.assertEqual(mod.decide_cdb("corrupt", natural, "cdb: no target\n", LOCATION, SOURCE_NAME)[0],
                         "INCOMPLETE_RECORDING")
        stripped = cdb_corrupt().replace("crd_diag_corruption_specimen!crd_diag_corrupt_neighbour+0x53 "
                                         "[D:\\Dev\\cerid\\tests\\support\\diag\\specimens\\corruption_specimen.cpp "
                                         "@ %d]" % (LOCATION["write"] + 1), "0x7ff7ca111234")
        self.assertEqual(mod.decide_cdb("corrupt", natural, stripped, LOCATION, SOURCE_NAME)[0], "SYMBOLS_MISSING")

    def test_reverse_replay_requires_the_exact_store(self):
        # A TTD replay stops before the store executes: only the marked line is the writer.
        text = (CDB_HEADER + "Breakpoint 8 hit\n" + (CDB_END_STOP % (1, "5eed5eed`c0ffee18")).split("\n", 1)[1]
                + (CDB_WRITER_STOP % LOCATION["write"]).replace("CRD_RD_STOP1", "CRD_RD_STOP2"))
        self.assertEqual(mod.decide_cdb("corrupt", NATURAL_CORRUPT, text, LOCATION, SOURCE_NAME, reverse=True)[0],
                         "FOUND")
        late = text.replace("@ %d]" % LOCATION["write"], "@ %d]" % (LOCATION["write"] + 1), 1)
        self.assertEqual(mod.decide_cdb("corrupt", NATURAL_CORRUPT, late, LOCATION, SOURCE_NAME, reverse=True)[0],
                         "NOT_FOUND")

    def test_commands_name_the_breakpoint_ids(self):
        forward = mod.cdb_command("C:/b/crd-diag-corruption-specimen.exe", False)
        self.assertIn("ba7 w8 crd_diag_corruption_specimen!crd_diag_corruption_block+0x20", forward)
        self.assertIn("bp8 crd_diag_corruption_specimen!crd_diag_corruption_window_end", forward)
        self.assertTrue(forward.startswith("sxd av; g @$exentry"))
        self.assertIn("g-", mod.cdb_command("C:/b/crd-diag-corruption-specimen.exe", True))


class EligibilityTests(unittest.TestCase):
    def test_rr(self):
        no_pmu = ["breakpoint", "kprobe", "msr", "power", "software", "tracepoint", "uprobe"]
        self.assertEqual(mod.rr_eligibility("2", no_pmu)[0], "HOST_INELIGIBLE")
        self.assertEqual(mod.rr_eligibility("2", no_pmu + ["cpu"])[0], "PERMISSION_DENIED")
        self.assertEqual(mod.rr_eligibility("2", no_pmu + ["cpu_core", "cpu_atom"])[0], "PERMISSION_DENIED")
        self.assertIsNone(mod.rr_eligibility("1", no_pmu + ["cpu"]))

    def test_refused_launch(self):
        denied = OSError(13, "Access is denied")
        denied.winerror = 5
        self.assertEqual(mod.launch_refusal(denied)[0], "PERMISSION_DENIED")
        elevation = OSError(22, "The requested operation requires elevation")
        elevation.winerror = 740
        self.assertEqual(mod.launch_refusal(elevation)[0], "PERMISSION_DENIED")
        self.assertEqual(mod.launch_refusal(FileNotFoundError(2, "missing"))[0], "TOOL_MISSING")

    def test_missing_gdb_is_an_instrument_failure(self):
        with mock.patch.object(mod.shutil, "which", return_value=None):
            self.assertEqual(mod.gdb_tool()[2], "gdb is not on PATH")
            args = mock.Mock(route="gdb-record")
            self.assertEqual(mod.route_gdb(args, {})[0], "TOOL_MISSING")

    def test_missing_ttd_or_cdb_is_an_instrument_failure(self):
        with mock.patch.object(mod, "find_windows_tool", return_value=None):
            self.assertEqual(mod.route_ttd(mock.Mock(), {})[0], "TOOL_MISSING")
            self.assertEqual(mod.route_cdb_watch(mock.Mock(), {})[0], "TOOL_MISSING")


class MainTests(unittest.TestCase):
    def test_missing_specimen_is_refused_and_expect_compares(self):
        with tempfile.TemporaryDirectory() as scratch:
            argv = ["--route", "gdb-record", "--specimen", str(Path(scratch) / "absent"), "--source", str(SOURCE),
                    "--trace-dir", scratch]
            with mock.patch("builtins.print"):
                self.assertEqual(mod.main(argv), mod.VERDICTS["SPECIMEN_INVALID"])
                self.assertEqual(mod.main(argv + ["--expect", "FOUND"]), 1)
                self.assertEqual(mod.main(argv + ["--expect", "SPECIMEN_INVALID"]), 0)
            report = json.loads((Path(scratch) / "verdict.json").read_text(encoding="utf-8"))
            self.assertEqual(report["verdict"], "SPECIMEN_INVALID")

    def test_driver_output_parsing(self):
        self.assertIsNone(mod.parse_driver_output("gdb crashed\n"))
        self.assertEqual(mod.parse_driver_output(driver(saved=True))["saved"], True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
