# 2026-11-04: NET.2, the state learner's pipes and its time on Windows (Track C batch 96)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [state learning](../design/network-foundation.md#state-learning).

## CI first

Dispatch 41754591577 (f387ced1): win-clang-cl failed one test, `crd-crypto-state-learning`, on its 3600 s timeout
with no output. Every other Windows job of the run was still running the same test. One cause, both halves fixed:

- **A deadlock.** `Sul.answer` wrote a whole batch (64 queries; the determinism recheck sent 200 at once) before it
  read any answer. The SUL answers each line as it reads it, so once the answers filled the stdout pipe the SUL
  stopped reading, and the learner blocked on stdin. Windows' anonymous pipes hold 4 KiB, Linux's 64 KiB, so only
  Windows hung. Reproduced on win-debug: a batch of 64 words of 24 inputs (13 KiB) hung; 8 KiB did not.
- **The time.** Without the deadlock, one SUL copy needs 1359 s on win-debug (463 s client, 895 s server; Linux took
  111 s). A sanitizer lane would not fit in 3600 s.

## What landed

- scripts/check-state-machine.py: a writer thread feeds each batch while the reader collects the answers. A SUL that
  ends early is reported with the word it ended on, and its broken pipe no longer hides that report. `SulPool` runs
  `--jobs` copies (default: the CPU count) and deals each batch out in contiguous shares. The queries are
  independent, because each one starts fresh sessions. The determinism recheck now crosses copies.
- The CTest runs `--jobs 4` with `PROCESSORS 4`.
- test-repository-tools.py: a 64-word batch of about 230 KiB through an echo SUL, more than either OS's pipe buffers;
  a SUL that ends after one line; and the pool's order for 0 to 64 words and its exit reports.
- Teeth: the new test against the old driver fails with "the SUL driver deadlocked" after 120 s.
- Also fixed: a 121-column comment in tests/foundation/crypto/CMakeLists.txt (batch 92).

## Evidence

- win-debug, `check-state-machine.py --jobs 4`: tls-client 116 s, tls-server 212 s, PASS. Both learned machines
  equal the reviewed ones, with the same 145782 and 177779 queries as with one copy.
- Unit tests: `test-repository-tools.py -k state_learning` passes; every validator passes.

## Next

Audit the first completed hosted run with this commit: `crd-crypto-state-learning` on every Windows lane, win-asan
and linux-clang-tsan included. Then HESAP-NET's next clause, FlexFEC parity per RFC 8627.
