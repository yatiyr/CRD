# 2026-11-07: RTT, jitter, delay trend and rate estimators, bit-exact with libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 102.

## CI

Nothing to fix: 42282917539 (474a37f8) has no failed job so far, with 15 jobs still running; 42300774218 (ac88bfed) and
42298265296 (7b1b1b32) have not started.

## What landed, in crd-hesap-net (capability `numerics.network-estimators`)

- `rtt.hpp`: RFC 9002 §5 in integer nanoseconds, with PTO, the loss delay and the persistent congestion duration; samples and
  the peer's ack delay are clamped so nothing a peer sends can overflow.
- `jitter.hpp`: RFC 3550 A.8's fixed point over wrapping timestamps, with libwebrtc's bound on a stream restart as an option.
- `delay_trend.hpp`: GCC's packet groups, trendline with its sort and cap options, Kalman filter and the one adaptive detector
  both feed. libwebrtc holds the detector twice; here it is one class. Its deques became a caller's fixed window and a ring.
- `rate.hpp`: Nichols' windowed extremum, the delivery rate sample and the Bayesian acknowledged rate.

Where libwebrtc turns to NaN or only logs, hesap stays finite: an estimate of zero, a sample variance past float, a noise
estimate driven to infinity by an absurd negative send delta, and a covariance off positive semi-definite. Values inside the
domain are unaffected: the oracle asserts no restart happened.

## The peer

webrtc.googlesource.com changes its archive bytes on every request, so `tools/webrtc-bwe-sources` pins 28 files one by one
through Gitiles' `format=TEXT` at branch-heads/8147 (84b14960), installed by `scripts/install-webrtc-bwe.py`, on the
linux-gcc-public-checks lane. Only checks, logging and field-trial parsing are shims, plus the Kalman filter's one libm call:
the Cerid Math Mandate takes `std::pow` out of the module, so both sides of the oracle use `crd::math::pow`, and the estimate
is the same on every platform. The module now links crd-math, whose closure stays jobs-free.

## Evidence

- `crd-hesap-estimators-webrtc-tests`, 5,602,750 assertions, all bits equal after every update: 9 simulated paths of 200,000
  packets (jitter, reordering, aggregation, clock jumps, capacity steps, key frames) through 7 trendline options, the Kalman
  filter with its detector and 4 rate options, every detector state reached. On linux-gcc-debug and linux-gcc-public-checks.
- The oracle found one real difference: a burst's later send time makes a group's send delta negative, which the first version
  refused. It is now accepted, and libwebrtc's -1 "not over" sentinel is kept for the same reason.
- `crd-hesap-net-tests` on win-debug: 729,916 assertions in 59 cases, 24 of them new (594,170 assertions), restating RFC 9002,
  RFC 3550, least squares and the Kalman update.
- `crd-fuzz-hesap-estimators`, 12 seeds: 3.17 million libFuzzer executions clean on linux-clang-fuzz, on the final code. Its two
  early findings were harness bugs (a shift of 64, a history shorter than the window), both fixed.
- One fuzz report is unexplained: a run on the final code stopped on an ASan stack-buffer-overflow at the end of the harness's
  state, written from `InterarrivalJitter::update`, whose next frame was an address in no module. The saved input replayed clean
  and the rerun above found nothing, so it was not reproduced. It matches this host's known misexecution; that is not proven,
  and the hosted corpus replays are the check. The scratch directory, with the input, was deleted by the rerun.
- Five teeth, each caught and restored: the Kalman alpha switch and the cap uncertainty (oracle), the application-limited scale
  and the reorder restart (unit and oracle), the noise guard (unit).
- clang-tidy 20.1.8 clean on the six engine files and the unit and fuzz tests; the peer's own test and board are not in a
  Windows compile database.
- [Board](../bench/2026-11-07-hesap-net-estimators-vs-webrtc.md): 1.5x to 1.8x on the group tracker and rate, level on the Kalman
  filter, 4% behind on the trendline (1.1 ns), no heap against libwebrtc's deque blocks.

Not run yet: no hosted run contains this; the peer's install from CI's network is unproven (Gitiles rate-limited one request
here, which `pins.fetch`'s retry should absorb).

## Next

Clock-offset and drift filters, then the interpolation, rewind and quantisation kernels.
