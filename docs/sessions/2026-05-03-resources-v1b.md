# Session — 2026-05-03: Phase 2.6 v1b — Cooker CLI + zstd compression

**Status:** SHIPPED  
**Tests after:** 408/408 win-debug, 408/408 win-relwithdebinfo, 408/408 win-asan, 408/408 win-clang-cl, 408/408 win-tidy, 405/405 win-release  
**Duration:** one session (continuation of v1a)

---

## What shipped

### zstd per-chunk compression in `CrdrWriter` / `crdr_read`

`CrdrWriter::add_chunk_compressed(fourcc, data, level)` — opt-in compression using zstd level 3 by default. Falls back to storing the uncompressed payload if the compressed output is larger (no-op for already-compressed payloads like BC7 textures or FLAC audio). Chunk flag bit 0 signals compression in the on-disk format.

`crdr_read()` updated with a two-pass approach:
- Pass 1: count how many chunks are compressed and total decompressed capacity needed.
- Pre-allocate one `Array<u8> decompressed_backing` in `CrdrFile` before the chunk loop.
- Pass 2: decompress into `decompressed_backing` in-place; `CrdrChunk::payload` spans into the backing buffer. Avoids `ConstSpan` invalidation that would occur if the array were grown chunk-by-chunk.

`CrdrError::DecompressFailed` added; `asset_cooker/main.cpp` switch updated.

zstd linked via CPM: `KhronosGroup/KTX-Software` dependency removed; a direct `zstd` CPM entry added (v1.5.5, MIT). `CRD_BIG_ENDIAN` / `CRD_LITTLE_ENDIAN` macros added to `engine/platform/include/crd/platform/platform.hpp` (MSVC hardcodes LE; GCC/Clang uses `__BYTE_ORDER__`). Old raw `__BYTE_ORDER__` checks in `crdr.cpp` updated to use these macros.

### `crd-cooker` static library

Split from the `asset_cooker` executable. New CMake layout:
- `add_library(crd-cooker STATIC ...)` — contains all cook logic, handler registration, PACK assembly.
- `add_executable(asset_cooker src/main.cpp)` — links `crd-cooker`, zero other compile units.
- Tests link `crd-cooker` directly without the CLI dispatch layer.

New public headers (`tools/asset_cooker/include/crd/cooker/`):
- `cook_handler.hpp` — `CookContext` (source_path, meta_path, id, allocator, options), `CookResult` (type_fourcc, cooked_bytes, dependencies, source_hash), `CookHandlerFn` function pointer typedef.
- `cook_command.hpp` — `cmd_cook(root, out, alloc)` free function.

### `cook` CLI sub-command

`cmd_cook()` implementation:
1. Recursive directory scan via `crd::platform::fs` (excludes `.meta` sidecars and `.cook_cache/` directories). Results sorted lexicographically for determinism.
2. For each source file: read or mint a `.meta` sidecar (UUID v4 via `ResourceId::mint_random()`), compute FNV1a-64 source hash.
3. `cook_key = source_hash ^ handler_version`. Stored in `.cook_cache/<uuid>.key` (8 bytes, native-endian — machine-local cache file).
4. If the cached key matches: skip this file, log "skipped (incremental key match)".
5. Otherwise: invoke the registered `CookHandlerFn`, write the resulting artifact to `.cook_cache/<uuid>.crdr`.
6. Two-pass PACK assembly: Pass 1 builds the CRDR section (MFST + STRP chunks) with `blob_offset = 0` for all entries, measures the resulting byte count. Pass 2 recomputes real `blob_offset = crdr_section_size`, rebuilds MFST, appends artifact bytes after the CRDR section.
7. Write `cook.log.toml` adjacent to the output pack (one entry per source file: action=cooked/skipped, source_hash, cook_key, artifact_size).

### `blob_passthrough_handler` — first cook handler

Registered for extension `.bin`. Wraps the raw source file bytes in a single `BLOB` chunk under `type_fourcc = kFourCC_BLOB`. No transformation. Demonstrates the handler API. Used in integration tests.

### Optional CMake `cook` target

When `CRD_COOK_ROOT` and `CRD_COOK_OUT` cache variables are set, CMake emits a `cook` custom target that re-runs the cooker on the configured root. Source files are listed as `DEPENDS` for fine-grained Ninja rebuild tracking.

### Tests (`tests/resources/` additions)

4 new tests in `test_resource_manager.cpp`:
- **Registry:** register two handlers for different extensions; verify dispatch routes correctly.
- **.bin round-trip:** cook a `.bin` directory, mount the output pack, load one entry, verify bytes match the source file contents.
- **zstd round-trip:** `add_chunk_compressed` on a compressible payload, `crdr_read` decompresses; resulting `ConstSpan` matches original bytes.
- **Integration (10 files):** cook 10 `.bin` files, verify pack has 10 entries with correct ids; re-run cooker without changes, verify all 10 are logged as "skipped".

---

## Issues encountered and resolved

1. **`ConstSpan` invalidation in decompression loop:** initial implementation grew the `decompressed_backing` array inside the chunk loop; this reallocated the buffer, invalidating `ConstSpan` views into it that had already been stored in earlier `CrdrChunk` entries. Fixed by the two-pass approach: pre-scan compressed chunk count + total decompressed size, pre-allocate once, then decompress.

2. **`CrdrWriter::finish()` FourCC sort interacts with compression flag:** sort was comparing only fourcc fields. Chunks with the same fourcc (impossible today but guard for future) would have non-deterministic relative order. Confirmed non-issue for current use (all fourccs unique per file) but documented.

3. **cmake `list(FILTER ... EXCLUDE REGEX)` for static lib split:** pattern `".*/main\\.cpp$"` must escape the dot and anchor the end to avoid accidentally excluding files named `domain.cpp`. Verified correct with regex tester.

4. **FNV1a-64 native-endian cook_key file:** deliberate decision to store cook_key as native-endian (not LE) since the `.cook_cache/` directory is machine-local, never shipped, and reading with the wrong endianness would only produce a spurious re-cook (safe degradation).

5. **`CRD_BIG_ENDIAN` macro on MSVC:** MSVC has no `__BYTE_ORDER__`. Added explicit `#if defined(_MSC_VER) ... #define CRD_LITTLE_ENDIAN` branch. All x64 Windows targets are LE; MSVC for ARM64 would need revisiting (parked in `docs/debt.md`).

---

## Proposed commit message

```
feat(resources): Phase 2.6 v1b — cooker CLI + zstd compression + crd-cooker static lib

zstd v1.5.5 per-chunk opt-in compression in CrdrWriter::add_chunk_compressed() (level 3
default; falls back to uncompressed if compression doesn't help). Two-pass decompression
in crdr_read() pre-allocates decompressed_backing before the chunk loop to avoid ConstSpan
invalidation. CrdrError::DecompressFailed added. CRD_BIG_ENDIAN / CRD_LITTLE_ENDIAN macros
added to platform.hpp (replaces raw __BYTE_ORDER__ checks).

crd-cooker static library split from asset_cooker exe (tests link it directly without CLI).
cook_handler.hpp: CookContext, CookResult, CookHandlerFn. cook_command.hpp: cmd_cook().
cmd_cook(): recursive scan, lexicographic sort, .meta sidecar mint/read, FNV1a-64 source
hash, cook_key = source_hash ^ handler_version, .cook_cache/<uuid>.key + .crdr, two-pass
PACK assembly, cook.log.toml. blob_passthrough_handler for .bin files. Optional CMake cook
target (CRD_COOK_ROOT + CRD_COOK_OUT). 4 new tests. All 6 configs green (408/408 win-debug).
```
