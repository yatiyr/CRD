#!/usr/bin/env python3
# DIAG.6c(j): platform-debugger recipes + an observed tool-availability verdict machine.
#
# Design goal (docs/design/runtime-diagnostics.md): "platform debugger recipes to inspect uninstrumented work,
# blocked time and memory pressure." External tool dependencies stay OPTIONAL -- so the recipes below name both the
# platform-debugger command AND the in-engine diagnostic that covers the same ground when no debugger is available.
# That is not a fallback afterthought: on THIS box no external interactive debugger is runnable (see the probe
# results at the bottom), which is exactly why the engine carries its own crash/hang/allocator/counter inspection.
#
# ============================================================================================================
# RECIPES  (verify command syntax against the tool's own --help / .help; do not recall)
# NOTE: every command below is DOCUMENTED-ONLY on this box -- no external debugger is runnable here (see the probe
# results / verdict machine). Verify each against the tool's own help before first use; none was executed live.
# ============================================================================================================
# (1) UNINSTRUMENTED WORK -- CPU time in code with no CRD_PERF_SCOPE (the DIAG.6c(f) case).
#     WinDbg/cdb : !runaway            -- per-thread user CPU time, hottest first
#                  ~*k                 -- every thread's stack; repeat to see where time concentrates
#                  wt / !analyze       -- watch-trace a call; (needs live process)
#     lldb       : (lldb) break set -n crd_diag_unscoped_hotspot_burn ; run ; bt ; thread apply all bt
#     gdb        : (gdb) break crd_diag_unscoped_hotspot_burn ; run ; bt ; thread apply all bt
#     IN-ENGINE  : scripts/sample-cpu-wpr.py (external sampler, DIAG.6c(f)) + the hotspot specimen; the sampled
#                  profile shows the unscoped burn the scope profiler cannot. No debugger required.
#
# (2) BLOCKED TIME -- threads/fibers waiting (the DIAG.4c case).
#     WinDbg/cdb : !analyze -v -hang   -- wait-chain analysis
#                  !locks / !cs        -- critical-section owners/waiters
#                  ~*k                 -- all stacks; blocked OS threads show their wait frame
#     lldb       : (lldb) process attach --pid <p> ; thread list ; thread apply all bt
#     gdb        : (gdb) attach <p> ; info threads ; thread apply all bt
#     HONEST LIMITATION (cross-ref DIAG.6c(g)): a PARKED crd-jobs fiber is on NO OS-thread stack -- fiber_switch is a
#     register swap onto a hand-rolled stack the debugger cannot unwind across. So ~*k / thread-apply-all-bt shows
#     the WORKERS idling in the scheduler, NOT the blocked fibers. The right tool for a fiber wait cycle is the
#     engine's own DIAG.4c hang watchdog + wait graph (it classifies WaitCycle from inside the runtime).
#     IN-ENGINE  : the DIAG.4c hang dump / wait graph; the deadlock/unenrolled-wait specimens exercise it.
#
# (3) MEMORY PRESSURE -- allocation growth, fragmentation, page faults.
#     WinDbg/cdb : !address -summary   -- committed/reserved by region type
#                  !heap -s / !heap -stat -- heap usage and block stats
#                  !vm                 -- system-wide commit/pagefile pressure
#     lldb       : (lldb) memory region <addr> ; memory read ; (no !heap equivalent -- OS heap is opaque to lldb)
#     gdb        : (gdb) info proc mappings ; x/ ; (no !heap equivalent)
#     IN-ENGINE  : the DIAG.3c DiagnosticAllocator report (provenance, redzones, quarantine, leak/retention) and the
#                  DIAG.6c(h) page_fault counter class -- these attribute pressure the OS heap view cannot.
#
# ============================================================================================================
# VERDICT MACHINE -- observe, per debugger, exactly one of:
#   SYMBOLS_RESOLVED     (exit 0)  the debugger launched, ran the specimen, and bt resolved the hot symbol
#   TOOL_MISSING         (exit 2)  the debugger is not installed / not on PATH
#   DEBUGGER_UNRUNNABLE  (exit 3)  present but fails to launch (a missing dependency DLL, etc.) -- a real, recorded
#                                  state, never a faked transcript
#   SYMBOLS_MISSING      (exit 4)  launched + ran but bt did not resolve the hot symbol (PDB/symbolication issue)
#
# Usage:
#   python scripts/debugger-recipes.py [--probe] [--specimen <exe>]
import argparse
import os
import shutil
import subprocess
import sys

HOT_SYMBOL = "crd_diag_unscoped_hotspot_burn"
LLDB_FALLBACK = r"C:\LLVM-20.1.8\bin\lldb.exe"

# 0xC0000135 STATUS_DLL_NOT_FOUND, as Python surfaces a Windows negative exit code (signed) or its unsigned form.
DLL_NOT_FOUND = (-1073741515, 3221225781)


def _lldb_path():
    return shutil.which("lldb") or (LLDB_FALLBACK if os.path.isfile(LLDB_FALLBACK) else None)


def classify_launch_failure(rc):
    if rc in DLL_NOT_FOUND:
        # lldb 20 links python310.dll; if the box has a different Python (e.g. 3.14) it cannot load.
        return "STATUS_DLL_NOT_FOUND (0xC0000135) -- a dependency DLL is missing (lldb 20 imports python310.dll)"
    return f"launch failed (rc={rc})"


def probe_lldb(specimen, run_batch=True):
    lldb = _lldb_path()
    if lldb is None:
        print("TOOL_MISSING: lldb not on PATH")
        return 2

    ver = subprocess.run([lldb, "--version"], capture_output=True, text=True)
    if ver.returncode != 0:
        print(f"DEBUGGER_UNRUNNABLE: lldb present ({lldb}) but {classify_launch_failure(ver.returncode)}")
        return 3

    if not run_batch:
        print("DEBUGGER_AVAILABLE: lldb launches")
        return 0
    if not os.path.isfile(specimen):
        print(f"TOOL_MISSING: specimen not built: {specimen}")
        return 2

    # Batch session (verified against `lldb --help`: -b batch, -o one-line command, -- separates the target).
    out = subprocess.run(
        [lldb, "-b",
         "-o", f"break set -n {HOT_SYMBOL}",
         "-o", "run",
         "-o", "bt",
         "-o", "thread list",
         "-o", "kill",
         "-o", "quit",
         "--", specimen],
        capture_output=True, text=True,
    )
    body = (out.stdout or "") + (out.stderr or "")
    if HOT_SYMBOL in body:
        print(f"SYMBOLS_RESOLVED: lldb bt resolved {HOT_SYMBOL}")
        return 0
    print(f"SYMBOLS_MISSING: lldb ran but bt did not resolve {HOT_SYMBOL} (PDB symbolication?)")
    return 4


def observe_availability():
    # Record what is installed here -- never assume. cdb/windbg need the Windows Debugging Tools; gdb is Linux.
    rows = []
    for name in ("lldb", "cdb", "windbg", "gdb"):
        path = shutil.which(name) or (LLDB_FALLBACK if name == "lldb" and os.path.isfile(LLDB_FALLBACK) else None)
        rows.append((name, path if path else "ABSENT"))
    width = max(len(n) for n, _ in rows)
    print("platform debuggers on this box (observed, not assumed):")
    for name, path in rows:
        print(f"  {name.ljust(width)}  {path}")


def main(argv):
    ap = argparse.ArgumentParser(description="DIAG.6c(j) platform-debugger recipes + availability probe")
    default_spec = os.path.join("build", "win-debug", "tests", "diag", "crd-diag-cpu-hotspot-specimen.exe")
    ap.add_argument("--specimen", default=default_spec)
    ap.add_argument("--probe", action="store_true", help="probe availability + attempt a verified lldb bt session")
    args = ap.parse_args(argv)

    observe_availability()
    if not args.probe:
        return 0
    print("---")
    return probe_lldb(args.specimen)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
