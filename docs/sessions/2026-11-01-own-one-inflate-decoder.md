# 2026-11-01 — OWN.1: one inflate decoder with an exact-size form, and the ZIP container registered

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the first remaining item after the
[compression module](2026-10-31-own-compression-module.md). Built, tested and measured on `cerid-track-b`. The
contract is [capability ownership](../design/capability-ownership.md#one-inflate-decoder).

## CI for the previous batches

No complete GitHub Actions run covers this track's commits yet. Run 41231558128 (d5cc2ec0) is the only one with
finished jobs past the build; the next, 41244525674 (09eca515, the compression module), had started no job when this
batch began, and every other run of the day was cancelled by a newer push while queued. Its failures, by owner:

- **This track's:** win-tidy's SIMD-probe FMA check (`simd_level_probe.cpp:31`), fixed by 11be5fc4 and since
  reworked by track A (b45831d2, a0255f50: each cl kernel unit restates its instruction set for clang-tidy).
  Unverified by a hosted run.
- **Fixed on main by their tracks:** `durable_file.cpp`'s ignored `ftruncate` (be0adf3f), `cpu_features.cpp`'s
  shadowed MIDR fields (1667ad60), `test_register_ids.cpp`'s signed compare (2782ab9e), the comply verb's real-binary
  command quoting on cmd.exe (ee040d0f), and the Windows hosted runners' padded processor name and hidden microcode in
  `test_cpu_features.cpp:1391`/`:1398` and `test_hardware_identity.cpp:500` (d2aa8482). Each unverified by a hosted
  run.

## One decoder (capability `compression.codecs`)

crd-compression's `inflate_raw` and `zlib_inflate` now decode into a buffer of the exact size the caller knows; the
buffer is the window and nothing is allocated, and any other decoded size is refused. hesap-tensor's npz inflate
(`io_zip.hpp`) is gone, merged into it; the header keeps the npy and zip records' little-endian reads and writes as
`detail/io_bytes.hpp`, and hesap-tensor links crd-compression publicly (`io.hpp` is header-only). The appending form
lost its last consumer and is gone with its two defects: a back-reference could reach bytes the caller had left in the
array, and a hostile claim grew the array until the stream failed.

- The rules are zlib's, the strictest of the two copies and nothing stricter: at most 286 literal/length and 30
  distance codes (the npz copy allowed 288 and 32), over-subscribed codes refused (resources' copy built them),
  incomplete codes refused but for one one-bit literal/length or distance code (both copies built them), a complete
  code-length code, a repeat past the declared lengths refused (resources' copy truncated it), an end-of-block code
  required, a zlib window of at most 32 KiB. The fixed trees are built at compile time, the distance tree over all 32
  five-bit codes so 30 and 31 decode and are refused.
- `deflate_max_inflated_size` (1032 bytes per compressed byte) bounds a claim before anything is allocated.
- Consumers, each refusing a size past the bound before it allocates: resources' ZIP reader sizes a member from its
  directory entry (`Corrupt`); the EXR reader decodes a ZIP block into its `block_bytes` (`BadData`); the PNG reader
  sizes its stream from the header (the scanlines of the image or of each Adam7 pass, with their filter bytes;
  `adam7_pass` now gives the passes to both the size and the walk, whose separate exact-size checks are gone;
  `BadData`); the npz reader also refuses a member larger than the archive, before it reserves its backing store.
- The decoder works on local copies of its bit reader and output position inside a block: a byte store may alias
  anything, so with the state behind references every byte sent both through memory (the first board below). A
  stored block is one `memcpy`.

Tests (`test_deflate.cpp`, 9 cases): round trips of six inputs, a 70 000-byte one past the window included, at the
exact size with one byte more and one less refused, raw and zlib; CPython's dynamic-Huffman zlib stream through both
forms at 2600 bytes, 2599 and 2601 refused, every truncation refused; the claim bound at compile time (zero, one byte,
the saturation edge) and the densest stream there is (a literal then 4000 258-byte matches at two bits each) decoding
within 10% of it; hand-built streams the decoder accepts (an empty fixed block, an overlapping match, a single one-bit
distance code, no distance code, a stored then a fixed block); and one stream per refusal, each valid but for the rule
it breaks: no input, the reserved block type, a stored length and its complement disagreeing, a short stored block, a
reference before the first byte, symbols 286 and 287 and distances 30 and 31, 287 literal/length and 31 distance
codes, an over-subscribed and an incomplete code-length code, a repeat with no previous length, a zero run past the
lengths, an over-subscribed and an incomplete literal/length code, an incomplete distance code, the single code's
unused pattern, a stream cut inside a code and before its final block; zlib's header check bits, method, window,
preset dictionary, checksum and a stream shorter than its frame. Two refusals have no valid counterpart (below) and
are tested as refusals alone: a one-code code-length code and a literal/length code without end of block. `test_zip_archive.cpp` and
`test_io.cpp` patch a deflated member's directory entry: one byte more, one less, past the bound, 4 GiB, and (npz) a
compressed size past the archive. `test_png.cpp` decodes a 16384 x 16384 16-bit RGBA header over two deflated bytes
(a 2 GiB stream) on an 8 MiB pool, and `test_hdr_image.cpp` an EXR ZIP block whose compressed size is patched to one
byte: both `BadData`.

Teeth (each rule removed, the suite rebuilt and run, restored): incomplete codes accepted, over-subscribed codes
accepted, 287 literal/length codes, 31 distance codes, a repeat past the lengths, a shorter decode, a 64 KiB zlib
window, the single one-bit code refused, the ZIP, npz and PNG claims unbounded: every one caught. The EXR bound
has no teeth on the test's allocator: a one-byte block also fails the zlib frame, so only the allocation it saves
differs (at most 16 lines of 65 536 pixels per block). The
end-of-block requirement and the one-code code-length code have no teeth by construction: no block can end without an
end-of-block code, and one code-length code can send only one length, so no stream decodes either way; they are
zlib's early refusals, tested as refusals.

## The ZIP container (capability `compression.zip-archives`)

The audit had missed a second ZIP container: hesap-tensor's npz reader and writer restate resources' `ZipReader`
and `ZipWriter` (EOCD search, central directory, local headers, CRC-checked members). Decision 8 of
[capability ownership](../design/capability-ownership.md#decisions) gives it to crd-compression (hesap-tensor sits
below crd-resources); the registry has it `assigned` to OWN.1, with the ZIP record signatures as its signature and
both files `pending`.

## Containers: a try_ path refuses instead of failing fatally

The 4 GiB npz teeth stopped on a trap, not a refusal: `Array::try_reserve` grew through `IAllocator::allocate`, which
TLSF, malloc and the growable allocators make fatal on exhaustion, so the try_ path was fatal exactly where its
callers had opted out. `crd/memory/construct.hpp` gains `try_allocate_array` (through `try_allocate`, nullptr on a
refusal or an overflowing size), and `Array::try_grow_to_at_least` and `String::try_grow_to` allocate through it.
`test_containers.cpp` holds both to a refusal on a 1 MiB TLSF pool, the contents kept.

## Before/after board

`bench_compression` gains `inflate-exact` (a buffer sized once, the npz reader's shape); `inflate-raw` and
`zlib-inflate` allocate their output per call, as the ZIP, PNG and EXR readers do. Two builds through
[ab-board.py](../../scripts/ab-board.py) with `--same` on every output's XXH3-64: the before arm is main at 4397334b
with the bench decoding as its consumers did there (appending into a fresh array; `inflate-exact` through
hesap-tensor's `iodetail::inflate`), the after arm HEAD. linux-gcc-release, GCC 13.3, `cerid-track-b` (8 vCPUs, Intel
Xeon), CPU 3 pinned, one warm-up, band 2%, the host idle before and after. Times are ns per KiB of decoded output;
speed ratio = before over after.

Final board, the batch rebased on main at 67977fda (10 repetitions):

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| deflate-raw | 61470 | 63941 | 0.9604 [0.9574-0.9636] | slower |
| inflate-raw | 4092 | 2945 | 1.39 [1.388-1.391] | faster |
| inflate-exact | 2830 | 2938 | 0.9633 [0.9601-0.9778] | slower |
| zlib-deflate | 64924 | 67405 | 0.9645 [0.9622-0.9694] | slower |
| zlib-inflate | 7127 | 5987 | 1.191 [1.19-1.192] | faster |
| adler32 | 3039 | 3041 | 0.9992 [0.9985-1] | level |
| zstd-compress | 2379 | 2388 | 0.9986 [0.9907-1.004] | level |
| zstd-decompress | 923.8 | 932.4 | 0.996 [0.9879-1.001] | level |

The same two sources with every function aligned to 64 bytes in both builds (`-falign-functions=64`, 30
repetitions), the layout control of the [compression module's board](2026-10-31-own-compression-module.md):

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| deflate-raw | 65409 | 63893 | 1.022 [1.014-1.027] | unresolved |
| inflate-raw | 4096 | 2834 | 1.446 [1.443-1.447] | faster |
| inflate-exact | 2803 | 2832 | 0.9905 [0.9887-0.9916] | level |
| zlib-deflate | 68686 | 67435 | 1.018 [1.009-1.023] | unresolved |
| zlib-inflate | 7137 | 5875 | 1.215 [1.214-1.216] | faster |
| adler32, zstd | | | 0.9982 to 1.01 | level |

- The ZIP, PNG and EXR inflate (inflate-raw, zlib-inflate) are 1.39x and 1.19x faster: resources' decoder read one
  bit per call, appended byte by byte and grew its array; zlib-inflate's Adler-32 is the rest of its time.
- The npz inflate (inflate-exact) is level with hesap-tensor's own inline decoder under the layout control and 3.7%
  slower without it, the same flip as the encoder's (the encoder's loop is unchanged; its tables now come from
  compile-time constants): placement, not the code.
- The first board of the merged decoder (its reader and position behind references) had inflate-exact 0.6997
  [0.6983-0.7004]: every byte store, which may alias anything, sent both through memory. Local copies inside a block
  fixed it (the inflate-exact rows above).

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- The tree built after each change with no warnings; smoke_hesap_tensor (the link-isolation gate) links and runs.
- From a scratch directory: crd-compression-tests (17 cases, 9 of them DEFLATE), crd-resources-tests (138),
  crd-cooker-tests (30), crd-hesap-tensor-io-tests (19, against the oracle corpus), crd-containers-tests (132).
- CTest: the resources fuzz corpus replay and fatal specimens, every hesap-tensor, ceridc, containers, memory and
  asset-io test (3MF through the ZIP reader included), after the rebase: 549 pass. The 22 guard tests pass, crd-check-capability-owners,
  crd-check-source-routes and crd-check-include-budget among them.
- The strict tidy helper (LLVM 20.1) on every changed C++ file: clean after three fixes (a local constant's case,
  nested conditionals in the fixed lengths, a missing include in the container test).
- The pinned clang-format 20.1.8 on every added line.

## Remaining on OWN.1

- The two ZIP containers move into crd-compression as one (decision 8), both modules consuming it.
- crd-compression and its zstd join the relocatable package, then crd-perf's bundle reads and writes compressed
  sections through it (its `kSectionFlagCompressed` is refused today) with a bounded inflate.
- crd-perf's bundle publish onto crd-platform's durable files, once crd-perf can link crd-platform.
