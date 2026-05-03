# Session — 2026-05-03: Phase 2.6 v1a — `crd-resources` + `asset_cooker manifest_dump`

**Status:** SHIPPED  
**Tests after:** 393/393 win-debug, 393/393 win-asan, 393/393 win-clang-cl, 393/393 win-tidy, 393/393 win-relwithdebinfo, 390/390 win-release  
**Duration:** one session (continuation of prior architecture session)

---

## What shipped

### `crd-resources` module (`engine/resources/`)

**`ResourceId`** (`resource_id.hpp` + `resource_id.cpp`):
- 128-bit UUID stored as two `u64` fields (`hi` bytes 0–7, `lo` bytes 8–15, big-endian within each field).
- `mint_random()`: UUID v4 via `thread_local std::mt19937_64` seeded from `std::random_device`. Sets version nibble at byte[6] = 4, RFC 4122 variant at byte[8] top 2 bits = 0b10.
- `from_content(span)`: UUID v5 via SHA-1(Cerid-namespace-UUID || content). Version nibble = 5, same variant bits. Namespace UUID = `1e57ab3c-f2a4-5f62-a012-3b4c5d6e7f80`. SHA-1 vendored in `src/detail/sha1.hpp`; computes over two-part message without heap allocation, handles 64-byte block boundary.
- `parse(text)`: validates 36-char hyphenated format, rejects bad length/dash-positions/non-hex. Returns `kNullResourceId` on any error.
- `to_string(alloc)`: outputs `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (36 chars, lowercase hex).
- `DefaultHash<ResourceId>` specialization in `crd::containers` namespace using `hash_u64` with golden-ratio XOR mixing.

**CRDR chunked binary container** (`crdr.hpp` + `crdr.cpp`):
- 32-byte header: magic `'CRDR'` (LE), version u16, flags u16, uuid_hi u64, uuid_lo u64, type_fourcc u32, chunk_count u32.
- Each chunk: fourcc u32, flags u32, uncompressed_size u32, payload_size u32, reserved u64 + payload (zero-padded to 16-byte alignment).
- `crdr_read()` validates magic (`kFourCC_CRDR`), version (==1), chunk sizes, builds `CrdrFile` with `ConstSpan` views into source bytes.
- `CrdrWriter::finish()` sorts chunks ascending by FourCC for determinism, assembles binary with LE writes.
- FourCCs registered: CRDR (container), PACK (manifest), BLOB, MFST, STRP, DEPS, META, SHDR, MATR, AUDO.

**Manifest format** (`crdr.hpp`):
- `ManifestEntry`: 48 bytes on disk (hi 8B, lo 8B, type_fourcc 4B, flags 4B, blob_offset 8B, blob_size 8B, name_strp_idx 4B, reserved 4B).
- `manifest_write(writer, entries, string_pool)`: encodes MFST + STRP + DEPS chunks.
- `manifest_read_entries(payload, entries, alloc)`: validates size alignment, decodes.

**`ILoader` + `LoadContext`** (`loader.hpp`):
- `LoadContext`: id, bytes span, manager pointer, allocator.
- `ILoader`: pure virtual `type_fourcc()`, `loader_version()`, `load(ctx)`, `load_placeholder(ctx)` (default: returns nullptr), `unload(ptr)`.

**`ResourceManager` shell** (`resource_manager.hpp` + `resource_manager.cpp`):
- `register_loader(unique_ptr<ILoader>)`: asserts non-null, no duplicate FourCC.
- `mount_manifest(path)`: reads CRDR PACK via `crd::platform::fs::read_file_binary`, validates, reads MFST+STRP, populates `m_live` HashMap. Newest-mount-wins collision: `erase(id)` then `insert(id, live)`. Logs collision at `Warn`. Returns `MountId{0}` on any failure.
- `unmount(MountId)`: removes live entries owned by that mount. Uses `swap_remove` on mount list.
- `find_entry(ResourceId)`: O(1) HashMap lookup.
- Diagnostics: `loader_count()`, `mount_count()`, `entry_count()`.
- Log channel `g_log_resources` defined in `engine/resources/src/log_channel.cpp` (NOT in crd-log to avoid circular dependency).

### `tools/asset_cooker/` (`tools/asset_cooker/`)

`manifest_dump` sub-command: reads a CRDR PACK file, validates type, reads MFST+STRP, prints all entries with UUID (36-char), type FourCC, blob offset+size, debug name from string pool. Prints `(unnamed)` for entries with no valid name offset.

### Tests (`tests/resources/`)

38 new tests across three files:
- `test_resource_id.cpp` (15 tests): mint_random non-null, 10K uniqueness, v4 version+variant bits, from_content determinism, from_content different bytes, from_content v5 bits, empty span stable, parse+to_string 1000-round-trip, 36-char format, parse rejects wrong length/dash-positions/non-hex, parse known stable value, null id, equality reflexive/copy.
- `test_crdr.cpp` (11 tests): empty span rejects, bad magic, bad version, truncated, round-trip empty, single BLOB chunk, multiple chunks sorted, 16-byte padding transparent, manifest 100-entry round-trip, crdr_find_chunk nullptr, crdr_find_chunk finds.
- `test_resource_manager.cpp` (6 tests): register loader, mount+unmount (writes real CRDR PACK to disk), mount missing file → invalid MountId, collision newest-wins, unmount invalid no-op, find_entry returns nullptr.

CMakeLists uses `Catch2::Catch2WithMain` (resources tests have no custom main, unlike jobs).

---

## Bug fixed (non-Phase-2.6): String SSO remaining-capacity encoding

The `crd-containers String` implementation had a latent UB that manifested as a test regression when the project was built with the newly installed MSVC 14.50.35717. The `win-release` build was failing "String: small construction stays in SSO at boundary 23".

**Root cause:** `init_from()` wrote `m_small.buf[n] = '\0'` when n == kSsoCapacity (23). `buf` is `char buf[23]` (indices 0–22), so `buf[23]` is one past the end — UB. The new MSVC optimizer, seeing this UB, eliminated the entire SSO branch (`n <= kSsoCapacity`) for n=23, causing the heap path to run instead. Result: `is_small()` returned false.

Additionally, for a 23-char SSO string with the old encoding (`size_or_flag = size`), `size_or_flag = 23 ≠ '\0'`, so `c_str()` would return a non-null-terminated char array — a second latent bug.

**Fix:** switched to remaining-capacity encoding: `size_or_flag = kSsoCapacity - size` in SSO mode.
- When size = 0: `size_or_flag = 23` (still ≠ 0xFF, so `sso_state()` remains correct).
- When size = 23 (full): `size_or_flag = 0 = '\0'`, which IS the null terminator at byte 23.
- `c_str()` now returns a correctly null-terminated buffer for all SSO string lengths.
- All `buf[n] = '\0'` writes guarded with `if (n < kSsoCapacity)` to avoid the UB.
- `push_back`, `append`, `resize`, `shrink_to_fit` all updated with the same guard.
- `size()` updated: `kSsoCapacity - static_cast<usize>(m_small.size_or_flag)`.
- `clear()`, `init_empty()`, `set_size()`: `size_or_flag = kSsoCapacity - n`.

No API change. Static assert `sizeof(String) == 32` still passes.

---

## Issues encountered and resolved

1. **`insert_or_assign` missing from HashMap**: `HashMap` only has `insert()` (no overwrite), `erase()`. Fixed by `erase(id)` then `insert(id, live)` for collision handling.
2. **`crd/log/manager.hpp` included in cooker**: No such header. Removed; cooker uses `printf` directly.
3. **`String::operator[]` missing**: Test `str[8] == '-'` → changed to `str.data()[8] == '-'`.
4. **`[[nodiscard]]` on `remove_file`**: Wrapped with `(void)` to suppress C4834.
5. **Missing `main` in test exe**: `Catch2::Catch2` (no main) linked instead of `Catch2::Catch2WithMain`. Fixed in CMakeLists.
6. **Log channel circular dependency**: `g_log_resources` would create crd-log → crd-resources header dependency. Fixed by defining in `engine/resources/src/log_channel.cpp`.
7. **CTest Unicode filter failure**: Test name with `→` (U+2192) corrupted in Windows command-line filter. Changed to ASCII `->`.
8. **MSVC toolchain update breaks cmake**: New MSVC 14.50.35717 installed; needed to reconfigure cmake with vcvars64.bat. String SSO regression discovered and fixed.

---

## Proposed commit message

```
feat(resources): ship Phase 2.6 v1a — ResourceId, CRDR container, ResourceManager shell, manifest_dump

fix(containers): String SSO remaining-capacity encoding eliminates buf[kSsoCapacity] UB

ResourceId: UUID v4 (mt19937_64), UUID v5 (SHA-1 + Cerid namespace), parse/to_string 36-char.
CRDR: 32-byte header + sorted 16-byte-aligned chunks, LE serialization, reader + writer.
ManifestEntry: 48-byte disk format, MFST/STRP/DEPS chunks, manifest_write/read_entries.
ResourceManager: register_loader, mount_manifest (newest-wins collision), unmount, find_entry.
asset_cooker: manifest_dump sub-command reads CRDR PACK and prints all entries.
38 new tests. All 6 configs green (393/393 debug, 390/390 release).

String fix: size_or_flag now stores kSsoCapacity-size (remaining capacity). A 23-char SSO
string has size_or_flag=0='\0' which doubles as the null terminator, avoiding out-of-bounds
UB exposed by MSVC 14.50.35717 optimizer in release builds.
```
