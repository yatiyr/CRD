# 2026-11-01 — OWN.1: one ZIP container in crd-compression

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the first remaining item after the
[one inflate decoder](2026-11-01-own-one-inflate-decoder.md). Built, tested and measured on `cerid-track-b`. The
contract is [capability ownership](../design/capability-ownership.md#one-zip-container), decision 8.

## CI for the previous batches

Run 41247045494 (67977fda, through batch 5's 4397334b) is the first with finished jobs past the build for this
track's commits; run 41251644253 (05a133c4, batch 6) was cancelled by a newer push before it started a job.

- The four failed Windows lanes (win-debug, win-debug-scalar, win-asan, win-public-checks) and win-tidy stop at one
  unit: `tests/tools/ceridc/test_ceridc_screen.cpp` (COMPLY.1's `ceridc screen` test), MSVC C2797 on its
  `std::initializer_list` member initialisers. Track C's d9be0cfd fixed it on main. win-tidy builds keep-going and
  stopped on that unit alone, so every other unit of batches 1 to 5 passed the strict clang-tidy on Windows.
- Passed: windows-native (win-vs), win-arm64-debug, linux-gcc-arm64-debug, linux-clang-arm64-debug and both
  repository-check jobs. The Linux, shipping and clang-cl lanes were still running.
- No failure traces to this track. Batch 6's MSVC and clang-cl evidence (the constexpr fixed trees) waits for a run
  that covers 9cdebf27 and ff1c2b3b.

## One container (capability `compression.zip-archives`)

`crd/compression/zip.hpp` is the engine's one ZIP container. Resources' `zip_archive.hpp`/`.cpp` are deleted, and
hesap-tensor's npz reader and writer no longer carry ZIP records. The decision and each rule's origin are in
[capability ownership](../design/capability-ownership.md#one-zip-container).

- **`ZipReader`**: `open` finds the end record, follows Zip64 when a classic field holds its sentinel, walks the
  directory and resolves every member through its local header. A structure fault, a STORED member whose sizes
  differ or a DEFLATE size past what its bytes can hold is refused before any member is read. Entries are plain
  values whose names view the archive. `view` returns a STORED member's bytes in place. `extract` decodes either
  method into a buffer of exactly the member's size, or into an array it sizes. CRC-32 is checked on every read.
- **`ZipWriter`**: `add(name, bytes, method)`; Deflate stores a member DEFLATE does not shrink. Every count, size and
  offset stays below its Zip64 sentinel, so the 65 535th member and 4 GiB are refused. `reserve` lets a caller that
  knows its members write the archive in one allocation. `finish` returns a status.
- Every refusal is a status: `NotRecognized`, `Truncated`, `Malformed`, `Corrupt`, `Unsupported` and the new
  `OutOfMemory`. Both classes grow through the containers' `try_` path.
- **Consumers.** The cooker's 3MF bridge and `ceridc import`'s 3MF route read through it. The 3MF and cook tests
  write through it. crd-cooker, crd-ceridc and the two test targets link crd-compression, and their `crd_module`
  declarations name it. `NpzReader` reads through it: STORED members stay views, and DEFLATE members decode into the
  backing store reserved up front. `NpzWriter` encodes its members as they are added, then writes them at `finish`
  through one reserved writer. hesap-tensor's `detail/io_bytes.hpp` keeps only the npy and safetensors header
  reads.
- The registry has `compression.zip-archives` landed with `zip.hpp` as its API and no pending file.

### Behaviour changes, each deliberate

- npz reads Zip64 archives (NumPy writes them past 4 GiB); the npz reader had refused them.
- `NpzReader::name` views the source bytes instead of a copy the reader owned, so the bytes must outlive names as
  they already had to outlive payloads. Its one engine caller, `tensor.io.info` in `cli_register_tensor.cpp`, keeps
  them for the reader's whole life.
- A member written with a data descriptor (general-purpose bit 3) reads. The central directory holds the real sizes
  and CRC (APPNOTE 4.4.4). Resources' reader had refused a member whose CRC and compressed size were both zero, a
  valid empty member among them.
- A spanned archive is refused by any disk field and by unequal entry counts. Resources' reader checked one field.
- Resources' archives get the DOS date 1980-01-01; they had date 0, which is not a valid DOS date. That is the only
  byte that changes; the npz archives are unchanged byte for byte.
- Resources' writer wrote a 65 535th member's count as the Zip64 sentinel and silently truncated offsets past
  4 GiB. Both are now refused.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- The tree built with no warnings after each change. smoke_hesap_tensor, the link-isolation gate, links and runs.
- `test_zip.cpp` (8 cases, moved from resources' `test_zip_archive.cpp` and rewritten) covers:
  - round trips through both methods, deterministic bytes, exact-name lookup, views and exact-size decodes (a
    buffer of any other size refused);
  - the written bytes against a hand-laid classic archive, and an empty archive and an empty member;
  - the end record found behind a comment, and a data-descriptor member;
  - Zip64 read, with the 0x0001 extra missing, short or overrunning, the locator and record missing, misplaced or
    spanned, the record's size field too small, overrunning or wrapping past 2^64 with its leading bytes, and the
    directory past its bounds;
  - every reader refusal class, each from a valid archive patched in one field;
  - the writer's refusals (empty name, a 65 536-byte name, a 65 535th member) with the longest name and 65 534
    members accepted;
  - a reserved archive written without another allocation (a counting allocator), the same bytes as an unreserved
    writer's, and refused reservations;
  - allocation refusals on a 1 MiB pool for the writer, the directory and a member, each a status with the archive
    so far kept.
- From a scratch directory, at the final code rebased on main at d64f2ce8: crd-compression-tests (25 cases),
  crd-hesap-tensor-io-tests (19, against NumPy's corpus; the npz round trips write `io_corpus/out/` byte-identical
  to before), crd-cooker-tests (30) and crd-resources-tests (135).
- CTest there: the zip, npz and 3MF cases and the `crd-check-` guards (29). The asset-io 3MF cases read lib3mf's and
  PrusaSlicer's Zip64 fixtures and round-trip the written package. Earlier in the batch (d97f06c7): every
  hesap-tensor, ceridc, compression, cooker, asset-io and resources-fuzz test (453), and the 22 guard tests, among
  them crd-check-capability-owners, crd-check-source-routes and crd-check-include-budget.
- The strict tidy helper (LLVM 20.1) on every changed C++ file is clean after one fix: the method enum is one byte.
- clang-format 20.1.8 on every added line: its hunks that lie wholly on added lines applied (62, alignment and
  operator placement), none mixing in older lines.
- Teeth: each rule removed, its suite rebuilt and run, then the file restored with a fresh time. All 12 were
  caught: the Zip64 record's size unbounded (a wrapping size had passed the first form of the check), the DEFLATE
  claim unbounded, a STORED view unchecked, the local header unchecked, a member past the bytes,
  encryption accepted, the sentinel count written, a wrong-size buffer accepted, the 1980 stamp dropped, the end
  record left out of a reservation, an npz member not decoded, and an npz STORED member unchecked.

### A stale object on the machine, not a defect

The first run of crd-resources-tests stopped on a TLSF out-of-memory fatal in `test_png.cpp`'s 2 GiB-claim case. In
gdb, `png_decode` skipped its size bound, and the object held no call to `deflate_max_inflated_size`. Batch 6's
PNG and EXR teeth had restored `png_image.cpp` and `hdr_exr.cpp` with `cp` and `mv`. That gave the sources an older
time than their mutated objects, so ninja kept the mutants. After touching both files, the suite passes (135
cases). This batch's teeth restore with a fresh time (`os.utime`). Hosted CI builds fresh and never saw the mutant.

## Before/after board

`bench_compression` gains six cases:

- `zip-write`: a 3MF-like package of eight 32 KiB members, DEFLATE.
- `deflate-members`: the same eight members deflated alone, which is the codec's share of `zip-write` and the
  in-binary control for it.
- `zip-read`: the package opened and every member extracted into its own array, as the cooker does.
- `npz-write`: eight f32 members STORED, as `np.savez` writes them.
- `npz-read-stored` and `npz-read-deflate`: those members parsed by `NpzReader`, the second from the
  `np.savez_compressed` shape.

Two builds run through [ab-board.py](../../scripts/ab-board.py) with `--same` on every output's XXH3-64:

- The before arm is main at 05a133c4, with the bench on resources' `ZipWriter`/`ZipReader` and hesap-tensor's own npz
  classes. Its `zip-write` digest rewrites each record's DOS date to 1980-01-01 outside the timed body, so `--same`
  proves the stamp is the only changed byte.
- The after arm is this batch's code.
- Setup: linux-gcc-release, GCC 13.3, `cerid-track-b` (8 vCPUs, Intel Xeon), CPU 3 pinned, one warm-up, band 2%,
  the host idle before and after.
- Times are ns per KiB of input; speed ratio = before over after.

The first board had `npz-write` at 0.6407 [0.6373-0.653]: the writer grew one buffer member by member, and every
growth copied the archive so far. The npz writer had computed its exact size first. `ZipWriter::reserve` brings that
back: `NpzWriter` encodes its members, then reserves and writes them in one allocation.

Final board at 8b1aa4da, 10 repetitions, `--same` held on all 14 keys:

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| zip-read | 3121 | 3085 | 1.011 [1.006-1.018] | level |
| npz-write | 94.77 | 94.91 | 0.9985 [0.9929-0.9992] | level |
| npz-read-stored | 50.98 | 50.81 | 1.003 [1.002-1.004] | level |
| npz-read-deflate | 3176 | 3121 | 1.016 [1.014-1.02] | level |
| zip-write | 55838 | 60505 | 0.923 [0.9168-0.9437] | slower |
| deflate-members | 55006 | 60653 | 0.9069 [0.9013-0.9345] | slower |
| deflate-raw | 61618 | 66150 | 0.9321 [0.9146-0.9525] | slower |
| zlib-deflate | 65083 | 70862 | 0.918 [0.9142-0.9381] | slower |
| inflate-raw, inflate-exact, zlib-inflate | | | 1.006 to 1.01 | level |
| adler32, zstd | | | 0.9983 to 1.004 | level |

Every reading case is level. The losses are in DEFLATE's encoder, and `deflate.cpp` is the same in both arms: the
codec's placement in the binary moves, not the code.

- **The in-binary control.** `zip-write` divided by `deflate-members` is the container's own cost over the codec, and
  it comes from one binary, so placement between builds cancels. Per repetition, the before arm's ratio has median
  1.0151 (zip-write 831 ns/KiB over its codec, from 648 to 1072). The after arm's ratio has median 1.0009 (55 ns/KiB,
  from -1068 to 837). The container's own write cost fell from 1.5% of the codec's to noise. Resources' writer
  appended every payload, directory and archive byte one at a time; this one copies each part once.
- **The layout control.** Both arms at 542d33ef were built with every function aligned to 64 bytes, 30 repetitions.
  The reading and npz cases were level (0.9954 to 1.001). The unchanged encoder still moved: deflate-raw 0.9772
  [0.9715-0.984] and zlib-deflate 0.9715 [0.9652-0.983]. zip-write moved with it, 0.972 [0.9666-0.98]. The same
  source had been 1.008 and 1.026 (deflate-raw, zip-write) on the first pair of builds and 0.9804 and 0.9834 on the
  second. The encoder's speed changes from one build pair to the next, between +2.6% and -9% on this machine, as the
  [compression module's board](2026-10-31-own-compression-module.md) found for its 4%.

## Remaining on OWN.1

- crd-compression and its zstd join the relocatable package, then crd-perf's bundle reads and writes compressed
  sections through it (its `kSectionFlagCompressed` is refused today) with a bounded inflate.
- crd-perf's bundle publish onto crd-platform's durable files, once crd-perf can link crd-platform.
