# 2026-05-15 — Detour D-003 v0f: CPROF capture file format

## What shipped

The serialisation half of D-003: every piece of substrate state captured
since v0a (samples, counters, allocators, frame history, interned names)
now survives a write + read round-trip through a single byte buffer.
Two pieces:

### 1. CPROF v1 save

**`engine/perf/include/crd/perf/capture.hpp`** + **`src/capture.cpp`** (~430 LOC):

- FourCC `'CPRO'` (`kCprofMagic = 0x4F525043`) + version `1`.
- **POD on-disk structs** with hard-pinned sizes (static-asserted; bumping
  any of these means bumping `kCprofVersion`):
  - `CprofHeader`  72 B
  - `ThreadHeader` 56 B (per-thread sample count + name + byte offset
    into the buffer where this thread's `Sample[]` lives + dropped count)
  - `CounterMeta`  64 B (index + kind + type + 56-byte name)
  - `AllocatorMeta` 64 B (index + 56-byte name)
- **Layout** (little-endian; 8-byte aligned section boundaries):

  ```
  [CprofHeader]
  [ThreadHeader] x thread_count
  [CounterMeta]  x counter_count
  [AllocatorMeta] x allocator_count
  [NameBlob]                    -- u32 capacity + u32 string_bytes +
                                    u32 offsets[capacity] + char strings[]
  [FrameRecord] x frame_count   -- 3616 B each; memcpy verbatim
  [Sample]      x sum(thread sample counts) -- 32 B each; memcpy verbatim
  ```
- **NameBlob** preserves NameId stability across save/load: the offsets
  array is indexed by `Sample.name_id` (which is the live profiler's
  intern hash-table slot index, sparse). Empty slots get
  `0xFFFFFFFF`; filled slots point into the packed string storage with
  NUL-terminated names. `Sample.name_id` thus remains valid against
  the `CaptureView::resolve_name(...)` table after load.
- **`save_capture_to_buffer(IAllocator*)`** returns an
  `Array<u8>` (allocator-aware; engine code controls where capture
  storage lives).
- **`save_capture_to_file(path, IAllocator*)`** writes via `fopen_s`
  on MSVC (`fopen` elsewhere) + `fwrite`. Returns false on any I/O
  failure.
- **`validate_capture_buffer(ConstSpan<u8>)`** — cheap header + size
  check; rejects bogus magic / version / sizeof mismatches / undersized
  buffers.

### 2. Read-only `CaptureView`

**`engine/perf/include/crd/perf/capture_view.hpp`** + **`src/capture_view.cpp`** (~200 LOC):

`CaptureView` parses a buffer once at construction (just bookkeeps byte
offsets; no allocations) and provides accessors that **mirror the live
profiler's introspection API**:

| live profiler                       | CaptureView                       |
|-------------------------------------|-----------------------------------|
| `thread_count()`                    | `thread_count()`                  |
| `thread_samples(idx)`               | `thread_samples(idx)`             |
| `counter_count()` + `counter_info`  | `counter_count()` + `counter_info` |
| `registered_allocator_count()`      | `allocator_count()`               |
| `allocator_info(idx)`               | `allocator_info(idx)`             |
| `frame_record(N)` + `frame_record_count()` | `frame_records()`          |
| `resolve_name(NameId)`              | `resolve_name(NameId)`            |

The v0g UI panel code will target a common read-only interface so it
can render either a live profiler or a loaded `CaptureView` through
identical code -- replay, diffing, and regression checking all fall
out for free.

**Why a `CaptureView` instead of "load into the live profiler"?** The
per-thread sample rings are SPSC; writing loaded samples into thread
3's ring while thread 3 might still try to write is a data race. The
view-side approach decouples loaded state from live state cleanly. v0h
sandbox integration can drive the UI off either source via a
polymorphic adapter (planned for v0g).

### 3. Public introspection extensions

Added to `profiler.hpp` to support the capture writer:
- `intern_name_capacity()` — sparse hash-table size; the writer walks
  `[0, capacity)` and serializes each filled slot's name.
- `intern_name_count()` — count of filled slots.

`crd-perf` PUBLIC-links `crd-containers` (for `Array<u8>` / `ConstSpan<u8>`).

## Tests `tests/perf/test_capture_roundtrip.cpp` (7 cases / 47 assertions):

- `save_capture_to_buffer` produces a valid CPROF v1 buffer (magic,
  version, sizeof sanity checks, non-empty content).
- Full round-trip: scopes + counter + allocator + frame_mark + save +
  `CaptureView`-load + verify thread samples (names round-trip), counter
  info (name + kind + type), allocator info (name), frame_record
  contents (counter values + allocator byte snapshots).
- `validate_capture_buffer` rejects too-short / bad-magic / bad-version
  inputs.
- `CaptureView` on a bogus buffer is invalid; accessors return safe
  defaults; no crash.
- Interned names round-trip via the name blob (three names of varying
  length resolve to the originals after load).
- `save_capture_to_buffer` on inactive profiler returns empty buffer.
- Header struct sizes pinned via `STATIC_REQUIRE`.

## Design decisions locked at v0f

1. **One-way save (no in-place load).** A loaded capture lives in a
   `CaptureView`, never overwrites live profiler state. Side-steps the
   SPSC-ring race that would otherwise make loading unsafe.
2. **NameId preservation.** The on-disk name blob indexes by the
   live intern-table slot (sparse hash-table index), so `Sample.name_id`
   in the saved buffer resolves identically post-load.
3. **Pinned POD layouts; static-asserted struct sizes.** Format
   evolution rule: any layout change bumps `kCprofVersion` and the
   reader rejects mismatched versions in `validate_capture_buffer`.
4. **Little-endian only.** All Cerid targets are little-endian; cross-
   arch big-endian support is YAGNI.
5. **Capture writer is thread-safe enough for "main-thread between
   frames" use.** Each per-thread ring is read under acquire-load of
   head/tail (matches `thread_samples` accessor). Concurrent
   push_region during save may or may not appear in the buffer --
   well-defined; not guaranteed.
6. **Allocator-aware buffer storage.** `save_capture_to_buffer` takes
   an `IAllocator*`. Engine code controls where capture bytes live;
   tests use a local `MallocAllocator`.

## Verification (5-config per-slice DoD)

| Config | Result |
|---|---|
| win-debug | **PASS** (1830/1830 full project ctest; 83/83 perf cases — 280 assertions) |
| win-asan | **PASS** (83/83 perf cases — 280 assertions) |
| win-shipping | **PASS** (6/6 perf cases — 23 assertions; 77 gated cases compile out at gate) |
| win-shipping-profile | **PASS** (83/83 perf cases — 280 assertions under LTCG + max optimization) |
| win-tidy | **PASS** (build clean) |

## Issues encountered + fixed

1. **MSVC C4996** `fopen` deprecation → use `fopen_s` on MSVC,
   `std::fopen` elsewhere.
2. **MSVC C4189** in `win-shipping-profile`: `name_blob_start`
   local used only in a debug-mode `CRD_ASSERT_MSG`; marked
   `[[maybe_unused]]`.

## What unlocks now

- **v0g** ImGui frontend has its full data model: live profiler
  introspection + loaded `CaptureView` are interchangeable from the
  UI panel's perspective. Replay / save / load / diff workflows fall
  out for free.
- File-based capture sharing across machines: a QA build under
  `win-shipping-profile` can dump a CPROF blob; a dev box can load it
  and inspect.
- The substrate side of D-003 is **complete**. Everything below the
  UI layer is in place: capture, scope, counters, jobs auto-
  instrumentation, GPU timing, memory tracking, file format.

## Next

**v0g — ImGui frontend.** Separate target `crd-perf-ui` (so the
substrate stays ImGui-free and the frontend is swappable). Panels:
Frame Summary, Timeline (zoom/pan per-thread + GPU track), Flame Graph,
Counters (line plots), GPU Passes, Memory (allocator table + plots),
Capture controls (record/stop/save/load + recent-captures dropdown).
Renders either the live profiler or a `CaptureView` through a single
common source interface. **The heart of the UX.** ~1500 LOC engine +
~300 LOC tests, 3-5 days.
