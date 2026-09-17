#!/usr/bin/env python3
# DIAG.6c(f): verified external CPU-sampling runbook (Windows WPR/xperf; Linux perf).
#
# Records a sampled CPU profile of a workload that has a KNOWN UNSCOPED hotspot -- a tight loop with NO CRD_PERF_SCOPE
# (tests/support/diag/specimens/cpu_hotspot_specimen.cpp, symbol `crd_diag_unscoped_hotspot_burn`) -- and confirms the
# hotspot is visible through SAMPLING. That is acceptance clause 3: "a known unscoped CPU hotspot is visible through
# sampling; scope-only charts do not satisfy this case." The in-process scope profiler cannot see this work at all;
# only an external sampler can.
#
# This is a VERDICT MACHINE, not a pass/fail test: it prints exactly one verdict and exits accordingly.
#   HOTSPOT_VISIBLE      (exit 0)  the symbol appears in the sampled profile (or --stacks call stacks)
#   HOTSPOT_NOT_VISIBLE  (exit 4)  a profile with stacks was captured but the symbol is absent (symbolication/specimen)
#   PERMISSION_DENIED    (exit 3)  wpr/perf could not start the kernel sampler (needs an elevated/privileged shell)
#   TOOL_MISSING         (exit 2)  wpr/xperf (or perf) not installed
#   MISSING_STACKS       (exit 5)  --stacks only: a profile was captured but carried no walked call stacks
#   USAGE                (exit 64) bad arguments
#
# --stacks (DIAG.6c(g), acceptance clause 2) records with the `Profile` stackwalk and processes with `-a stack`, so
# the optimized/fiber call stacks -- not just flat function buckets -- must show the hotspot. It is the external
# (WPA/xperf) counterpart to the in-process RtlCaptureStackBackTrace oracle in
# tests/foundation/jobs/test_diag_unwind_qualification.cpp, and is elevation-gated the same way (PERMISSION_DENIED
# on a shell without SeSystemProfilePrivilege).
#
# Windows note: `wpr -start CPU` needs SeSystemProfilePrivilege. An ordinary "Run as administrator" shell usually has
# it; the Claude/desktop-app shell observed at authoring time did NOT (wpr returned 0xc5585011 "Failed to enable the
# policy to profile system performance"). The two admin probes on that shell disagreed -- `net session` returned 0 but
# `IsUserAnAdmin()` was false -- so neither is authoritative here; wpr's own privilege check is. When wpr denies the
# start the honest result is PERMISSION_DENIED -- NEVER an empty trace treated as "no hotspot". Re-run from a real
# elevated PowerShell.
#
# xperf syntax is checked against the installed `xperf -help processing` / `-help symbols`: `-symbols` is a top-level
# PROCESSING flag placed BEFORE the action; `-a profile -detail` buckets samples by function name when symbol decoding
# is enabled. Execution of the full pipeline is pending a privileged run.
#
# Usage:
#   python scripts/sample-cpu-wpr.py [--specimen <exe>] [--symbols <dir>] [--out <dir>] [--stacks] [--linux]
import argparse
import os
import re
import shutil
import subprocess
import sys

HOTSPOT_SYMBOL = "crd_diag_unscoped_hotspot_burn"


def _which(name):
    return shutil.which(name)


def _has_stack_frames(body):
    # A decoded call stack shows module!symbol frames. Their absence means the trace carried no walked stacks
    # (stackwalk not enabled / all samples stackless) -- a DIFFERENT failure from "stacks present but our symbol
    # is not among them". re.search over the whole body is enough to tell the two apart.
    return re.search(r"\w+!\w+", body) is not None


def _is_admin_windows():
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def run_windows(specimen, symbols_dir, out_dir, stacks=False):
    # stacks=False (clause 3): does the unscoped hotspot appear in a flat sampled profile? Verdicts VISIBLE /
    # NOT_VISIBLE / PERMISSION_DENIED / TOOL_MISSING.
    # stacks=True (clause 2, DIAG.6c(g)): do the optimized frames appear in the sampled CALL STACKS -- the
    # external counterpart to the in-process RtlCaptureStackBackTrace oracle (tests/foundation/jobs/
    # test_diag_unwind_qualification.cpp). Adds a MISSING_STACKS verdict: a profile was captured but it carried
    # no walked stacks (stackwalk absent), which is NOT the same as "stacks present, our symbol missing".
    wpr = _which("wpr")
    xperf = _which("xperf")
    if wpr is None or xperf is None:
        print("TOOL_MISSING: wpr/xperf not on PATH (install the Windows Performance Toolkit)")
        return 2
    if not os.path.isfile(specimen):
        print(f"TOOL_MISSING: specimen not built: {specimen}")
        return 2

    etl = os.path.join(out_dir, "crd_hotspot.etl")
    txt = os.path.join(out_dir, "crd_hotspot_profile.txt")
    env = dict(os.environ)
    # LOCAL symbols only -- never srv*...msdl.microsoft.com (that would trigger a network symbol download).
    env["_NT_SYMBOL_PATH"] = symbols_dir

    started = False
    try:
        start = subprocess.run([wpr, "-start", "CPU", "-filemode"], capture_output=True, text=True)
        if start.returncode != 0:
            msg = (start.stderr or start.stdout or "").strip().replace("\n", " ")
            print(f"PERMISSION_DENIED: wpr -start failed (rc={start.returncode}): {msg}")
            print("  -> re-run from an elevated shell with SeSystemProfilePrivilege")
            return 3
        started = True

        subprocess.run([specimen], capture_output=True, text=True)  # burn ~2s, unscoped

        stop = subprocess.run([wpr, "-stop", etl], capture_output=True, text=True)
        if stop.returncode != 0:
            msg = (stop.stderr or stop.stdout or "").strip().replace("\n", " ")
            print(f"PERMISSION_DENIED: wpr -stop failed (rc={stop.returncode}): {msg}")
            return 3
        started = False  # -stop consumed the session

        # -symbols is a processing flag BEFORE the action. Actions checked against `xperf -help processing`:
        #   flat profile  -> `-a profile -detail` (per-function sample buckets)
        #   call stacks   -> `-a stack`           (walked stacks per sample; the WPR CPU profile enables the
        #                                           `Profile` stackwalk that populates them)
        action = ["-a", "stack"] if stacks else ["-a", "profile", "-detail"]
        exp = subprocess.run(
            [xperf, "-i", etl, "-o", txt, "-symbols"] + action,
            capture_output=True, text=True, env=env,
        )
        if exp.returncode != 0 or not os.path.isfile(txt):
            msg = (exp.stderr or exp.stdout or "").strip().replace("\n", " ")
            print(f"HOTSPOT_NOT_VISIBLE: xperf export failed (rc={exp.returncode}): {msg}")
            return 4

        with open(txt, "r", errors="ignore") as fh:
            body = fh.read()
        kind = "sampled call stacks" if stacks else "sampled profile"
        if HOTSPOT_SYMBOL in body:
            print(f"HOTSPOT_VISIBLE: {HOTSPOT_SYMBOL} found in the {kind} ({txt})")
            return 0
        if stacks and not _has_stack_frames(body):
            # A profile was captured but no walked stacks decoded -- distinct from "stacks present, symbol absent".
            print(f"MISSING_STACKS: the trace carried no decoded call stacks ({txt}); enable the `Profile` "
                  f"stackwalk / check _NT_SYMBOL_PATH={symbols_dir}")
            return 5
        print(f"HOTSPOT_NOT_VISIBLE: {HOTSPOT_SYMBOL} not in the {kind} "
              f"(symbolication? check _NT_SYMBOL_PATH={symbols_dir})")
        return 4
    finally:
        if started:
            subprocess.run([wpr, "-cancel"], capture_output=True, text=True)  # never orphan a kernel session


def run_linux(specimen, out_dir):
    perf = _which("perf")
    if perf is None:
        print("TOOL_MISSING: perf not installed (Linux external sampler)")
        return 2
    if not os.path.isfile(specimen):
        print(f"TOOL_MISSING: specimen not built: {specimen}")
        return 2
    data = os.path.join(out_dir, "crd_hotspot.perf.data")
    rec = subprocess.run([perf, "record", "-g", "-o", data, specimen], capture_output=True, text=True)
    if rec.returncode != 0:
        msg = (rec.stderr or rec.stdout or "").strip().replace("\n", " ")
        # perf_event_paranoid / privilege issues surface here.
        print(f"PERMISSION_DENIED: perf record failed (rc={rec.returncode}): {msg}")
        return 3
    rep = subprocess.run([perf, "report", "--stdio", "-i", data], capture_output=True, text=True)
    if HOTSPOT_SYMBOL in (rep.stdout or ""):
        print(f"HOTSPOT_VISIBLE: {HOTSPOT_SYMBOL} found in perf report")
        return 0
    print(f"HOTSPOT_NOT_VISIBLE: {HOTSPOT_SYMBOL} not in perf report")
    return 4


def main(argv):
    ap = argparse.ArgumentParser(description="DIAG.6c(f) external CPU-sampling runbook")
    default_spec = os.path.join("build", "win-debug", "tests", "diag", "crd-diag-cpu-hotspot-specimen.exe")
    default_syms = os.path.join("build", "win-debug", "tests", "diag")
    ap.add_argument("--specimen", default=default_spec)
    ap.add_argument("--symbols", default=default_syms)
    ap.add_argument("--out", default=os.environ.get("TEMP", "."))
    ap.add_argument("--linux", action="store_true")
    ap.add_argument("--stacks", action="store_true",
                    help="qualify optimized/fiber CALL STACKS (clause 2), not just a flat profile (clause 3)")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    if args.linux or sys.platform.startswith("linux"):
        return run_linux(args.specimen, args.out)
    if sys.platform != "win32":
        print("USAGE: unsupported platform; use --linux on Linux")
        return 64
    if not _is_admin_windows():
        # Not fatal by itself (wpr's own privilege check is authoritative) but a useful early signal.
        print("note: not running as administrator; wpr -start may be denied", file=sys.stderr)
    return run_windows(args.specimen, args.symbols, args.out, stacks=args.stacks)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
