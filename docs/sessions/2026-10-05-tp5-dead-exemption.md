# TP-5: the never-matching mikktspace UBSan exemption removed

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3d](../ROADMAP.md#slice-repo.3d); register:
> [TP-5](../third-party-defects.md#tp-5). Rules: [AGENTS](../../AGENTS.md).

## Finding

While triaging `linux-gcc-asan` on `1ab3b1ef`, the mikktspace oracle failure turned out to be the registered TP-5
(UBSan `shift exponent 32` at `mikktspace.c:1667`). The register gate expects it, and it already failed the same way on
`dde01ed0` while that lane passed.

`tests/assets/asset-io/CMakeLists.txt` also carried a compile option meant to silence the same finding:

```cmake
COMPILE_OPTIONS "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<CONFIG:Asan>>:-fno-sanitize=shift-exponent>"
```

It never applied. The `linux-gcc-asan` preset builds `CMAKE_BUILD_TYPE=Debug` with `CRD_ENABLE_ASAN` and
`CRD_ENABLE_UBSAN`, so there is no configuration named `Asan`. Its comment pointed to "the cooker's `mesh.cpp`", which
GEO-6 deleted.

## Decision

TP-5 stays the single mechanism. The oracle is the unmodified upstream file and the sanitizer finding stays visible
and registered. An exemption that worked would hide it and make the register's retirement condition (the gate fails on
an unexpected pass) unreachable.

## Change

- The dead genex and its comment are removed. The new comment names TP-5.
- Two stale comments now point to the real include site, `test_condition.cpp`: the root `CMakeLists.txt` MikkTSpace
  block and the include line in `test_condition.cpp`. The first also said the package served the retired glTF cook
  handler.
- No behaviour changes on any lane: the removed option never reached a compile command.
