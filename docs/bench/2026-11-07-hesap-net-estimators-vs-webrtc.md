# 2026-11-07 — HESAP-NET: the delay trend and acknowledged rate against libwebrtc

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Estimators](../sessions/2026-11-07-hesap-net-estimators.md).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_ESTIMATORS_VS_WEBRTC=ON`. libwebrtc is branch-heads/8147 (84b14960), its estimator files verbatim with its
checks, logging and field-trial parsing shimmed and its one `pow` routed to `crd::math::pow` as on hesap's side, without
link-time optimisation. `crd-hesap-estimators-board --rounds 25`, run twice: 400,000 packets of the `capacity-steps` path,
about 82,000 of them completing a group; the best of 25 interleaved rounds. The board fails if the two sides' final states
differ in one bit.

## Results

The first run; the second agreed within 0.5 ns on every row.

| Estimator | hesap ns/update | libwebrtc ns/update | libwebrtc / hesap | hesap bytes, heap | libwebrtc bytes, heap |
|---|---|---|---|---|---|
| packet groups | 4.4 | 7.7 | 1.76 | 128, none | 112, none |
| trendline, window 20 | 24.2 | 23.1 | 0.96 | 624, none | 256, 1.97 MB in 3920 blocks |
| Kalman and detector | 40.0 | 41.8 | 1.04 | 608, none | 216, 658 kB in 1287 blocks |
| acknowledged rate | 1.4 | 2.0 | 1.48 | 72, none | 248, none |

## Reading

hesap is faster on the group tracker and the rate. libwebrtc's unit types check for infinities on every operation, which is the
likely reason; that was not measured separately.

The trendline and the Kalman filter are level, by construction: the arithmetic has to round identically, so neither side can
reorder the additions and multiplications that dominate the update. The trendline's 1.1 ns gap is the one row where hesap is
behind; hesap walks a ring where libwebrtc walks a deque, and the cause was not isolated further. Within the gap, libwebrtc's
windows allocate: each 512-byte deque block holds 21 samples or 64 frame periods, which the table shows as 3920 and 1287 blocks
over the run. hesap's windows are fixed at setup, so its hot path never allocates.

The byte columns compare only together with the heap beside them: hesap's samples are in its figure and libwebrtc's are on its
heap. libwebrtc's 248-byte rate is with the shim's parameters, which hold no `std::string` as its own do.
