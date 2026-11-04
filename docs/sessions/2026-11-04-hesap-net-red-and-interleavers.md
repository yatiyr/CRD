# 2026-11-04: HESAP-NET, RED and the interleavers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 98.

## CI first

In dispatch 41754591577 (f387ced1), win-tidy failed on `cppcoreguidelines-macro-usage` at
tests/foundation/crypto/test_key_store.cpp:1450, which batch 92 had added. 236ff49d makes `CRD_TEST_PLATFORM_VAULT`
a flag macro tested with `#ifdef`, and clang-tidy 20.1.8 passes the file against win-tidy. The four
`crd-crypto-state-learning` timeouts in the same run are at f387ced1, which is older than 2681433b (batch 96's fix).

## What landed

- **RED (RFC 2198), `red.hpp`.** I read RFC 2198 in full first (SHA-256 86c433f7...ce4).
  - `red_parse` checks the header chain, the room and the lengths, then fills the blocks; the data spans view the
    payload. The writer keeps §3's "different time intervals" rule (no offset 0, no offset twice). The reader accepts
    whatever the format can express. Which payload types a block may carry is the session's choice (§5).
  - Refusals come in a documented order. A refused parse leaves the blocks unchanged, and a refused write writes nothing.
  - Register entry `fec.red` is now implemented.
- **Interleavers, `interleave.hpp`.**
  - Block interleaver: R x C symbols, written by rows and sent by columns; position functions plus byte permutations.
  - Convolutional (Forney) interleaver: B branches of depth M. A branch's ring slot is (turn mod length), so no state
    is kept per branch. It runs in place or into a separate buffer.
- **Wire bytes owner:** `containers.wire-bytes` gains `wire_load`/`wire_store` at a fixed offset. FlexFEC's private
  copies of `load16`/`load32`/`store16`/`store32` are gone; its block writer now uses `WireWriter`.
- **scripts/fuzz.py:** reusing a scratch directory reported the earlier run's crash as a new finding, and added the
  old workers' statistics to this run's. The board now names only artifacts this run wrote and removes stale
  `fuzz-*.log` files. A unit test covers this.

## Evidence

- win-debug: crd-hesap-comms-tests, 61 cases (133478 assertions); the `crd-fuzz-hesap-red` corpus (20 seeds),
  `crd-fuzz-hesap-flexfec` corpus and `crd-fuzz-hesap-erasure` corpus; crd-containers-tests, 135 cases; crd-crypto-tests
  `[mls],[policy],[hpke]`, 77 cases (they use WireReader); the `crd-no-`/`crd-check` guards (crd-check-capability-owners
  failed once and passed when rerun, the host fault already recorded).
- Teeth, each caught:
  - refusing lengths that exactly fill the payload: 3 unit cases and the fuzz parse oracle fail;
  - dropping the offset-0 refusal: unit cases and the fuzz encode oracle fail;
  - losing the input on the in-place convolutional path: the run stopped at its cap of 30 failed checks.
- linux-gcc-debug (WSL): 1068 CTests passed, no warnings. linux-gcc-release (LTO, -Werror) found three defects,
  fixed in the follow-up commit: an assert-only helper unused in release (an overlap is now a refusal) and two
  -Wstringop-overflow tag loops in the test; 61 cases and the three corpora then pass.
- clang-tidy 20.1.8 (win-tidy) over every C++ source changed since f387ced1: clean after fixing batch 94-97
  findings in hesap-comms (nested conditionals, local constant names, a missing [[nodiscard]], a hand loop).
- linux-clang-fuzz, `fuzz.py run`: hesap-red ran 300 s at 2 jobs, 16.4M executions, no finding. hesap-flexfec
  (after the wire-bytes move) ran 120 s, 3.8M executions; its only "finding" was batch 97's stale artifact, which led to
  the fuzz.py fix above.

## Next

1. The ISA-L NASM `ec_encode_data` budget and the OpenFEC RFC 5510 oracle.
2. FlexFEC and RED against libwebrtc, with their budgets.
3. RaptorQ (RFC 6330).
4. The hesap-stats estimators.
