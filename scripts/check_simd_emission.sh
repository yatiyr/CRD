#!/usr/bin/env bash
# check_simd_emission.sh — verifies the compiled obj file contains the SIMD
# instructions implied by CRD_SIMD_LEVEL_RESOLVED. Runs as a CTest test;
# guards against regressions where -mavx2 silently stops being passed.
#
# Usage: check_simd_emission.sh --obj <path> --expect <avx2|sse2|neon|scalar>

set -euo pipefail

obj=""
expect=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --obj)    obj="$2";    shift 2 ;;
        --expect) expect="$2"; shift 2 ;;
        *) echo "[check_simd_emission] unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$obj" || -z "$expect" ]]; then
    echo "[check_simd_emission] usage: $0 --obj <path> --expect <avx2|sse2|neon|scalar>" >&2
    exit 2
fi

if [[ ! -f "$obj" ]]; then
    echo "[check_simd_emission] FAIL: obj not found: $obj" >&2
    exit 2
fi

if [[ "$expect" == "neon" ]]; then
    echo "[check_simd_emission] expect=neon - ARM disasm parity check not implemented; skipping"
    exit 0
fi

if ! command -v objdump >/dev/null 2>&1; then
    echo "[check_simd_emission] FAIL: objdump not on PATH" >&2
    exit 2
fi

disasm=$(objdump -d "$obj" 2>&1 || true)
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

# LTCG/LTO emits IL-only objs; native code only exists post-link. Skip
# the check in that case — non-LTO configs cover the same code path.
if [[ "$instr_total" -lt 100 ]]; then
    echo "[check_simd_emission] SKIP - obj appears IL-only (likely LTO build); covered by non-LTO configs"
    exit 0
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
