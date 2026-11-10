# 2026-11-10 — NET-PATH: BBRv3 on delivery rate samples

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S69](../design/network-foundation.md#bbr). Source:
draft-ietf-ccwg-bbr-07 (2026-11-02, read from the IETF archive).

## CI first

- win-arm64-debug (scheduled run 42801965828, efed53d5): `fuzz_congestion.cpp` still failed on C4324 after b2dac68b
  moved the slot first. ARM64 MSVC reports the tail padding the slot's alignment adds. 1d964d5c wraps the fuzzer's
  world in the same scoped pragma as `AeadKey`. Only the next ARM64 run can confirm it. No finished run holds
  8eecd439 or d5432c9c yet.

## What landed

- `crd::multiply_divide` (crd-core): the exact floor of a 128-bit product's quotient, native on GCC/Clang and MSVC
  x64, by restoring division on clang-cl and ARM64 MSVC. `crd-hesap-net`'s `delivery_rate()` is now exact integers.
- `DeliveryRateSampler` (`delivery_rate.hpp`, draft section 4.1): a stamp on every packet in flight
  (`QuicSentPacket::delivery`), one sample per ACK frame, and application-limited phases marked by `gate_data`.
- Congestion interface version 2: `CongestionAlgorithm::Bbr`, `on_limited(window, now)`, the bytes in flight at
  `on_ack_end`, the sampler and an entropy word for `make_congestion`, plus BBR's states and its model in
  `CongestionState`. The slot is 768 bytes.
- BBRv3 (`congestion.cpp`): all of section 5 in integer arithmetic. crd-net reads crd-hesap-net's headers only
  (windowed filters, delivery rate, Philox), so the export set is unchanged.
- `cc.bbr` is in the standards register. The test fixture's `Path` gains a rate and a drop-tail buffer.
- Commits: 1d964d5c fix, 3684720d feat (core), 347f6537 feat (net), c053ea44 docs, c04ab10f test (fuzz oracles),
  19339382 docs.

## Evidence

- win-debug: `crd-net-tests [net]`, 692 cases and 92.2 million assertions; `[bbr],[delivery-rate]`, 19 cases;
  `crd-core-tests [wide-multiply]` and `crd-hesap-net-tests [estimators]`; the congestion fuzz seeds and corpus,
  replayed. WSL linux-gcc-debug: the net and core targets build clean, and `[bbr],[delivery-rate],[congestion],
  [net-path]` pass (80 cases).
- linux-clang-fuzz, `fuzz.py run --target net-congestion`: three early findings were oracle gaps (a window inherited
  from BBR, BBR leaving ProbeRTT on a send, a pacer debt from a larger quantum), fixed and adopted; then 420 s and
  1.72 million executions clean.
- The simulated bottleneck (10 MB/s, 40 ms, a one-BDP buffer): 99.2% of the link with 35 KB queued; 82% under 1%
  random loss, where CUBIC gets 3.9%; 0.08% loss at 94% of the link behind a 0.1-BDP buffer; 3 KB of queue behind a
  CE-marking AQM. QUIC: 6 MiB through 2 MB/s, 50 ms at 85% or more of the link, Startup included, staying inside the
  window and allocating nothing.

## Next

BBRv3 against real peers over the interop relays (`--congestion bbr` in the Cerid test programs). Then Prague.
