# 2026-11-01 — OWN.1: the bundle's compressed sections and crd-compression in the package

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the first remaining item after the
[one ZIP container](2026-11-01-own-one-zip-container.md). Built, tested and measured on `cerid-track-b`. The
contracts are [capability ownership](../design/capability-ownership.md#the-bundles-compressed-sections) (decision 1),
[DIAG.5d](../design/runtime-diagnostics.md#diag-5d) and the
[relocatable package](../design/public-consumption.md#the-diagnostic-family-diag11a).

## CI for the previous batches

- Run 41247045494 (67977fda, through batch 5) finished. Its nine failed jobs (win-debug, win-debug-scalar, win-asan,
  win-public-checks, win-release, win-relwithdebinfo, win-shipping, win-shipping-profile, win-tidy) stop at one unit
  each, `tests/tools/ceridc/test_ceridc_screen.cpp`, MSVC C2797 (track C's screen test, fixed on main by d9be0cfd).
  win-tidy builds keep-going and failed on that unit alone. Every other job passed, the clang-cl, shipping, Linux,
  ARM64 and sanitizer lanes among them, so batches 1 to 5 are clean on every hosted lane but the screen test's.
- Run 41254049879 (31404ec4) is the first past that wall and covers batch 6 (9cdebf27, ff1c2b3b, 05a133c4). Its
  finished jobs pass: win-clang-cl-shipping (the constexpr fixed trees under clang-cl's shipping build), win-vs,
  win-arm64-debug, linux-gcc-arm64-debug, linux-clang-arm64-debug and both repository checks. The MSVC lanes were still
  running. No run has started a job for batch 7 (00b9dc88, c29c5844) yet: every later run was cancelled by a newer push.
- No failure traces to this track.

## Design

### The format stays v1

`kSectionFlagCompressed` has meant "the payload is raw DEFLATE" since format v1; its reader refused it as
`UnsupportedFeature` until a codec edge was added. Producing it changes no layout and no field's meaning, so the format
is not bumped. A bump would make every older reader refuse the whole bundle. Without it, an older reader refuses the
compressed section, as it was built to, and keeps the prefix.

### Writing

- `BundleWriter` takes a `BundleCompression`; the default is `Deflate`, and `None` keeps every section raw.
- A section kept whole is stored as crd-compression's raw DEFLATE when that is smaller. A section DEFLATE does not
  shrink is stored raw, the ZIP container's rule. A compressed section's `original_length` is its decoded size, and
  `payload_crc32` covers the stored, compressed bytes.
- A section cut to a cap is stored raw. Compressed and Truncated never meet, so `original_length` keeps one meaning per
  flag.
- The caps bound the decoded bundle, which is what a reader holds. Room is counted in decoded bytes
  (`decoded_bytes()`), so compression shrinks the file and admits no more content than before; `total_bytes()` is the
  file's size.

### Reading: the decompression-bomb defence

`read_bundle(buf, limits, alloc)` walks the stream first and decodes after. A compressed claim is bounded before
anything is allocated for it:

- A claim past what DEFLATE can reach from the stored bytes (`deflate_max_inflated_size`, 1032 bytes per byte) is a
  lying header, and Compressed with Truncated is a section the writer never makes. Either stops the read with
  `RefusedCompressed` (it was `UnsupportedFeature`) and keeps the prefix.
- A claim past `max_section_bytes`, or one that takes the decoded bundle past `max_total_bytes`, is listed undecoded:
  an empty payload, `original_length` its claim. The walk goes on, as it does past a raw section over the importer's
  cap, and the importer flags it `kImportSectionOversized`. An importer with smaller caps than the writer's therefore
  loses one section, not the rest of the bundle.
- The sections that passed decode into one allocation of exactly their claims (`BundleReadResult::decoded`, reserved
  through the containers' `try_` path), each with the exact-size `inflate_raw`. A stream that does not decode to its
  claim, or a refused allocation, ends the prefix there with `RefusedCompressed`.
- Views stay valid when the result moves. The importer moves the decoded bytes into `BundleImport::decoded`.
- Both views carry `stored_length`. `bundle.inspect` reports it per section, its summary says `compressed_refused`
  (it said `compressed_unsupported`), and it reads a flight record or crash capture from a compressed section like any
  other (it refused one before).

### The package

crd-perf links crd-compression privately, so the relocatable package needs `Cerid::compression` and zstd's archive.

- `cmake/CrdPackage.cmake` adds `crd_package_dependency(<target> NAME <name>)`: a pinned third-party static library
  exported link-only, with its compile PDB under `pdb` and no header. zstd's header is private to crd-compression, so a
  consumer reaches zstd only through `zstd.hpp` and cannot swap in another build. `CeridConfig.cmake` lists it in
  `Cerid_DEPENDENCIES`. The archive and PDB rules of both functions are one helper, which reads the compile PDB from the
  directory that defined the target (it had assumed the calling directory).
- zstd is added `EXCLUDE_FROM_ALL`. A prototype on the machine (CMake 4.4.5, the pinned zstd added the same way, a
  static library linking it privately) settled the mechanism:
  - without it, `cmake --install` ran zstd's own rules (its three headers, `libzstd.pc`, `lib/cmake/zstd/`) in every
    full install, and the Cerid export resolved the link to `zstd::libzstd_static`, a second package a consumer
    would have to find;
  - with it, zstd's install rules are gone, and installing `libzstd_static` into the Cerid export set gives one
    archive and `Cerid::zstd`, with no conflict.
- Policy CMP0141 is NEW for zstd's directory. zstd's `cmake_policy(VERSION 3.13)` left it unset, so the engine's
  `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` (embedded under the compiler launcher) never reached zstd's objects.
- Expected on Windows, not yet seen there (this machine builds Linux; the first Windows run confirms both): zstd's
  objects compile `/Z7` under the launcher, and under the `pdb` form the package finds zstd's compile PDB at
  `<zstd's binary directory>/CMakeFiles/libzstd_static.dir/libzstd_static.pdb`, CMake's default for a static library.
- The registry's `compression.codecs` decision and the cryptography inventory follow:
  - zstd joins the `cerid-package` export's packages, and `cprof_export` reaches it through crd-perf;
  - both classifications were restated (zstd performs no cryptographic function, so every finding stands) and
    reviewed with `gen-crypto-inventory.py --review`. Counsel's review stays open, as it was.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

At the final code rebased on main at 8a243a34:

- The whole tree built with no warnings.
- crd-perf-tests: 335 cases pass but one, `test_diag_host.cpp`'s GPU barrier case. Its specimen reports
  `CRD_HOST_GPU_ABSENT no-validation-layer`: this machine's Vulkan layer is not set up as the hosted Linux lanes set
  it up. The case does not touch bundles, and it failed the same way before this batch's last change.
- CTest: 203 tests, every fuzz corpus and every ceridc, compression, resources, cooker, asset and `crd-check-` test.
- New and changed cases:
  - `test_diag_bundle.cpp`:
    - a section stored as the owner's DEFLATE byte for byte, beside a raw one;
    - the same sections raw under `None`, with the same decoded size;
    - a cut section stays raw;
    - the total cap counts decoded bytes, in the writer and the reader;
    - the reader's refusals, each from a valid bundle patched in one field: a claim past DEFLATE's reach (with
      nothing asked of the allocator for it), a claim one byte long and short, both flags, a stream whose block type
      became the reserved one with its CRC recomputed, a refused allocation, and a corrupt stored byte caught by the
      CRC first;
    - a claim past the section cap is listed undecoded;
    - moving the result keeps the decoded views.
  - `test_diag_bundle_import.cpp`: the import owns its decoded bytes; a bomb within the format but past the
    importer's cap is flagged oversized with nothing allocated; a lying claim is refused with the crash bytes kept.
    The golden v1 bundle is written raw: its frozen bytes are the form every reader keeps reading.
  - `test_diag_flight_recorder.cpp`: the redaction case checked the bundle file's bytes for the public and restricted
    texts. It now reads every section back, because the flight record is stored compressed.
- The fuzz target's oracle covers compressed sections: never also truncated, a claim within DEFLATE's reach, decoded
  to exactly its claim inside the import's own bytes, which hold nothing else. A capped claim is flagged oversized
  instead. The 12 committed seeds are written raw again, so `--seed` reproduces them byte for byte. Two new seeds join
  the corpus: a writer-made compressed log tail, and the same with its claim one byte long. Corpus and seeds replay
  clean.
- The first CTest run failed crd-check-registers: the two exports' inventories had changed. The review above fixed it.
- `scripts/test-package-consumer.py --preset linux-gcc-debug` passes:
  - the installed layout is the family's 12 archives plus `libzstd.a`, with no zstd header;
  - the engine build tree is deleted before the consumers build;
  - the diagnostic application's failure and every example pass, the capture example with its flight record stored
    compressed and decoded to its size;
  - the new `codecs` route round-trips DEFLATE and a zstd frame from the moved prefix.
- `test-repository-tools.py`'s package-consumer tests cover the dependency archive under both platform names, its
  PDB, a missing or doubled archive, the codecs judge and the compressed flight-record judge.
- Teeth: each rule removed, crd-perf-tests rebuilt and `[bundle],[import]` run, then the file restored with a fresh
  time. All nine were caught: the DEFLATE-reach refusal, the both-flags refusal, the section cap and the total cap
  leaving a claim undecoded, a refused reservation stopping the read, the writer's whole-sections-only and
  shrinks-only rules, the writer counting decoded bytes against the caps, and the importer flagging an undecoded
  section oversized. Two first mutations did not isolate their rule (one left a variable unused, which the compiler
  refused; one changed half of the room condition, which the other half still held) and were rerun whole.
- The strict tidy helper (LLVM 20.1) on every changed C++ file is clean, but for `example_codecs.cpp`, which only the
  package consumer's own project compiles (the helper reports it ungated, as it would the other examples; GCC built it
  clean there). clang-format 20.1.8: its hunks that lie
  wholly on added lines applied (35, alignment and wrapping), none mixing in older lines. The read result's aggregate
  initialiser, which the formatter laid out in columns, became a constructor taking the allocator.

## Before/after board

`bench_compression` gains two cases:

- `bundle-write`: its 256 KiB text input as a flight bundle's log tail, beside a 32 KiB crash record and an explicit
  absent symbol index;
- `bundle-read`: every section of that bundle read back.

Two release builds ran through [ab-board.py](../../scripts/ab-board.py):

- The before arm is main at c29c5844 with this bench source, so its sections are raw.
- The after arm is this batch's code.
- Setup: linux-gcc-release, GCC 13.3, `cerid-track-b`, CPU 3 pinned, one warm-up, 10 repetitions, band 2%. Times are
  ns per KiB of input.
- `--same` held on every key but `bundle-write`, whose bytes are the change, so the read decodes exactly what the raw
  bundle held.

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| bundle-write | 135 | 70467 | 0.0019 | slower |
| bundle-read | 51.96 | 3365 | 0.0155 | slower |
| zstd, adler32, npz-write, npz-read-stored | | | 0.9996 to 1.007 | level |
| DEFLATE and inflate cases (deflate.cpp unchanged) | | | 0.96 to 0.98 | slower |

- **The bundle shrinks 5.6 times**: 295 048 bytes become 52 803. Writing now costs the encoder, about 14 MB/s, since
  crd-compression's DEFLATE emits one fixed-Huffman block. Reading costs the inflate, about 300 MB/s. A flight capture
  is a few KiB to a few hundred, so a capture costs well under the 20 ms this 288 KiB bundle takes. A section DEFLATE
  does not shrink costs one encode and is stored raw.
- **The codec cases moved, not the codec.** The DEFLATE and inflate cases are 2% to 4% slower and `deflate.cpp` is the
  same in both arms. This is the code-placement shift the earlier boards measured under a 64-byte function-alignment
  control: between +2.6% and -9% for the unchanged encoder from one build pair to the next
  ([one ZIP container](2026-11-01-own-one-zip-container.md#beforeafter-board)).

## Remaining on OWN.1

- crd-perf's bundle publish (`write_bundle_atomic`) onto crd-platform's durable files, once crd-perf can link
  crd-platform. crd-platform is not in the relocatable package, and its windowing brings GLFW.
