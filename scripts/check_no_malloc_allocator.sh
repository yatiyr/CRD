#!/usr/bin/env bash
# check_no_malloc_allocator.sh — Linux twin of check_no_malloc_allocator.ps1.
# Bans crd::memory::MallocAllocator outside the memory module's own definition +
# the allocator stress/unit tests that legitimately exercise it. Project rule
# (2026-05-27): no MallocAllocator as a working allocator; use TlsfAllocator /
# GrowableTlsfAllocator. See memory/project_no_malloc_sweep_before_v5.
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

hits=$(grep -rn "MallocAllocator" "$ROOT/engine" "$ROOT/tests" "$ROOT/runtime" \
        --include='*.cpp' --include='*.hpp' --include='*.h' 2>/dev/null \
      | grep -vE "/engine/memory/" \
      | grep -vE "/tests/memory/test_memory\.cpp|/tests/stress/test_allocators_stress\.cpp|/tests/stress/test_allocators_v5_stress\.cpp" \
      | grep -v "crd-lint-allow-malloc-allocator" \
      | awk -F: '{ code=$0; sub(/^[^:]*:[^:]*:/,"",code); t=code; sub(/^[ \t]+/,"",t); if (t ~ /^\/\// || t ~ /^\*/) next; print }' \
      || true)

if [ -n "$hits" ]; then
    echo "[check_no_malloc_allocator] FAIL: MallocAllocator reference(s) outside allowed scopes:"
    echo "$hits"
    echo ""
    echo "  Use crd::memory::TlsfAllocator (bounded) or GrowableTlsfAllocator (unbounded) instead."
    echo "  default_allocator() is also discouraged outside engine/memory. Marker: crd-lint-allow-malloc-allocator."
    exit 1
fi

echo "[check_no_malloc_allocator] PASS - no MallocAllocator use outside engine/memory + allocator tests"
exit 0
