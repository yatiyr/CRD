# 2026-11-07 — HESAP-NET: the clock filters against ntpd and libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Clock filters](../sessions/2026-11-07-hesap-net-clock-filters.md).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_ESTIMATORS_VS_WEBRTC=ON -DCRD_BUILD_CLOCK_VS_NTPD=ON`. The peers are built without link-time optimisation:
ntpd 4.2.8p18's `clock_filter()` cut verbatim, and libwebrtc branch-heads/8147's `RtpToNtpEstimator` and `TimestampAligner`.
`crd-hesap-clock-board --rounds 25`, run twice: 400,000 updates of each arm, the best of 25 interleaved rounds. The board fails
if the two sides' final states differ in one bit.

## Cost

| Filter | hesap ns/update | peer ns/update | peer / hesap | hesap bytes, heap | peer bytes, heap |
|---|---|---|---|---|---|
| RFC 5905 clock filter / ntpd | 50.8 | 69.8 | 1.37 | 360, none | 328, none |
| RTP to NTP line / RtpToNtpEstimator | 37.2 | 39.7 | 1.07 | 376, none | 72, a 32-byte list node per report |
| capture aligner / TimestampAligner | 2.9 | 3.0 | 1.02 | 40, none | 40, none |
| ClockTracker, whole exchange | 144.5 | no peer | | 2,232, none | |

The two runs agree within 0.5 ns. The gain on ntpd's filter is integer time and no global state; the line and the aligner are
the same arithmetic, so they should be level. 145 ns per exchange against a poll of one per second is no budget concern.

## Accuracy

Errors at T4 in microseconds (median, p99) past the first 200 polls and an event's 120 polls of recovery, 4,000 polls of each
`clock_paths.hpp` path. "ntpd filter alone" is the last sample its filter called fresh. ntpd disciplines the host clock behind its
filter and the path's clocks run free, so that column carries the frequency's lag, which is what the tracker removes; it is not
ntpd's accuracy as a daemon. chrony and linuxptp, whole, are NET-TIME's comparison.

| Path | raw exchange | ntpd filter alone | tracker | bound violations | frequency error, PPM | final bound |
|---|---|---|---|---|---|---|
| lan | 13.8, 87.6 | 100.3, 220.4 | 1.0, 4.0 | 0 | 0.00 | 204 |
| wan | 914.9, 13006.3 | 381.2, 1520.9 | 63.1, 228.3 | 0 | 0.28 | 21,555 |
| wifi-bursts | 3927.6, 44783.4 | 817.8, 4905.9 | 168.0, 739.8 | 0 | 7.29 | 8,156 |
| asymmetric | 15004.6, 16506.7 | 14809.9, 15309.6 | 15003.4, 15067.6 | 0 | 0.10 | 25,549 |
| lossy | 1240.9, 26408.1 | 399.3, 2121.2 | 108.5, 404.6 | 0 | 0.82 | 17,388 |
| wander | 104.8, 655.9 | 44.8, 177.1 | 7.1, 29.0 | 0 | 0.05 | 2,306 |
| step-forward | 173.6, 1097.1 | 99.1, 351.0 | 14.3, 49.6 | 0 | 0.06 | 5,374 |
| step-back | 173.8, 1096.7 | 101.1, 359.7 | 14.3, 49.5 | 0 | 0.06 | 5,374 |
| leap-smear | 173.6, 1096.9 | 57.7, 263.0 | 13.8, 280.4 | 0 | 0.06 | 5,374 |
| frequency-jump | 174.1, 1096.6 | 376.0, 2189.8 | 14.5, 49.9 | 0 | 0.06 | 5,374 |
| route-change | 208.0, 1316.8 | 77.4, 336.6 | 20.4, 373.2 | 0 | 0.07 | 33,433 |

The tracker's median is 10x to 23x below the raw exchange's. The asymmetric path's 15 ms is the 30 ms forward asymmetry's half,
which no exchange can see: it stays inside the bound, as every error on every path does. The p99 on leap-smear and route-change is
the line catching up within the window the runs test cuts.
