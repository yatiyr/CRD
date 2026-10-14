#!/usr/bin/env python3
"""DIAG.9c: reverse-debugging and watchpoint recipes with an observed verdict machine.

Contract: docs/design/runtime-diagnostics.md#diag-9c. Every route runs the DIAG.9c corruption specimen
(tests/support/diag/specimens/corruption_specimen.cpp), whose `corrupt` mode writes one word past four cells into a
sentinel. A route passes only when it names `crd_diag_corrupt_neighbour` as the writer of that sentinel, at the
specimen's marked line, and only when the specimen's own uninstrumented run detected the same corruption. The clean
mode is the valid control: a route that names a writer there is an instrument failure, as is a missing tool, a refused
permission, a recording that lost the write, a trace over its byte bound or missing symbols.

Routes
  gdb-record  Linux. gdb's process record (`record full`) from the window's start to its end with a bounded
              instruction log, then a live `reverse-continue` to a `watch -l` on the sentinel. The log is saved with
              `record full save` and replayed offline in a second gdb: `record full restore`, then a bisection over
              instruction numbers with `record goto` finds the first instruction after which the sentinel differs.
              gdb 15.1 (Ubuntu 24.04) hits an internal error (`find_inferior_pid: Assertion 'pid != 0'`) on
              `continue`, `reverse-continue` or a watchpoint over a restored log, so the offline leg navigates and
              reads values only. Process record serializes execution: real multicore races stay with TSan.
  gdb-watch   Linux. A forward hardware watchpoint (`watch -l`) on the sentinel; a software fallback is refused.
  cdb-watch   Windows. A forward hardware data breakpoint (`ba w8`) with cdb, which the WinDbg package ships.
  rr          Linux. `rr record`, then `rr replay` driven by the same gdb steps as gdb-record. Needs hardware
              performance counters and perf_event_paranoid <= 1; this runbook never lowers that machine-wide setting.
  ttd         Windows. TTD.exe recording (needs an elevated shell), then an offline replay in cdb (`-z <trace>.run`)
              stepping backwards (`g-`) to a `ba w8` on the sentinel.

Verdicts (process exit code without --expect; with --expect the exit code is 0 only for the expected verdict):
  FOUND 0, CLEAN 0, TOOL_MISSING 2, PERMISSION_DENIED 3, SYMBOLS_MISSING 4, INCOMPLETE_RECORDING 5, NOT_FOUND 6,
  FALSE_FIND 7, OBSERVER_EFFECT 8, SPECIMEN_INVALID 9, HOST_INELIGIBLE 10.

Usage
  python scripts/reverse-debug-recipes.py --probe
  python scripts/reverse-debug-recipes.py --route gdb-record --mode corrupt --specimen <exe> --source <cpp>
      --trace-dir <dir> [--insn-max N] [--trace-max-bytes N] [--expect FOUND] [--json <file>]
  python scripts/reverse-debug-recipes.py --route ttd ... --replay-trace <trace.run>
      (judge a recorded TTD trace again without recording; only recording needs an elevated shell)
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

VERDICTS = {
    "FOUND": 0,
    "CLEAN": 0,
    "TOOL_MISSING": 2,
    "PERMISSION_DENIED": 3,
    "SYMBOLS_MISSING": 4,
    "INCOMPLETE_RECORDING": 5,
    "NOT_FOUND": 6,
    "FALSE_FIND": 7,
    "OBSERVER_EFFECT": 8,
    "SPECIMEN_INVALID": 9,
    "HOST_INELIGIBLE": 10,
}

ROUTES = ("gdb-record", "gdb-watch", "cdb-watch", "rr", "ttd")
WRITER = "crd_diag_corrupt_neighbour"
WINDOW_BEGIN = "crd_diag_corruption_window_begin"
WINDOW_END = "crd_diag_corruption_window_end"
BLOCK = "crd_diag_corruption_block"
SENTINEL_WORD = 4
SENTINEL_OFFSET = 32
SENTINEL = 0x5EED5EEDC0FFEE11
DETECTED_EXIT = 3
GDB_WATCH_EXPR = "%s.words[%d]" % (BLOCK, SENTINEL_WORD)
DEFAULT_INSN_MAX = 200000
DEFAULT_TRACE_MAX_BYTES = 64 * 1024 * 1024
DEFAULT_TIMEOUT = 120

# Text a debugger prints when a recording is no longer whole: each one makes the route's evidence incomplete.
INCOMPLETE_MARKERS = (
    "internal-error",
    "does not support instruction",
    "failed to record execution log",
    "Process record: failed",
    "A problem internal to GDB",
)

# Windows error codes a refused launch surfaces as (access denied, elevation required).
WINDOWS_PERMISSION_ERRORS = (5, 740)


# --------------------------------------------------------------------------------------------------------------------
# The specimen's source markers: where the writer is and which line performs the corrupting store.
# --------------------------------------------------------------------------------------------------------------------
def writer_location(source_text):
    """Return {'begin', 'end', 'write'} line numbers (1-based) from the specimen's markers, or None."""
    begin = end = write = None
    for number, line in enumerate(source_text.splitlines(), start=1):
        if "// CRD_DIAG_WRITER_BEGIN" in line:
            begin = number
        elif "// CRD_DIAG_WRITER_END" in line:
            end = number
        elif "// CRD_DIAG_CORRUPTING_WRITE" in line:
            write = number
    if begin is None or end is None or write is None or not begin < write < end:
        return None
    return {"begin": begin, "end": end, "write": write}


def judge_writer(frame, location, source_name, exact):
    """Classify a debugger's frame at the corrupting store: FOUND, SYMBOLS_MISSING or NOT_FOUND.

    A reverse route stops before the store executes, so it must name the marked line (exact). A forward watchpoint
    stops after it, so any line of the writer's body is accepted.
    """
    if not frame or not frame.get("name"):
        return "SYMBOLS_MISSING", "the stop has no function symbol"
    if WRITER not in frame["name"]:
        return "NOT_FOUND", "the stop is in %s, not %s" % (frame["name"], WRITER)
    if not frame.get("file") or not frame.get("line"):
        return "SYMBOLS_MISSING", "%s resolved without a source line" % WRITER
    if Path(frame["file"].replace("\\", "/")).name.lower() != source_name.lower():
        return "NOT_FOUND", "the stop's source is %s, not %s" % (frame["file"], source_name)
    line = int(frame["line"])
    if exact and line != location["write"]:
        return "NOT_FOUND", "the stop is at line %d, the corrupting store is line %d" % (line, location["write"])
    if not exact and not location["begin"] < line < location["end"]:
        return "NOT_FOUND", "line %d is outside the writer (lines %d-%d)" % (line, location["begin"], location["end"])
    return "FOUND", "%s at %s:%d" % (WRITER, source_name, line)


def judge_natural(mode, natural):
    """The uninstrumented run must behave as its mode says, or the specimen itself is not the claimed one."""
    expected = DETECTED_EXIT if mode == "corrupt" else 0
    if natural.get("error"):
        return "SPECIMEN_INVALID", natural["error"]
    if natural["exit"] != expected or natural["detected"] != (mode == "corrupt"):
        return "SPECIMEN_INVALID", "the uninstrumented %s run exited %s (detected=%s), expected %d" % (
            mode, natural["exit"], natural["detected"], expected)
    return None


def run_natural(specimen, mode, timeout):
    try:
        proc = subprocess.run([specimen, mode], capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"error": "the specimen did not run: %s" % error, "exit": None, "detected": False}
    return {"exit": proc.returncode, "detected": "CRD_DIAG_CORRUPTION=detected" in (proc.stdout or ""),
            "stdout": (proc.stdout or "")[-400:]}


# --------------------------------------------------------------------------------------------------------------------
# gdb: the drivers run inside gdb's Python and print one CRD_RD_JSON line; everything else here only reads text.
# --------------------------------------------------------------------------------------------------------------------
GDB_DRIVER_COMMON = r'''
import gdb, json, os
cfg = json.load(open(os.environ["CRD_RD_CONFIG"], encoding="utf-8"))
res = {"steps": []}

def ex(cmd):
    try:
        out = gdb.execute(cmd, to_string=True)
        res["steps"].append({"cmd": cmd, "out": out[-2000:]})
        return out, None
    except gdb.error as error:
        res["steps"].append({"cmd": cmd, "error": str(error)})
        return "", str(error)

def frame():
    try:
        f = gdb.selected_frame()
        sal = f.find_sal()
        return {"name": f.name(), "file": sal.symtab.filename if sal.symtab else None, "line": sal.line}
    except (gdb.error, RuntimeError) as error:
        return {"error": str(error)}

def sentinel():
    try:
        return int(gdb.parse_and_eval(cfg["watch_expr"])) & 0xFFFFFFFFFFFFFFFF
    except (gdb.error, RuntimeError):
        return None

def record_bounds():
    out, err = ex("info record")
    bounds = {}
    for line in out.splitlines():
        words = line.replace(".", " ").split()
        if line.startswith("Lowest recorded instruction number is"):
            bounds["lowest"] = int(words[-1])
        elif line.startswith("Highest recorded instruction number is"):
            bounds["highest"] = int(words[-1])
        elif line.startswith("Log contains"):
            bounds["count"] = int(words[2])
    return bounds

last_stop = {}

def on_stop(event):
    # Stop notifications are printed, not returned by gdb.execute, so stops are classified from their events.
    last_stop.clear()
    last_stop["kind"] = type(event).__name__
    if isinstance(event, gdb.BreakpointEvent):
        last_stop["watch"] = any(b.type in (gdb.BP_HARDWARE_WATCHPOINT, gdb.BP_WATCHPOINT)
                                 for b in event.breakpoints)
    if isinstance(event, gdb.SignalEvent):
        last_stop["signal"] = event.stop_signal

gdb.events.stop.connect(on_stop)

def stopped_at_watch():
    return bool(last_stop.get("watch"))

# Never reach the network for symbols: the runbook resolves only the specimen's own debug information.
for setting in ("set debuginfod enabled off", "set confirm off", "set pagination off", "set width 0",
                "set print pretty off"):
    ex(setting)
'''

GDB_DRIVER_LIVE = GDB_DRIVER_COMMON + r'''
route = cfg["route"]
if route == "gdb-record":
    ex("set record full insn-number-max %d" % cfg["insn_max"])
    ex("set record full stop-at-limit off")
for name in (cfg["window_begin"], cfg["window_end"]):
    out, err = ex("break " + name)
    if err:
        res["missing_symbol"] = name
if "missing_symbol" not in res:
    if route == "rr":
        # A replay starts at exec, before the window: run on to its end (through the window's start).
        for _ in range(3):
            out, err = ex("continue")
            if err or cfg["window_end"] in (frame().get("name") or ""):
                break
    else:
        out, err = ex("run " + cfg["mode"])
        res["begin_stop"] = frame()
        if route == "gdb-record":
            ex("record full")
        elif route == "gdb-watch":
            out, err = ex("watch -l " + cfg["watch_expr"])
            res["hardware"] = out.lstrip().startswith("Hardware watchpoint")
        out, err = ex("continue")
    res["first_stop"] = frame()
    res["first_stop_kind"] = dict(last_stop)
    if route == "gdb-watch" and stopped_at_watch():
        res["writer"] = res["first_stop"]
        ex("delete")
        ex("break " + cfg["window_end"])
        ex("continue")
    res["end_stop"] = frame()
    res["end_sentinel"] = sentinel()
    if route in ("gdb-record", "rr"):
        if route == "gdb-record":
            res["bounds"] = record_bounds()
            out, err = ex("record save " + cfg["trace"])
            res["saved"] = err is None and "Saved core file" in out
        ex("delete")
        out, err = ex("watch -l " + cfg["watch_expr"])
        last_stop.clear()
        out, err = ex("reverse-continue")
        res["reverse_stop_kind"] = dict(last_stop)
        res["reverse_error"] = err
        # Without a watchpoint stop, reverse execution ran to the start of the log: no write in the history.
        res["no_history"] = err is None and not stopped_at_watch()
        res["writer"] = frame() if stopped_at_watch() else None
    if route != "rr":
        ex("kill")
print("CRD_RD_JSON " + json.dumps(res))
'''

GDB_DRIVER_OFFLINE = GDB_DRIVER_COMMON + r'''
def goto(number):
    try:
        gdb.execute("record goto %d" % number, to_string=True)
        return True
    except gdb.error as error:
        if "Already at target" in str(error):
            return True
        res["goto_error"] = str(error)
        return False

out, err = ex("core-file " + cfg["trace"])
out, err = ex("record full restore " + cfg["trace"])
res["restored"] = err is None
bounds = record_bounds()
res["bounds"] = bounds
if res["restored"] and "lowest" in bounds and "count" in bounds:
    # A restored log reports one instruction past its last navigable one as its highest; navigate by the count.
    low = bounds["lowest"]
    high = low + bounds["count"] - 1
    if goto(low):
        res["first_value"] = sentinel()
    if goto(high):
        res["last_value"] = sentinel()
    if res.get("first_value") is not None and res.get("last_value") is not None \
            and res["first_value"] != res["last_value"]:
        lo, hi = low, high
        while lo < hi:
            mid = (lo + hi) // 2
            if not goto(mid):
                break
            if sentinel() != res["first_value"]:
                hi = mid
            else:
                lo = mid + 1
        res["first_changed"] = lo
        if goto(lo - 1):
            res["writer"] = frame()
            res["before_value"] = sentinel()
        if goto(lo):
            res["after_value"] = sentinel()
print("CRD_RD_JSON " + json.dumps(res))
'''


def parse_driver_output(text):
    """The last CRD_RD_JSON line of a driver run, or None when the driver never finished (a gdb crash)."""
    found = None
    for line in (text or "").splitlines():
        if line.startswith("CRD_RD_JSON "):
            try:
                found = json.loads(line[len("CRD_RD_JSON "):])
            except ValueError:
                found = None
    return found


def incomplete_reason(text):
    if (text or "").startswith("CRD_RD_TIMEOUT: "):
        return text.splitlines()[0][len("CRD_RD_TIMEOUT: "):]
    for marker in INCOMPLETE_MARKERS:
        if marker in (text or ""):
            return "the debugger reported '%s'" % marker
    return None


def decide_gdb_record(mode, natural, live_text, offline_text, location, source_name, trace_bytes, trace_max,
                      route="gdb-record"):
    """The verdict of a recorded route (gdb-record, or rr with offline_text None) from the drivers' output."""
    bad = judge_natural(mode, natural)
    if bad:
        return bad
    live = parse_driver_output(live_text)
    reason = incomplete_reason(live_text)
    if live is None:
        return "INCOMPLETE_RECORDING", reason or "the live %s session ended without its result" % route
    if live.get("missing_symbol"):
        return "SYMBOLS_MISSING", "the debugger has no symbol %s" % live["missing_symbol"]
    if reason:
        return "INCOMPLETE_RECORDING", reason
    end = live.get("end_stop") or {}
    if WINDOW_END not in (end.get("name") or ""):
        return "INCOMPLETE_RECORDING", "the recorded run did not reach %s" % WINDOW_END
    recorded_detected = live.get("end_sentinel") is not None and live["end_sentinel"] != SENTINEL
    if live.get("end_sentinel") is None:
        return "SYMBOLS_MISSING", "the debugger could not read %s" % GDB_WATCH_EXPR
    if recorded_detected != natural["detected"]:
        return "OBSERVER_EFFECT", "uninstrumented detected=%s, recorded detected=%s" % (
            natural["detected"], recorded_detected)
    bounds = live.get("bounds") or {}
    truncated = bounds.get("lowest", 1) > 1
    if route == "gdb-record":
        if not live.get("saved"):
            return "INCOMPLETE_RECORDING", "the recording was not saved"
        if trace_bytes is None:
            return "INCOMPLETE_RECORDING", "the saved recording is missing"
        if trace_bytes > trace_max:
            return "INCOMPLETE_RECORDING", "the saved recording is %d bytes, over its %d byte bound" % (
                trace_bytes, trace_max)
    if live.get("reverse_error"):
        return "INCOMPLETE_RECORDING", "reverse execution failed: %s" % live["reverse_error"]
    if mode == "clean":
        if not live.get("no_history"):
            return "FALSE_FIND", "the clean run's sentinel has a writer: %s" % live.get("writer")
    else:
        if live.get("no_history"):
            if truncated:
                return "INCOMPLETE_RECORDING", "the write fell out of the bounded log (lowest instruction %d)" % (
                    bounds["lowest"])
            return "NOT_FOUND", "reverse execution reached the start of the log without the write"
        verdict, why = judge_writer(live.get("writer"), location, source_name, exact=True)
        if verdict != "FOUND":
            return verdict, "live: " + why
    if route != "gdb-record":
        return ("CLEAN", "no writer of the sentinel in the recording") if mode == "clean" else (
            "FOUND", "live: " + judge_writer(live["writer"], location, source_name, True)[1])
    offline = parse_driver_output(offline_text)
    reason = incomplete_reason(offline_text)
    if offline is None or reason:
        return "INCOMPLETE_RECORDING", "offline replay: " + (reason or "the session ended without its result")
    if not offline.get("restored"):
        return "INCOMPLETE_RECORDING", "offline replay: the saved log was not restored"
    if offline.get("goto_error"):
        return "INCOMPLETE_RECORDING", "offline replay: " + offline["goto_error"]
    changed = offline.get("first_changed") is not None
    if mode == "clean":
        if changed:
            return "FALSE_FIND", "offline replay found a write at instruction %d" % offline["first_changed"]
        return "CLEAN", "no writer of the sentinel, live or offline"
    if not changed:
        if truncated:
            return "INCOMPLETE_RECORDING", "offline replay: the saved log does not hold the write"
        return "NOT_FOUND", "offline replay: the sentinel never changes in the saved log"
    if offline.get("before_value") != SENTINEL:
        return "NOT_FOUND", "offline replay: the sentinel before instruction %d is not the original" % (
            offline["first_changed"])
    verdict, why = judge_writer(offline.get("writer"), location, source_name, exact=True)
    if verdict != "FOUND":
        return verdict, "offline replay: " + why
    return "FOUND", "live and offline: " + why


def decide_gdb_watch(mode, natural, text, location, source_name):
    bad = judge_natural(mode, natural)
    if bad:
        return bad
    live = parse_driver_output(text)
    if live is None:
        return "INCOMPLETE_RECORDING", incomplete_reason(text) or "the gdb session ended without its result"
    if live.get("missing_symbol"):
        return "SYMBOLS_MISSING", "the debugger has no symbol %s" % live["missing_symbol"]
    if not live.get("hardware"):
        return "HOST_INELIGIBLE", "gdb set a software watchpoint; this route is the hardware one"
    end = live.get("end_stop") or {}
    if WINDOW_END not in (end.get("name") or "") or live.get("end_sentinel") is None:
        return "INCOMPLETE_RECORDING", "the watched run did not reach %s" % WINDOW_END
    watched_detected = live["end_sentinel"] != SENTINEL
    if watched_detected != natural["detected"]:
        return "OBSERVER_EFFECT", "uninstrumented detected=%s, watched detected=%s" % (
            natural["detected"], watched_detected)
    if mode == "clean":
        if live.get("writer"):
            return "FALSE_FIND", "the clean run's watchpoint triggered in %s" % live["writer"].get("name")
        return "CLEAN", "the watchpoint never triggered"
    if not live.get("writer"):
        return "NOT_FOUND", "the watchpoint never triggered"
    return judge_writer(live["writer"], location, source_name, exact=False)


def run_gdb_driver(gdb, driver_text, config, trace_dir, args, timeout):
    name = "gdb_%s.py" % config["leg"]
    driver = trace_dir / name
    driver.write_text(driver_text, encoding="utf-8")
    config_path = trace_dir / ("gdb_%s.json" % config["leg"])
    config_path.write_text(json.dumps(config), encoding="utf-8")
    env = dict(os.environ, CRD_RD_CONFIG=str(config_path))
    try:
        proc = subprocess.run([gdb, "-nx", "-batch", "-x", str(driver)] + args, capture_output=True, text=True,
                              timeout=timeout, env=env, cwd=str(trace_dir))
    except subprocess.TimeoutExpired as error:
        partial = error.stdout.decode("utf-8", "replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        text = "CRD_RD_TIMEOUT: the %s session ran past %ss\n%s" % (config["leg"], timeout, partial)
    else:
        text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    (trace_dir / ("gdb_%s.log" % config["leg"])).write_text(text, encoding="utf-8")
    return text


def gdb_tool():
    gdb = shutil.which("gdb")
    if gdb is None:
        return None, None, "gdb is not on PATH"
    try:
        version = subprocess.run([gdb, "--version"], capture_output=True, text=True, timeout=30)
        python = subprocess.run([gdb, "-nx", "-batch", "-ex", "python print('CRD_RD_PY_OK')"], capture_output=True,
                                text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, None, "gdb did not run: %s" % error
    if "CRD_RD_PY_OK" not in (python.stdout or ""):
        return None, None, "gdb has no Python support"
    return gdb, (version.stdout or "").splitlines()[0] if version.stdout else "unknown", None


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def ptrace_refused():
    # Yama mode 3 forbids every attach and launch-under-trace; modes 0-2 allow gdb to start its own inferior.
    return read_text("/proc/sys/kernel/yama/ptrace_scope") == "3"


def route_gdb(args, report):
    gdb, version, missing = gdb_tool()
    report["tool"] = {"name": "gdb", "path": gdb, "version": version}
    if missing:
        return "TOOL_MISSING", missing
    report["ptrace_scope"] = read_text("/proc/sys/kernel/yama/ptrace_scope")
    if ptrace_refused():
        return "PERMISSION_DENIED", "Yama ptrace_scope is 3; this runbook never lowers it"
    trace_dir = args.trace_dir
    config = {"route": args.route, "mode": args.mode, "watch_expr": GDB_WATCH_EXPR, "window_begin": WINDOW_BEGIN,
              "window_end": WINDOW_END, "insn_max": args.insn_max, "trace": str(trace_dir / "corruption.gdbrec")}
    trace = Path(config["trace"])
    if trace.exists():
        trace.unlink()
    live_text = run_gdb_driver(gdb, GDB_DRIVER_LIVE, dict(config, leg="live"), trace_dir, [args.specimen],
                               args.timeout)
    report["live"] = parse_driver_output(live_text)
    if args.route == "gdb-watch":
        return decide_gdb_watch(args.mode, report["natural"], live_text, report["location"], args.source.name)
    trace_bytes = trace.stat().st_size if trace.exists() else None
    report["trace"] = {"path": str(trace), "bytes": trace_bytes, "max_bytes": args.trace_max_bytes,
                       "insn_max": args.insn_max}
    offline_text = ""
    if trace_bytes is not None:
        offline_text = run_gdb_driver(gdb, GDB_DRIVER_OFFLINE, dict(config, leg="offline"), trace_dir,
                                      [args.specimen], args.timeout)
        report["offline"] = parse_driver_output(offline_text)
    return decide_gdb_record(args.mode, report["natural"], live_text, offline_text, report["location"],
                             args.source.name, trace_bytes, args.trace_max_bytes)


# --------------------------------------------------------------------------------------------------------------------
# rr: eligibility is observed, never arranged. The replay reuses the gdb live driver.
# --------------------------------------------------------------------------------------------------------------------
def rr_eligibility(paranoid, event_sources):
    """Why rr cannot record on this host, or None. `event_sources` lists /sys/bus/event_source/devices."""
    if not any(name == "cpu" or name.startswith("cpu_") for name in event_sources):
        return "HOST_INELIGIBLE", "no hardware performance counters (no cpu PMU under /sys/bus/event_source/devices)"
    if paranoid is None or int(paranoid) > 1:
        return "PERMISSION_DENIED", "perf_event_paranoid is %s; rr needs <= 1, a machine-wide change this " \
                                    "runbook never makes" % paranoid
    return None


def route_rr(args, report):
    rr = shutil.which("rr")
    report["tool"] = {"name": "rr", "path": rr}
    if rr is None:
        return "TOOL_MISSING", "rr is not on PATH"
    version = subprocess.run([rr, "--version"], capture_output=True, text=True, timeout=30)
    report["tool"]["version"] = (version.stdout or "").strip()
    paranoid = read_text("/proc/sys/kernel/perf_event_paranoid")
    try:
        sources = sorted(os.listdir("/sys/bus/event_source/devices"))
    except OSError:
        sources = []
    report["perf_event_paranoid"] = paranoid
    report["event_sources"] = sources
    blocked = rr_eligibility(paranoid, sources)
    if blocked:
        return blocked
    gdb, gdb_version, missing = gdb_tool()
    if missing:
        return "TOOL_MISSING", missing
    trace = args.trace_dir / "rr-trace"
    if trace.exists():
        shutil.rmtree(trace)
    recorded = subprocess.run([rr, "record", "-o", str(trace), str(args.specimen), args.mode], capture_output=True,
                              text=True, timeout=args.timeout)
    report["rr_record_exit"] = recorded.returncode
    if not trace.is_dir():
        return "INCOMPLETE_RECORDING", "rr record wrote no trace: %s" % (recorded.stderr or "")[-300:]
    trace_bytes = sum(f.stat().st_size for f in trace.rglob("*") if f.is_file())
    report["trace"] = {"path": str(trace), "bytes": trace_bytes, "max_bytes": args.trace_max_bytes}
    if trace_bytes > args.trace_max_bytes:
        return "INCOMPLETE_RECORDING", "the rr trace is %d bytes, over its %d byte bound" % (trace_bytes,
                                                                                            args.trace_max_bytes)
    config = {"route": "rr", "leg": "rr", "mode": args.mode, "watch_expr": GDB_WATCH_EXPR,
              "window_begin": WINDOW_BEGIN, "window_end": WINDOW_END}
    driver = args.trace_dir / "gdb_rr.py"
    driver.write_text(GDB_DRIVER_LIVE, encoding="utf-8")
    config_path = args.trace_dir / "gdb_rr.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    replay = subprocess.run([rr, "replay", str(trace), "--", "-nx", "-batch", "-x", str(driver)], capture_output=True,
                            text=True, timeout=args.timeout, env=dict(os.environ, CRD_RD_CONFIG=str(config_path)))
    text = (replay.stdout or "") + "\n" + (replay.stderr or "")
    report["live"] = parse_driver_output(text)
    return decide_gdb_record(args.mode, report["natural"], text, None, report["location"], args.source.name,
                             trace_bytes, args.trace_max_bytes, route="rr")


# --------------------------------------------------------------------------------------------------------------------
# Windows: cdb and TTD from the WinDbg package (not on PATH), the Windows Kits debuggers or PATH.
# --------------------------------------------------------------------------------------------------------------------
def windbg_package_dir():
    """The WinDbg package's install directory, asked of the package manager (its folder cannot be listed)."""
    if os.name != "nt":
        return None
    try:
        proc = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                               "(Get-AppxPackage Microsoft.WinDbg).InstallLocation"], capture_output=True, text=True,
                              timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    location = (proc.stdout or "").strip().splitlines()
    return Path(location[0]) if location and location[0] else None


def find_windows_tool(name, package_relative):
    found = shutil.which(name)
    if found:
        return Path(found)
    package = windbg_package_dir()
    if package is not None and (package / package_relative).is_file():
        return package / package_relative
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / \
        "Debuggers" / "x64" / name
    return kits if kits.is_file() else None


def cdb_module(specimen):
    return re.sub(r"[^0-9A-Za-z_]", "_", Path(specimen).stem)


CDB_FRAME = re.compile(r"(?P<module>[\w]+)!(?P<name>[\w:~<>]+)(?:\+0x[0-9a-fA-F]+)?\s+\[(?P<file>[^\]]+?) @ "
                       r"(?P<line>\d+)\]")
CDB_SYMBOL_ONLY = re.compile(r"(?P<module>[\w]+)!(?P<name>[\w:~<>]+)(?:\+0x[0-9a-fA-F]+)?")
CDB_QWORD = re.compile(r"^[0-9a-fA-F`]{8,17}\s+(?P<value>[0-9a-fA-F]{8}`[0-9a-fA-F]{8})\s*$", re.MULTILINE)


# The data breakpoint and the window-end breakpoint carry fixed IDs, so each stop says which one it was.
CDB_WATCH_ID = 7
CDB_END_ID = 8
CDB_HIT = re.compile(r"^Breakpoint (?P<id>\d+) hit\s*$", re.MULTILINE)


def parse_cdb_stops(text):
    """Split a cdb transcript at its CRD_RD_STOP markers: each stop's breakpoint, top frame and sentinel read.

    cdb prints `Breakpoint <id> hit` when it stops, before the stop's marker, so a stop's breakpoint is the last one
    reported in the text before its marker. A stop with no breakpoint (process exit, start of a trace) has id None.
    """
    stops = []
    # The markers are matched as whole lines: cdb echoes its initial command line, which names them too.
    segments = re.split(r"(?m)^CRD_RD_STOP\d\s*$", text or "")
    for index, block in enumerate(segments[1:], start=1):
        hits = CDB_HIT.findall(segments[index - 1])
        frame = None
        for line in block.splitlines():
            if re.match(r"^\s*00\s", line):
                match = CDB_FRAME.search(line)
                if match:
                    frame = {"name": match.group("name"), "file": match.group("file"), "line": int(match.group("line"))}
                else:
                    symbol = CDB_SYMBOL_ONLY.search(line)
                    frame = {"name": symbol.group("name") if symbol else None, "file": None, "line": None}
                break
        value = CDB_QWORD.search(block)
        stops.append({"breakpoint": int(hits[-1]) if hits else None, "frame": frame,
                      "sentinel": int(value.group("value").replace("`", ""), 16) if value else None})
    return stops


def decide_cdb(mode, natural, text, location, source_name, reverse=False):
    """Judge a cdb transcript of the cdb-watch route, or of a TTD replay when reverse is true.

    cdb-watch runs forward: the data breakpoint (corrupt mode) stops before the window-end breakpoint. A TTD replay
    first runs to the window end, then steps backwards (`g-`) to the data breakpoint, or to the trace's start.
    """
    bad = judge_natural(mode, natural)
    if bad:
        return bad
    stops = parse_cdb_stops(text)
    if not stops:
        return "INCOMPLETE_RECORDING", "cdb produced no stop: %s" % (text or "")[-300:].strip()
    ends = [stop for stop in stops if stop["breakpoint"] == CDB_END_ID]
    if not ends:
        return "INCOMPLETE_RECORDING", "the run did not reach %s" % WINDOW_END
    if ends[0]["frame"] is None or WINDOW_END != ends[0]["frame"].get("name"):
        return "SYMBOLS_MISSING", "the window-end stop does not resolve to %s" % WINDOW_END
    if ends[0]["sentinel"] is None:
        return "SYMBOLS_MISSING", "cdb could not read %s+0x%x" % (BLOCK, SENTINEL_OFFSET)
    run_detected = ends[0]["sentinel"] != SENTINEL
    if run_detected != natural["detected"]:
        return "OBSERVER_EFFECT", "uninstrumented detected=%s, debugged detected=%s" % (natural["detected"],
                                                                                         run_detected)
    writers = [stop for stop in stops if stop["breakpoint"] == CDB_WATCH_ID]
    if mode == "clean":
        if writers:
            name = (writers[0]["frame"] or {}).get("name")
            return "FALSE_FIND", "the clean run's data breakpoint stopped in %s" % name
        return "CLEAN", "the data breakpoint never triggered"
    if not writers:
        return "NOT_FOUND", "the data breakpoint never triggered"
    return judge_writer(writers[0]["frame"], location, source_name, exact=reverse)


def cdb_command(specimen, reverse):
    module = cdb_module(specimen)
    sentinel = "%s!%s+0x%x" % (module, BLOCK, SENTINEL_OFFSET)
    end = "%s!%s" % (module, WINDOW_END)
    # MSVC's AddressSanitizer commits its shadow memory through first-chance access violations, so the route does not
    # stop on those; an access violation nothing handles still stops at its second chance.
    if reverse:
        # A data breakpoint reports the instruction after the write, so a backward run stops just past the store
        # (measured 2026-10-14: crd_diag_corrupt_neighbour+0x53, the loop's closing line 79, not the store's line 78).
        # `t-` steps back over that one instruction to the store itself before the frame is read.
        return ("sxd av; bp%d %s; g; .echo CRD_RD_STOP1; kn 3; dq %s L1; bc %d; ba%d w8 %s; g-; t-; "
                ".echo CRD_RD_STOP2; kn 3; dq %s L1; q" % (CDB_END_ID, end, sentinel, CDB_END_ID, CDB_WATCH_ID,
                                                           sentinel, sentinel))
    # A hardware breakpoint set at the initial process breakpoint is lost when the loader resets the thread
    # context, so the route first runs to the image entry point.
    return ("sxd av; g @$exentry; ba%d w8 %s; bp%d %s; g; .echo CRD_RD_STOP1; kn 3; dq %s L1; g; "
            ".echo CRD_RD_STOP2; kn 3; dq %s L1; q" % (CDB_WATCH_ID, sentinel, CDB_END_ID, end, sentinel, sentinel))


def route_cdb_watch(args, report):
    cdb = find_windows_tool("cdb.exe", Path("amd64") / "cdb.exe")
    report["tool"] = {"name": "cdb", "path": str(cdb) if cdb else None}
    if cdb is None:
        return "TOOL_MISSING", "cdb is neither on PATH nor in the WinDbg package or the Windows Kits debuggers"
    symbols = str(Path(args.specimen).parent)
    proc = subprocess.run([str(cdb), "-lines", "-y", symbols, "-c", cdb_command(args.specimen, False),
                           str(args.specimen), args.mode], capture_output=True, text=True, timeout=args.timeout)
    text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    (args.trace_dir / "cdb_watch.log").write_text(text, encoding="utf-8")
    version = re.search(r"Microsoft \(R\) Windows Debugger Version (\S+)", text)
    report["tool"]["version"] = version.group(1) if version else None
    report["stops"] = parse_cdb_stops(text)
    return decide_cdb(args.mode, report["natural"], text, report["location"], args.source.name)


def launch_refusal(error, tool=None):
    """Classify an OSError from launching a tool: PERMISSION_DENIED for access denied or elevation required."""
    if getattr(error, "winerror", None) in WINDOWS_PERMISSION_ERRORS or isinstance(error, PermissionError):
        if tool is not None and "windowsapps" in str(tool).lower() and is_elevated():
            # Measured 2026-10-14: the TTD.exe inside the WinDbg Store package is refused even in an elevated shell;
            # files inside an app package are not launchable directly. The standalone package adds a ttd.exe alias.
            return "PERMISSION_DENIED", ("%s: %s is inside an app package and cannot be launched directly; install "
                                         "the standalone TTD (winget install --id Microsoft.TimeTravelDebugging -e)"
                                         % (error, tool))
        return "PERMISSION_DENIED", "%s (an elevated shell is required)" % error
    return "TOOL_MISSING", "the tool did not start: %s" % error


def is_elevated():
    if os.name != "nt":
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        return False


def route_ttd(args, report):
    ttd = find_windows_tool("TTD.exe", Path("amd64") / "ttd" / "TTD.exe")
    cdb = find_windows_tool("cdb.exe", Path("amd64") / "cdb.exe")
    report["tool"] = {"name": "TTD", "path": str(ttd) if ttd else None, "cdb": str(cdb) if cdb else None,
                      "elevated": is_elevated()}
    if ttd is None or cdb is None:
        return "TOOL_MISSING", "TTD.exe or cdb.exe is missing (install the WinDbg package)"
    if getattr(args, "replay_trace", None):
        # Recording needs an elevated shell; replaying a recorded trace does not.
        trace = Path(args.replay_trace)
        if not trace.is_file():
            return "INCOMPLETE_RECORDING", "no such trace: %s" % trace
        report["trace"] = {"path": str(trace), "bytes": trace.stat().st_size, "replayed_only": True}
        return replay_ttd(args, report, cdb, trace)
    trace_dir = args.trace_dir / "ttd"
    if trace_dir.exists():
        shutil.rmtree(trace_dir)
    trace_dir.mkdir(parents=True)
    max_mb = max(1, args.trace_max_bytes // (1024 * 1024))
    try:
        recorded = subprocess.run([str(ttd), "-accepteula", "-out", str(trace_dir), "-maxFile", str(max_mb),
                                   "-launch", str(args.specimen), args.mode], capture_output=True, text=True,
                                  timeout=args.timeout)
    except OSError as error:
        return launch_refusal(error, ttd)
    report["ttd_record_exit"] = recorded.returncode
    runs = sorted(trace_dir.glob("*.run"))
    if not runs:
        return "INCOMPLETE_RECORDING", "TTD wrote no .run trace: %s" % ((recorded.stdout or "") +
                                                                        (recorded.stderr or ""))[-300:]
    trace_bytes = sum(f.stat().st_size for f in trace_dir.iterdir() if f.is_file())
    report["trace"] = {"path": str(runs[0]), "bytes": trace_bytes, "max_bytes": args.trace_max_bytes}
    if trace_bytes > args.trace_max_bytes:
        return "INCOMPLETE_RECORDING", "the TTD trace is %d bytes, over its %d byte bound" % (trace_bytes,
                                                                                             args.trace_max_bytes)
    return replay_ttd(args, report, cdb, runs[0])


def replay_ttd(args, report, cdb, trace):
    replay = subprocess.run([str(cdb), "-lines", "-y", str(Path(args.specimen).parent), "-z", str(trace), "-c",
                             cdb_command(args.specimen, True)], capture_output=True, text=True, timeout=args.timeout)
    text = (replay.stdout or "") + "\n" + (replay.stderr or "")
    (args.trace_dir / "ttd_replay.log").write_text(text, encoding="utf-8")
    report["stops"] = parse_cdb_stops(text)
    return decide_cdb(args.mode, report["natural"], text, report["location"], args.source.name, reverse=True)


# --------------------------------------------------------------------------------------------------------------------
# Probe: what this host can run, observed.
# --------------------------------------------------------------------------------------------------------------------
def probe():
    rows = []
    if os.name == "nt":
        cdb = find_windows_tool("cdb.exe", Path("amd64") / "cdb.exe")
        ttd = find_windows_tool("TTD.exe", Path("amd64") / "ttd" / "TTD.exe")
        rows.append(("cdb-watch", "available: %s" % cdb if cdb else "TOOL_MISSING"))
        if ttd is None:
            rows.append(("ttd", "TOOL_MISSING"))
        else:
            try:
                subprocess.run([str(ttd), "-help"], capture_output=True, text=True, timeout=60)
                rows.append(("ttd", "launches: %s (elevated=%s)" % (ttd, is_elevated())))
            except OSError as error:
                rows.append(("ttd", "%s: %s" % launch_refusal(error)))
        rows.append(("gdb-record", "not on Windows: the Linux route"))
        rows.append(("rr", "not on Windows: the Linux route"))
    else:
        gdb, version, missing = gdb_tool()
        rows.append(("gdb-record", missing or "available: %s" % version))
        rows.append(("gdb-watch", missing or "available: %s" % version))
        rr = shutil.which("rr")
        if rr is None:
            rows.append(("rr", "TOOL_MISSING"))
        else:
            try:
                sources = sorted(os.listdir("/sys/bus/event_source/devices"))
            except OSError:
                sources = []
            blocked = rr_eligibility(read_text("/proc/sys/kernel/perf_event_paranoid"), sources)
            rows.append(("rr", "%s: %s" % blocked if blocked else "eligible"))
        rows.append(("cdb-watch", "not on Linux: the Windows route"))
        rows.append(("ttd", "not on Linux: the Windows route"))
    width = max(len(route) for route, _ in rows)
    print("reverse-debugging routes on this host (observed, not assumed):")
    for route, state in rows:
        print("  %s  %s" % (route.ljust(width), state))
    return 0


ROUTE_FUNCTIONS = {"gdb-record": route_gdb, "gdb-watch": route_gdb, "rr": route_rr, "cdb-watch": route_cdb_watch,
                   "ttd": route_ttd}


def main(argv):
    parser = argparse.ArgumentParser(description="DIAG.9c reverse-debugging and watchpoint recipes")
    parser.add_argument("--probe", action="store_true", help="report which routes this host can run")
    parser.add_argument("--route", choices=ROUTES)
    parser.add_argument("--mode", choices=("corrupt", "clean"), default="corrupt")
    parser.add_argument("--specimen", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--trace-dir", type=Path)
    parser.add_argument("--insn-max", type=int, default=DEFAULT_INSN_MAX)
    parser.add_argument("--trace-max-bytes", type=int, default=DEFAULT_TRACE_MAX_BYTES)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument("--expect", choices=sorted(VERDICTS))
    parser.add_argument("--expect-reason", help="with --expect: text the verdict's reason must contain")
    parser.add_argument("--json", type=Path, help="verdict record (default: <trace-dir>/verdict.json)")
    parser.add_argument("--replay-trace", type=Path,
                        help="ttd only: judge this recorded .run trace instead of recording (no elevation needed)")
    args = parser.parse_args(argv)
    if args.probe:
        return probe()
    if not args.route or not args.specimen or not args.source or not args.trace_dir:
        parser.error("--route, --specimen, --source and --trace-dir are required")
    args.trace_dir = args.trace_dir.resolve()
    args.trace_dir.mkdir(parents=True, exist_ok=True)
    report = {"route": args.route, "mode": args.mode, "specimen": str(args.specimen)}
    location = writer_location(read_text(args.source) or "")
    if not args.specimen.is_file() or location is None:
        verdict, reason = "SPECIMEN_INVALID", "the specimen %s or its source markers are missing" % args.specimen
    else:
        report["location"] = location
        report["natural"] = run_natural(str(args.specimen), args.mode, args.timeout)
        verdict, reason = ROUTE_FUNCTIONS[args.route](args, report)
    report["verdict"] = verdict
    report["reason"] = reason
    (args.json or args.trace_dir / "verdict.json").write_text(json.dumps(report, indent=2, default=str),
                                                              encoding="utf-8")
    print("%s: %s (%s, %s mode)" % (verdict, reason, args.route, args.mode))
    if args.expect:
        if verdict != args.expect or (args.expect_reason and args.expect_reason not in reason):
            print("expected %s%s" % (args.expect, " because %s" % args.expect_reason if args.expect_reason else ""))
            return 1
        return 0
    return VERDICTS[verdict]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
