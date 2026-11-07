# 2026-11-07: clock offset and drift filters, bit-exact with ntpd and libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 103. CI: nothing to fix; 42282917539 has no failed
job so far and no run containing 2d83dca0 has finished, so the libwebrtc peer's install and the `InterarrivalJitter` ASan report
(the hosted corpus replay) stay open.

## What landed, `clock.hpp` in crd-hesap-net (capability `numerics.network-estimators`), no allocation

- `on_wire_sample`: RFC 5905 §8; a peer writes two timestamps, so the arithmetic is modulo 2^64 and an impossible exchange is refused.
- `ClockFilter`: §10's eight stages as ntpd's `clock_filter()`, Allan-intercept metric and popcorn gate included.
- `ClockDriftFilter`: a weighted line over at most 64 samples, shrunk by a 500 PPM prior; a runs test cuts the oldest samples.
- `ClockTracker`: the three for one peer. A raw exchange whose interval excludes the line is a spike; three that agree (or 16 in
  a row) are a step, which keeps the frequency. The frequency is removed inside the filter, so a sample's age costs nothing on
  free-running clocks. It publishes an offset, a frequency and an error bound; NET-TIME's clock-quality value reads them.
- `MediaClockMap` and `CaptureClockAligner`: libwebrtc's RTP to NTP line and capture aligner, for NET-LIVE.

The peer for §10 is ntpd itself: `clock_filter()` is cut verbatim from the pinned 4.2.8p18 tarball at configure time and compiled on
`ntpd_peer.c`'s few stand-ins. An RFC Appendix A restatement would share its author with the code and miss the deployed metric.
chrony and linuxptp are NET-TIME's real peers (GPL, never read for this code).

## Evidence

- `crd-hesap-clock-ntpd-tests`: 9 streams of 200,000 updates, every stage, order, offset, delay, dispersion, jitter and outcome equal
  by bits, every return of the function reached; on linux-gcc-public-checks and LTO linux-gcc-release.
- `crd-hesap-estimators-webrtc-tests` `[clock]`, 2,400,025 assertions: both libwebrtc classes, slope, offset, estimates and
  translations by bits, through wrap, half-range ties, duplicates, jumps and restarts.
- `crd-hesap-net-tests` `[clock]`, 244,274 assertions in 29 cases, on win-debug, linux-gcc-public-checks and linux-gcc-release: 11 hostile
  paths (loss, queue spikes, asymmetry, wander, steps both ways, leap smear, a 340 PPM jump, a route change), 5 seeds each: no
  bound violation, no false step.
- The first version stepped falsely: two samples of tiny stated error gave a line of thousands of PPM. A sample's error now starts
  at the exchange's own bound and falls with a running mean of the path's noise.
- `crd-fuzz-hesap-clock`, 14 seeds: 2.88 million libFuzzer executions clean on UBSan and ASan; the one finding was a harness negation.
- 18 teeth, each caught by the named test and restored. One survived at first (the half-range unwrap tie): the oracle now asks there.
- clang-tidy clean: 20.1.8 on win-debug's files, 20.1.0 on the peer lane's.
- [Board](../bench/2026-11-07-hesap-net-clock-vs-ntpd-webrtc.md): median error 10x to 23x below the raw exchange; 1.37x ntpd's filter speed,
  level with libwebrtc's line and aligner, no heap against a list node per report.
- Not proven yet: no hosted run contains this, so the ntp tarball's install from CI's network is untested.

## Next

Interpolation, extrapolation, dead reckoning and rewind sampling; quantisation helpers; then NET-DEBUG's Philox distributions and
RaptorQ.
