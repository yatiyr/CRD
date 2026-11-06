# 2026-11-07: RTT, jitter, delay trend and rate estimators, bit-exact with libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 102. CI: nothing to fix; no failed job in
42282917539 so far, 42300774218 and 42298265296 not started.

## What landed, in crd-hesap-net (capability `numerics.network-estimators`)

- `rtt.hpp`: RFC 9002 §5 in integer nanoseconds; a peer's ack delay cannot overflow it. `jitter.hpp`: RFC 3550 A.8.
- `delay_trend.hpp`: GCC's packet groups, trendline (sort and cap options), Kalman filter and one adaptive detector for both.
- `rate.hpp`: Nichols' windowed extremum, the delivery rate sample and the Bayesian acknowledged rate.

Where libwebrtc turns to NaN or only logs (an estimate of zero, a sample variance past float, an infinite noise estimate, a
covariance off positive semi-definite), hesap stays finite; the oracle asserts no such restart happened on its paths.

The peer, `tools/webrtc-bwe-sources`: Gitiles archives change bytes on every request, so 28 files of branch-heads/8147
(84b14960) are pinned one by one, on linux-gcc-public-checks. Shims: checks, logging, field-trial parsing and its one libm call.
The Cerid Math Mandate takes `std::pow` out of the module, so both sides use `crd::math::pow` and the module links crd-math.

## Evidence

- `crd-hesap-estimators-webrtc-tests`, 5,602,750 assertions, all bits equal after every update: 9 paths of 200,000 packets
  through 7 trendline options, the Kalman filter with its detector and 4 rate options, every detector state reached; on
  linux-gcc-debug and linux-gcc-public-checks. It caught one real difference: a burst makes a group's send delta negative, which
  the first version refused.
- `crd-hesap-net-tests` on win-debug and linux-gcc-public-checks: 729,916 assertions in 59 cases, 24 new.
- `crd-fuzz-hesap-estimators`, 12 seeds: 3.17 million libFuzzer executions clean on the final code; two early findings were
  harness bugs.
- One fuzz report is unexplained: an earlier run on the final code stopped on an ASan stack-buffer-overflow written from
  `InterarrivalJitter::update`, next frame in no module. Its input replayed clean and the rerun found nothing. It matches this
  host's known misexecution; that is not proven. The hosted corpus replays are the check.
- Five teeth, each caught and restored: Kalman alpha and cap uncertainty (oracle), application-limited scale and reorder restart
  (unit and oracle), noise guard (unit). clang-tidy 20.1.8 clean on the engine files and the unit and fuzz tests; the peer's own
  test and board are not in a Windows compile database.
- [Board](../bench/2026-11-07-hesap-net-estimators-vs-webrtc.md): 1.5x to 1.8x on the group tracker and rate, level on the Kalman
  filter, 4% behind on the trendline (1.1 ns), no heap against libwebrtc's deque blocks.
- Not proven yet: no hosted run contains this, so the peer's install from CI's network is untested.

## Next

Clock-offset and drift filters, then the interpolation, rewind and quantisation kernels.
