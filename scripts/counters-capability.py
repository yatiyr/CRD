#!/usr/bin/env python3
# DIAG.6c(h): rich-counter capability probe + verdict machine (Windows ETW / xperf; Linux perf noted).
#
# Design clause (docs/design/runtime-diagnostics.md, "Where supported ..."): include context switches, page faults,
# cache/branch counters, lock contention and CPU/NUMA affinity; "record permissions, multiplexing and missing PMU
# events rather than treating unavailable counters as zero." This is a CAPABILITY machine, not a metric collector:
# for each counter class it records exactly one of
#   AVAILABLE            the provider/source exists on this box (a capture is possible given privilege)
#   MULTIPLEXING_REQUIRED more counters were requested than the hardware exposes concurrently
#   PERMISSION_DENIED    a capture was attempted but the shell lacks SeSystemProfilePrivilege
#   UNAVAILABLE          the provider/source is genuinely absent (reason recorded -- e.g. no PMU under a hypervisor)
#   TOOL_MISSING         xperf not installed
# It NEVER reports zero for an unavailable counter -- an absent counter is a named state with a reason.
#
# The capability enumeration (`xperf -pmcsources`, `xperf -providers KF`) is UNPRIVILEGED and runs fully here; the
# actual capture of any class needs an elevated shell, the same gate as scripts/sample-cpu-wpr.py. `--capture <class>`
# attempts the (elevation-gated) capture and records PERMISSION_DENIED honestly.
#
# xperf syntax verified against the INSTALLED help (do not recall):
#   xperf -pmcsources                       -> hardware PMU sources + "Maximum selectable profile sources: N"
#   xperf -providers KF                     -> kernel flags incl. CSWITCH, HARD_FAULTS, DISPATCHER
#   xperf -on <groups/flags> -PmcProfile C  -> sample on hardware counters (elevation-gated)
#   xperf -on <groups/flags> -Pmc C events  -> counting mode attributing counts to events (elevation-gated)
#
# Single-class verdict codes (--capture): AVAILABLE 0, TOOL_MISSING 2, PERMISSION_DENIED 3, UNAVAILABLE 4,
# MULTIPLEXING_REQUIRED 5. Default (report) mode prints every class and exits 0 (report produced) or 2 (no xperf).
#
# Usage:
#   python scripts/counters-capability.py                 # print the capability report for every class
#   python scripts/counters-capability.py --capture pmu   # attempt an (elevation-gated) capture of one class
import argparse
import os
import re
import shutil
import subprocess
import sys

# Counter classes and how each maps to an xperf mechanism.
#   kernel_flag: a single kernel provider flag; availability == the flag is listed by `xperf -providers KF`.
#   pmu:         hardware performance counters enumerated by `xperf -pmcsources`.
#   derived:     WPA derives it from other kernel events (no dedicated counter); availability == those flags exist.
CLASSES = {
    "context_switch":    ("kernel_flag", "CSWITCH"),
    "page_fault":        ("kernel_flag", "HARD_FAULTS"),
    "cache_branch_pmu":  ("pmu",         None),
    "lock_contention":   ("derived",     "CSWITCH"),      # derived from CSWITCH + DISPATCHER + ReadyThread
    "cpu_numa_affinity": ("derived",     "CSWITCH"),      # CPU per sample from CSWITCH; NUMA topology is static
}

# The `-on` flags a real capture of each class would use (kept next to the class so the runbook is self-documenting).
CAPTURE_FLAGS = {
    "context_switch":    "CSWITCH+PROC_THREAD+LOADER",
    "page_fault":        "HARD_FAULTS+PROC_THREAD+LOADER",
    "cache_branch_pmu":  "Latency",  # PROFILE-bearing group; -PmcProfile attaches the counters
    "lock_contention":   "CSWITCH+DISPATCHER+PROC_THREAD+LOADER",
    "cpu_numa_affinity": "CSWITCH+PROC_THREAD+LOADER",
}

PMU_KEYWORDS = ("Branch", "Cache", "LLC", "Instruction", "Cycle", "Issues", "Lbr")


def _which(name):
    return shutil.which(name)


def _run(xperf, args):
    return subprocess.run([xperf] + args, capture_output=True, text=True)


def parse_pmcsources(text):
    # Returns (max_concurrent, [source names]). Robust to the fixed-width table `xperf -pmcsources` prints.
    m = re.search(r"Maximum selectable profile sources:\s*(\d+)", text)
    max_concurrent = int(m.group(1)) if m else 0
    sources = []
    for line in text.splitlines():
        mm = re.match(r"\s*\d+\s+([A-Za-z]\w*)\s+\d+", line)  # "  10 CacheMisses  65536 4096 ..."
        if mm:
            sources.append(mm.group(1))
    return max_concurrent, sources


def pmu_concurrent_limit(max_concurrent):
    # `xperf -pmcsources` reports the total profile-source slots (e.g. 9). One is the timer, so `-PmcProfile` accepts
    # at most (N-1) hardware counters at once -- OBSERVED: xperf rejects a request for 9 with
    # "Too many counters specified (max=8)". Model the effective counter limit as N-1, not N.
    return max(0, max_concurrent - 1)


def classify_pmu(max_concurrent, sources):
    pmu = [s for s in sources if any(k in s for k in PMU_KEYWORDS)]
    limit = pmu_concurrent_limit(max_concurrent)
    if not pmu:
        return ("UNAVAILABLE", "no hardware PMU sources exposed (hypervisor/VM?)", pmu, limit)
    return ("AVAILABLE", f"{len(pmu)} cache/branch PMU sources, max {limit} concurrent counters (else multiplexing)",
            pmu, limit)


def classify_kernel_flag(kf_text, flag):
    if re.search(r"\b" + re.escape(flag) + r"\b", kf_text):
        return ("AVAILABLE", f"kernel flag {flag} present (capture needs SeSystemProfilePrivilege)")
    return ("UNAVAILABLE", f"kernel flag {flag} not listed by `xperf -providers KF`")


def plan_pmu_capture(requested, max_concurrent):
    # Pure decision: does a PMU request exceed the hardware's concurrent limit -> multiplexing, not zero.
    if max_concurrent <= 0:
        return ("UNAVAILABLE", 4)
    if requested > max_concurrent:
        return ("MULTIPLEXING_REQUIRED", 5)
    return ("AVAILABLE", 0)


def capability_report(xperf):
    pmc_text = _run(xperf, ["-pmcsources"]).stdout or ""
    kf_text = _run(xperf, ["-providers", "KF"]).stdout or ""
    max_concurrent, sources = parse_pmcsources(pmc_text)

    rows = []
    for name, (kind, mech) in CLASSES.items():
        if kind == "pmu":
            state, detail, _pmu, _max = classify_pmu(max_concurrent, sources)
        elif kind == "kernel_flag":
            state, detail = classify_kernel_flag(kf_text, mech)
        else:  # derived
            base, base_detail = classify_kernel_flag(kf_text, mech)
            if base == "AVAILABLE":
                state, detail = "AVAILABLE", f"derived from {mech}+DISPATCHER (WPA wait analysis); capture needs elevation"
            else:
                state, detail = base, base_detail
        rows.append((name, state, detail))
    return rows


def print_report(rows):
    width = max(len(n) for n, _, _ in rows)
    print("counter class capability (never zero -- absent is a named state):")
    for name, state, detail in rows:
        print(f"  {name.ljust(width)}  {state:<20} {detail}")


def capture_class(xperf, cls, out_dir, requested_pmu=None):
    if cls not in CLASSES:
        print(f"USAGE: unknown counter class {cls!r}; choose from {', '.join(CLASSES)}")
        return 64
    kind, mech = CLASSES[cls]
    on_flags = CAPTURE_FLAGS[cls]
    pmc_args = []

    if kind == "pmu":
        max_concurrent, sources = parse_pmcsources(_run(xperf, ["-pmcsources"]).stdout or "")
        pmu = [s for s in sources if any(k in s for k in PMU_KEYWORDS)]
        limit = pmu_concurrent_limit(max_concurrent)
        req = requested_pmu if requested_pmu is not None else min(len(pmu), max(1, limit))
        state, code = plan_pmu_capture(req, limit)
        if code == 4:
            print(f"UNAVAILABLE: {cls}: no hardware PMU sources exposed")
            return 4
        if code == 5:
            print(f"MULTIPLEXING_REQUIRED: {cls}: requested {req} > {limit} concurrent PMU counters "
                  f"(the hardware cannot program them all at once -- record this, do not zero the extras)")
            return 5
        pmc_args = ["-PmcProfile", ",".join(pmu[:req])]

    etl = os.path.join(out_dir, f"crd_counters_{cls}.etl")
    started = False
    try:
        start = _run(xperf, ["-on", on_flags] + pmc_args)
        if start.returncode != 0:
            msg = (start.stderr or start.stdout or "").strip().replace("\n", " ")
            low = msg.lower()
            # A counter-count limit is multiplexing, NOT a permission failure -- classify it honestly (xperf's own
            # English error text; not OS-localized): "Too many counters specified (max=N)".
            if "too many counters" in low or "max=" in low:
                print(f"MULTIPLEXING_REQUIRED: {cls}: {msg}")
                return 5
            print(f"PERMISSION_DENIED: {cls}: xperf -on failed (rc={start.returncode}): {msg}")
            print("  -> re-run from an elevated shell with SeSystemProfilePrivilege")
            return 3
        started = True
        _run(xperf, ["-stop", "-d", etl])  # (only reached with privilege) flush+merge the kernel session
        started = False
        print(f"AVAILABLE: {cls}: captured to {etl}")
        return 0
    finally:
        if started:
            _run(xperf, ["-stop"])  # never orphan a kernel logger


def main(argv):
    ap = argparse.ArgumentParser(description="DIAG.6c(h) rich-counter capability probe")
    ap.add_argument("--capture", metavar="CLASS", help="attempt an (elevation-gated) capture of one class: "
                                                       + ", ".join(CLASSES))
    ap.add_argument("--pmu-count", type=int, default=None, help="PMU sources to request (to exercise multiplexing)")
    ap.add_argument("--out", default=os.environ.get("TEMP", "."))
    args = ap.parse_args(argv)

    if sys.platform != "win32":
        # Linux equivalent is `perf list` (sources) + `perf stat -e ...`; perf_event_paranoid gates capture.
        print("UNAVAILABLE: this probe is Windows/xperf; on Linux use `perf list` + `perf stat` "
              "(perf_event_paranoid gates capture) -- documented runbook, no `perf` on this box")
        return 4

    xperf = _which("xperf")
    if xperf is None:
        print("TOOL_MISSING: xperf not on PATH (install the Windows Performance Toolkit)")
        return 2

    os.makedirs(args.out, exist_ok=True)
    if args.capture:
        return capture_class(xperf, args.capture, args.out, requested_pmu=args.pmu_count)
    print_report(capability_report(xperf))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
