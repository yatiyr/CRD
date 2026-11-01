# REPO.3: the CUDA board on the NVIDIA GPU; 525c8417 audited

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3](../ROADMAP.md#slice-repo.3).
> Contract: [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` at 819245c4 plus this batch's commit. The track machine was not needed (0 h):
the board ran on the development PC, by the operator, and this track builds nothing there.

## CI first

No hosted run covers a0c9d534 or later yet. The push run 41295647725 at 43963cb2 (started 10:01 UTC) still held
the push group at 14:50 with six jobs running (`win-shipping-profile`, `win-relwithdebinfo`, `win-debug`,
`win-public-checks`, `win-asan` shard 2, `linux-gcc-public-checks`), so every later push run waited and was replaced
by the next push; 41317436766 at 819245c4 is pending with no job. Of its six failures, five are the `[shewchuk]`
look-ahead reads a0c9d534 guards (`linux-gcc-asan` shard 1, finished 14:48: `stack-buffer-overflow` in
`fast_expansion_sum_zeroelim` from `orient3dexact`, the one case; shard 2 with the registered TP-5;
`linux-gcc-arm64-asan`, `linux-clang-arm64-asan`, `win-asan` shard 1). The sixth, `win-release` (14:36), fails only
`diagnostic host: the application's wild write leaves one attributable crash capture` (`code image: the module is
not a Windows image`), track A's code-image comparison, repaired by its 79d0c0c0 after 43963cb2. The dispatched runs 41298463221
(13453acb, its Linux repository-checks job queued), 41302856153 and 41311293265 and the scheduled 41295924613 wait on
the account's concurrent-job limit behind it. Nothing of this track's commits has failed; nothing was dispatched (a
dispatch would only queue behind the same limit).

## The board

The operator ran `bash scripts/run_bench_gpu_cuda.sh` under WSL2 on the PC's RTX 4070 Ti SUPER, from a native copy
of the mirror at e834f58d, and kept the logs (`build/gpubench/README.txt`, `run.log`, `run-1-onesweep-crash.log` in
the development checkout, untracked). [The board](../bench/2026-11-01-repo-3-cuda-board.md) records both runs from
those files, as the operator asked, without a rerun:

- The first run stopped in `ckir_onesweep_bench`'s kernel profile with CUDA error 700 at the `clr+sb0` row, after
  the three sorts had been judged sorted permutations.
- The second, with 525c8417's program, ran all 17 programs to exit 0, every judge passing. Its ratios repeat the July
  boards: the fused MLP 2.42x cuBLASLt forward and 1.94x backward, flash attention 4.05x the unfused peer at
  S = 4,096 (and 0.57x at 512, as on AS-4), the onesweep sort 0.735x CUB, cuFFT within 2 % of its July rows.
- What the files lack is stated on the board: the host's load before and after.

The onesweep loss to CUB is the one the sort campaign closed with its gap itemized (the 2026-07-13 board); no row of
this track's scope owns a sort change, and REPO.3's clause is the route, which now holds on the GPU it was written for.

## 525c8417, audited

- The defect is older than this track: the July program timed `k_ghist_b` and `k_gbase` in the same order into
  histograms nothing cleared before the `clr+sb0` row, and its `solo` helper checked no launch. 3fd30fe7 (this
  track's failure path) made the board stop on it, which is how the operator found it. No board quotes a CUDA
  `clr+sb0` figure, so no recorded number was measured with the faulty scatter.
- The repair is complete: `k_sb0` (`ckir_onesweep_gen.cu`) reads every digit base from its third buffer, `gb`, which
  `k_gbase` writes from the histograms, and only the ticket (word 1,024) and the look-back cells from the auxiliary
  buffer; the timed `k_clear<<<2049, 256>>>` resets that buffer (2,049 x 2,048 words, past configuration B's
  4,195,332) and not `gb`. `k_gbase` timed alone is idempotent over fixed histograms. So the bases built once before
  the timed loop stay valid for every timed scatter, and `a`'s keys (not written by `k_sb0`) stay the ones they were
  built from.
- The repository checks at 819245c4 (which contains it): `check-allman-braces.py` (3,316 sources, `.cu` included)
  and `check-generated.py` (199 generated sources) pass. No hosted lane builds the CUDA programs
  (`CRD_BUILD_GPU_VS_CUDA` is off in CI), so the hosted half is the repository-checks jobs of the first complete run
  that covers it.

## Remaining on REPO.3

- The Qt report ([the draft](2026-11-01-repo-3-qfont-resolvefont.md#the-upstream-report); the user's, with a Qt
  account).
- The first complete run at a0c9d534 or later: the ASan lanes' `[shewchuk]` cases, the cold `win-debug-scalar`
  install with the local repository, the oracle with the tree's warnings on MSVC, clang-cl and the Linux Clang lanes
  (no `-Wdeprecated-non-prototype` line), and both repository-checks jobs over 525c8417; the Linux repository-checks
  job of 41298463221.
- The close after [REPO.3d](../ROADMAP.md#slice-repo.3d), which waits for track A's DIAG close.
