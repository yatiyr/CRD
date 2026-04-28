# ADR-0004 — Containers v1

**Date:** 2026-04
**Status:** Accepted
**Tags:** [containers]

## Decision

- Allocator-aware via constructor argument, not template parameter.
- `Array<T>` grows 1.5x, initial capacity 8. Two push APIs: `push_back`
  (assert + grow / fatal on OOM) and `try_push_back` (false on refusal).
- HashMap: open-addressing + Robin Hood + backshift, no tombstones.
  `kMaxLoadFactor = 0.875`, power-of-two capacity, 2x growth.
- `String` SSO = 23 inline bytes (24-byte payload). `sizeof(String) = 32`.
- `String` ctors from `const char*` / `string_view` are explicit.
- Heterogeneous hash equality is mandatory for `HashMap<String, V>` lookup
  via `StringView` / `const char*`. `std::equal_to<>` is default.
- Iterators are std-compatible.
- `crd-containers` depends on `crd-core, crd-memory`. Link arrow is
  one-way: `crd-log → crd-containers`.
- RingBuffer v1 single-threaded, refuses on full. SPSC version arrives
  with `crd-jobs`.
- Force-link anchors must use `volatile int`, not `const int`.

## References

- `docs/phases/phase-1-foundations.md`
- `docs/containers/CONTAINERS_FILE.md`
