#!/bin/bash
# Repro + forensics harness for the 2026-07-02 CI moat-test hang (the
# std::counting_semaphore lost wake — see docs/sessions/2026-07-02-jobs-
# semaphore-lost-wake.md). Mimics the 4-vCPU CI runner via taskset; on a
# hang: CPU-climb check (timeout-is-not-a-hang-proof), gdb stacks, and the
# futex word each thread sleeps on. Pre-fix it hung by iteration <= 76;
# post-fix 300/300 clean. Rerun after any crd-jobs sleep/wake change.
# Usage: bash scripts/repro_moat_hang.sh [iters=300] [timeout_s=90]
#   (gdb attach needs: wsl -u root: echo 0 > /proc/sys/kernel/yama/ptrace_scope)
set -u
OPT="$HOME/cerid-build/linux-gcc-release/tests/hesap-opt/crd-hesap-opt-tests"
EIG="$HOME/cerid-build/linux-gcc-release/tests/hesap-eigen/crd-hesap-eigen-tests"
LOG=/tmp/moat_hang_repro.log
STACKS=/tmp/moat_hang_stacks.txt
ITERS=${1:-300}
TIMEOUT_S=${2:-90}

: > "$LOG"
echo "[repro] ptrace_scope=$(cat /proc/sys/kernel/yama/ptrace_scope)" | tee -a "$LOG"

check_and_dump() {
    local pid=$1 label=$2 iter=$3
    if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
    local t1 t2
    t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    sleep 5
    t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    echo "[repro] HANG iter=$iter bin=$label pid=$pid cpu_ticks_5s=$((t2-t1))" | tee -a "$LOG"
    {
        echo "===== HANG iter=$iter bin=$label pid=$pid cpu_ticks_over_5s=$((t2-t1)) ====="
        for t in /proc/$pid/task/*; do
            echo "tid=$(basename $t) state=$(awk '{print $3}' $t/stat) wchan=$(cat $t/wchan 2>/dev/null)"
            echo "  syscall: $(cat $t/syscall 2>/dev/null)"
        done
        gdb -p "$pid" -batch -ex 'set pagination off' -ex 'thread apply all bt' 2>&1
        for t in /proc/$pid/task/*; do
            tid=$(basename $t)
            [ "$tid" = "$pid" ] && continue
            uaddr=$(awk '{print $2}' $t/syscall 2>/dev/null)
            if [ -n "$uaddr" ]; then
                echo "--- futex word at $uaddr (tid $tid) ---"
                gdb -p "$pid" -batch -ex "x/2dw $uaddr" 2>&1 | tail -2
            fi
        done
    } >> "$STACKS"
    kill -9 "$pid" 2>/dev/null
    return 1
}

for i in $(seq 1 "$ITERS"); do
    taskset -c 0-3 "$OPT" "[moat]" --reporter compact > /tmp/moat_opt.txt 2>&1 &
    P_OPT=$!
    taskset -c 0-3 "$EIG" "[moat]" --reporter compact > /tmp/moat_eig.txt 2>&1 &
    P_EIG=$!
    for s in $(seq 1 "$TIMEOUT_S"); do
        sleep 1
        kill -0 $P_OPT 2>/dev/null || kill -0 $P_EIG 2>/dev/null || break
    done
    hung=0
    check_and_dump $P_OPT opt "$i" || hung=1
    check_and_dump $P_EIG eig "$i" || hung=1
    if [ $hung -eq 1 ]; then
        echo "[repro] stacks written to $STACKS" | tee -a "$LOG"
        exit 42
    fi
    wait $P_OPT; rc_opt=$?
    wait $P_EIG; rc_eig=$?
    if [ $rc_opt -ne 0 ] || [ $rc_eig -ne 0 ]; then
        echo "[repro] iter=$i non-hang failure (opt=$rc_opt eig=$rc_eig)" | tee -a "$LOG"
        exit 43
    fi
    if [ $((i % 10)) -eq 0 ]; then echo "[repro] iter=$i ok" | tee -a "$LOG"; fi
done
echo "[repro] NO REPRO in $ITERS iters" | tee -a "$LOG"
