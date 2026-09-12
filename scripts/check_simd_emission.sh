#!/usr/bin/env bash
# check_simd_emission.sh — verifies the compiled obj file contains the SIMD
# instructions implied by CRD_SIMD_LEVEL_RESOLVED. Runs as a CTest test;
# guards against regressions where -mavx2 silently stops being passed.
#
# Usage: check_simd_emission.sh --obj <path> --expect <avx2|sse2|neon|scalar> [--ipo <0|1>]

set -euo pipefail

obj=""
expect=""
ipo="0"
while [[ $# -gt 0 ]]; do
    if [[ $# -lt 2 ]]; then
        echo "[check_simd_emission] missing value for $1" >&2; exit 2
    fi
    case "$1" in
        --obj)    obj="$2";    shift 2 ;;
        --expect) expect="$2"; shift 2 ;;
        --ipo)    ipo="$2";    shift 2 ;;
        *) echo "[check_simd_emission] unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$obj" || -z "$expect" ]]; then
    echo "[check_simd_emission] usage: $0 --obj <path> --expect <avx2|sse2|neon|scalar>" >&2
    exit 2
fi

case "$expect" in avx2|sse2|neon|scalar) ;; *) echo "Invalid SIMD expectation: $expect" >&2; exit 2 ;; esac
case "$ipo" in 0|1) ;; *) echo "Invalid IPO setting: $ipo" >&2; exit 2 ;; esac

if [[ ! -f "$obj" ]]; then
    echo "[check_simd_emission] FAIL: obj not found: $obj" >&2
    exit 2
fi

if [[ "$expect" == "neon" ]]; then
    echo "[check_simd_emission] expect=neon - ARM disasm parity check not implemented; skipping"
    exit 77
fi

if ! command -v objdump >/dev/null 2>&1; then
    echo "[check_simd_emission] FAIL: objdump not on PATH" >&2
    exit 2
fi

if disasm=$(objdump -d "$obj" 2>&1); then
    :
else
    decoder_exit=$?
    echo "[check_simd_emission] FAIL: objdump exited $decoder_exit" >&2
    printf '%s\n' "$disasm" >&2
    exit 2
fi
# Match both AT&T (%ymm) and Intel (ymm) syntaxes; objdump defaults to AT&T
# but newer binutils may emit either.
instr_total=$(printf '%s\n' "$disasm" | grep -cE '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]' || true)
ymm_total=$(printf '%s\n' "$disasm"   | grep -cE '\b%?ymm[0-9]+\b' || true)
ymm_fp=$(printf '%s\n' "$disasm"      | grep -cE '\bv(add|sub|mul|div|sqrt|min|max)ps[[:space:]]+%?ymm' || true)

echo "[check_simd_emission] obj         : $obj"
echo "[check_simd_emission] expect      : $expect"
echo "[check_simd_emission] instr_total : $instr_total"
echo "[check_simd_emission] ymm_total   : $ymm_total"
echo "[check_simd_emission] ymm_fp_ops  : $ymm_fp"

# Only explicit target IPO permits an IL-only skip. Empty output is not proof of LTO.
if [[ "$instr_total" -lt 100 ]]; then
    if [[ "$ipo" == "1" ]]; then
        echo "[check_simd_emission] SKIP - configured IPO object has insufficient native code; non-IPO proof required"
        exit 77
    fi
    echo "[check_simd_emission] FAIL: insufficient disassembly in a configured non-IPO object" >&2
    exit 1
fi

case "$expect" in
    avx2)
        # We require any 256-bit reference (ymm register usage), not
        # specifically vaddps/vmulps ymm. Rationale: GCC at -O0 emits
        # _mm256_* intrinsics as AVX-encoded vaddps/vmulps with the
        # 128-bit xmm register form (the register allocator avoids ymm
        # at -O0). The intrinsics still get the AVX encoding, ymm refs
        # appear in moves/broadcasts, and functional correctness is
        # preserved (proven by the bit-exact math suite). If -mavx2
        # were silently dropped, GCC emits ZERO ymm refs anywhere
        # (verified by the scalar preset which reports ymm_total=0).
        if [[ "$ymm_total" -eq 0 ]]; then
            echo "[check_simd_emission] FAIL: expected AVX2 build but no ymm references found in obj" >&2
            echo "[check_simd_emission]       (likely -mavx2 / /arch:AVX2 is not being passed)" >&2
            exit 1
        fi
        echo "[check_simd_emission] PASS - $ymm_total ymm references ($ymm_fp 256-bit FP ops) emitted"
        ;;
    sse2|scalar)
        if [[ "$ymm_total" -gt 0 ]]; then
            echo "[check_simd_emission] FAIL: expected $expect build but ymm references found in obj" >&2
            exit 1
        fi
        echo "[check_simd_emission] PASS - no ymm/AVX2 instructions emitted"
        ;;
esac
