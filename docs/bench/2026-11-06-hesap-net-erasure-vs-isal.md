# 2026-11-06 — HESAP-NET: hesap-comms' Reed-Solomon code against Intel ISA-L

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [ISA-L board](../sessions/2026-11-06-hesap-net-erasure-board.md).

## Machine and arms

- Intel Core i9-14900K (AVX2, GFNI; no AVX-512), WSL2 Ubuntu 24.04, one P-core thread (`--cpus 6`), GCC 13.3.0, the
  `linux-gcc-release` tree (-O3, link-time optimisation; kernels and ISA-L's objects without it) with
  `-DCRD_BUILD_ERASURE_VS_ISAL=ON`, ISA-L 2.32.1, NASM 2.16.01.
- CPU level lines (`cpu_level_record`): `cpu detected=avx2 selected=avx2 override=none
  extra=aes,pclmulqdq,xsave,sha,gfni,vaes,vpclmulqdq,avx-vnni,invariant-tsc kernel_cap=avx512` (the **cerid-gfni** arm,
  table Avx2Gfni) and `cpu detected=avx2 selected=avx2 override=applied kernel_cap=avx512` under `CRD_CPU_LEVEL=avx2`
  (the **cerid-avx2** arm, plain Avx2).
- ISA-L's arms, in one process with hesap's: **isal-dispatched** (`ec_encode_data`, its own dispatch; the reference),
  **isal-avx2-gfni**, **isal-avx2**, **isal-base** (`ec_encode_data_base`). The sse and avx bodies run in the board too
  and are not tabled.
- Work, rates in MB/s of source data: `encode` makes r repair symbols from what an application keeps per shape (hesap's
  `prepare_encode` rows, ISA-L's tables); `encode-cold` forms those in every call; `decode` recovers r lost sources with
  the whole per-pattern work on each side (hesap's barycentric weights; ISA-L's `gf_invert_matrix`, table build and
  encode). After every batch the outputs are compared with the reference bytes; a mismatch exits 1.
- avx512 throughput is not measured: this CPU has none and SDE is an emulator. SDE (icx, skx) proves those bodies' bytes
  only ([session](../sessions/2026-11-06-hesap-net-erasure-board.md)); the real measurement is the [AVX-512 board](2026-11-07-hesap-net-avx512.md).

## Harness

```
B=$TREE/tests/hesap-net/crd-hesap-erasure-board
python3 scripts/ab-board.py --cmd gfni="$B --rounds 5" --cmd avx2="env CRD_CPU_LEVEL=avx2 $B --rounds 5" \
  --arm cerid-gfni='gfni:^cerid-avx2-gfni (?P<key>\S+) (?P<value>\S+)$' \
  --arm cerid-avx2='avx2:^cerid-avx2 (?P<key>\S+) (?P<value>\S+)$' \
  --arm isal-dispatched='gfni:^isal-dispatched (?P<key>\S+) (?P<value>\S+)$' \
  --arm isal-avx2-gfni='gfni:^isal-avx2-gfni (?P<key>\S+) (?P<value>\S+)$' \
  --arm isal-avx2='gfni:^isal-avx2 (?P<key>\S+) (?P<value>\S+)$' \
  --arm isal-base='gfni:^isal-base (?P<key>\S+) (?P<value>\S+)$' \
  --reference isal-dispatched --higher-is-better --cpus 6 --warmup 1 --min-reps 5 --max-reps 5
```

Host before (`build-bench.py host`): Linux 30.2 of 31.2 GiB free, no other load; Windows 4.3 of 63.7 GiB free, 7.8 cores
of other load (a game, 3 cores; a Windows build during part of the run): contended, so the intervals are wide and only
the interleaved ratios are read. The Linux side after: unchanged.

## Verdict

Where the CPU has GFNI, hesap-comms beats ISA-L 2.32.1's own dispatch on every prepared encode except k=4, r=2, 1200 bytes,
which the interval does not separate (0.89 [0.82-1.10]), and on decode (medians 1.3x to 13.7x: barycentric weights against
Gauss-Jordan; only 1 MiB's interval straddles 1 on this contended host). Cold encode stays behind at k=4, r=2 (0.78): each call pays hesap's O(k) coefficient formulas against
ISA-L's table build, and an encoder of many blocks uses the prepared path. Without GFNI (cerid-avx2) the plain nibble
kernel is level with isal-avx2 on encode (tables below); both are about half the GFNI rate. isal-base is ISA-L's portable floor.

## Board

5 repetitions (the maximum of 5 repetitions), 1 warm-up, CPUs 6, band 2%; speed ratio = arm rate / reference rate, median [bootstrap 95%].

cerid-gfni against isal-dispatched:

| key | isal-dispatched | cerid-gfni | speed ratio | verdict |
|---|---|---|---|---|
| encode-k4-r2-1200 | 74339 | 65596 | 0.8919 [0.8206-1.101] | unresolved |
| encode-cold-k4-r2-1200 | 68121 | 50068 | 0.7758 [0.6781-0.9144] | slower |
| decode-k4-r2-1200 | 28226 | 40178 | 1.499 [1.277-1.597] | faster |
| encode-k10-r4-1200 | 34303 | 46650 | 1.415 [1.344-1.481] | faster |
| encode-cold-k10-r4-1200 | 31787 | 34294 | 1.041 [0.9543-1.178] | unresolved |
| decode-k10-r4-1200 | 8754 | 25393 | 2.721 [2.426-3.021] | faster |
| encode-k10-r4-1500 | 34166 | 49260 | 1.428 [1.312-1.887] | faster |
| encode-cold-k10-r4-1500 | 32337 | 34295 | 1.062 [0.9364-1.188] | unresolved |
| decode-k10-r4-1500 | 10590 | 27271 | 2.688 [2.084-2.858] | faster |
| encode-k32-r8-1200 | 16719 | 21376 | 1.239 [1.123-1.494] | faster |
| encode-cold-k32-r8-1200 | 15559 | 15204 | 0.9774 [0.8394-1.073] | unresolved |
| decode-k32-r8-1200 | 1469 | 10620 | 7.022 [6.454-8.478] | faster |
| encode-k64-r16-1200 | 8128 | 10591 | 1.338 [1.256-1.603] | faster |
| encode-cold-k64-r16-1200 | 7316 | 7096 | 0.9734 [0.9012-1.003] | unresolved |
| decode-k64-r16-1200 | 389.6 | 5000 | 13.65 [9.869-14.78] | faster |
| encode-k10-r4-65536 | 26435 | 36738 | 1.385 [1.295-1.517] | faster |
| encode-cold-k10-r4-65536 | 25845 | 33768 | 1.309 [1.235-1.537] | faster |
| decode-k10-r4-65536 | 25383 | 34301 | 1.305 [1.257-1.511] | faster |
| encode-k10-r4-1048576 | 19523 | 32449 | 1.594 [1.164-2.054] | faster |
| encode-cold-k10-r4-1048576 | 20439 | 29606 | 1.338 [1.121-1.948] | faster |
| decode-k10-r4-1048576 | 20991 | 31257 | 1.574 [0.8377-1.73] | unresolved |

cerid-avx2 against isal-dispatched:

| key | isal-dispatched | cerid-avx2 | speed ratio | verdict |
|---|---|---|---|---|
| encode-k4-r2-1200 | 74339 | 30780 | 0.4613 [0.3963-0.4733] | slower |
| encode-cold-k4-r2-1200 | 68121 | 26688 | 0.448 [0.3706-0.5237] | slower |
| decode-k4-r2-1200 | 28226 | 26856 | 0.9515 [0.7861-1.028] | unresolved |
| encode-k10-r4-1200 | 34303 | 20059 | 0.6214 [0.5243-0.6422] | slower |
| encode-cold-k10-r4-1200 | 31787 | 15590 | 0.4855 [0.4693-0.6053] | slower |
| decode-k10-r4-1200 | 8754 | 14719 | 1.539 [1.456-1.708] | faster |
| encode-k10-r4-1500 | 34166 | 20290 | 0.5605 [0.5338-0.6825] | slower |
| encode-cold-k10-r4-1500 | 32337 | 16888 | 0.5222 [0.4416-0.6773] | slower |
| decode-k10-r4-1500 | 10590 | 14529 | 1.351 [1.268-1.68] | faster |
| encode-k32-r8-1200 | 16719 | 9819 | 0.5913 [0.4702-0.6431] | slower |
| encode-cold-k32-r8-1200 | 15559 | 7984 | 0.5171 [0.4432-0.5787] | slower |
| decode-k32-r8-1200 | 1469 | 6404 | 4.36 [3.954-4.485] | faster |
| encode-k64-r16-1200 | 8128 | 4760 | 0.5707 [0.5219-0.6274] | slower |
| encode-cold-k64-r16-1200 | 7316 | 3754 | 0.5037 [0.4657-0.5689] | slower |
| decode-k64-r16-1200 | 389.6 | 3028 | 7.561 [6.259-8.883] | faster |
| encode-k10-r4-65536 | 26435 | 15549 | 0.5882 [0.5037-0.6803] | slower |
| encode-cold-k10-r4-65536 | 25845 | 16379 | 0.6392 [0.5674-0.7425] | slower |
| decode-k10-r4-65536 | 25383 | 15554 | 0.5941 [0.4968-0.6968] | slower |
| encode-k10-r4-1048576 | 19523 | 16946 | 0.8236 [0.5647-1.094] | unresolved |
| encode-cold-k10-r4-1048576 | 20439 | 16522 | 0.7838 [0.5621-0.9544] | slower |
| decode-k10-r4-1048576 | 20991 | 16645 | 0.793 [0.774-1.007] | unresolved |

isal-avx2-gfni against isal-dispatched:

| key | isal-dispatched | isal-avx2-gfni | speed ratio | verdict |
|---|---|---|---|---|
| encode-k4-r2-1200 | 74339 | 70629 | 0.9234 [0.9122-1.236] | unresolved |
| encode-cold-k4-r2-1200 | 68121 | 67002 | 0.9936 [0.966-1.208] | unresolved |
| decode-k4-r2-1200 | 28226 | 28322 | 1.011 [0.9435-1.126] | unresolved |
| encode-k10-r4-1200 | 34303 | 36409 | 1.061 [0.9933-1.168] | unresolved |
| encode-cold-k10-r4-1200 | 31787 | 31770 | 1.001 [0.9273-1.103] | unresolved |
| decode-k10-r4-1200 | 8754 | 9685 | 1.115 [0.9381-1.2] | unresolved |
| encode-k10-r4-1500 | 34166 | 33990 | 1.039 [0.8738-1.078] | unresolved |
| encode-cold-k10-r4-1500 | 32337 | 31303 | 0.9599 [0.8537-1.186] | unresolved |
| decode-k10-r4-1500 | 10590 | 10676 | 1.017 [0.9019-1.046] | unresolved |
| encode-k32-r8-1200 | 16719 | 15846 | 0.9469 [0.8359-1.109] | unresolved |
| encode-cold-k32-r8-1200 | 15559 | 15324 | 0.9849 [0.7978-1.046] | unresolved |
| decode-k32-r8-1200 | 1469 | 1418 | 0.9657 [0.9499-1.049] | unresolved |
| encode-k64-r16-1200 | 8128 | 7666 | 0.9612 [0.9263-0.9923] | unresolved |
| encode-cold-k64-r16-1200 | 7316 | 7366 | 1.01 [0.9107-1.072] | unresolved |
| decode-k64-r16-1200 | 389.6 | 365.5 | 0.9406 [0.8459-1.042] | unresolved |
| encode-k10-r4-65536 | 26435 | 24752 | 0.9559 [0.8887-1.144] | unresolved |
| encode-cold-k10-r4-65536 | 25845 | 26158 | 1.012 [0.8618-1.095] | unresolved |
| decode-k10-r4-65536 | 25383 | 24914 | 0.9244 [0.8966-0.9923] | unresolved |
| encode-k10-r4-1048576 | 19523 | 19224 | 0.9847 [0.8419-1.215] | unresolved |
| encode-cold-k10-r4-1048576 | 20439 | 18969 | 0.8077 [0.7935-1.113] | unresolved |
| decode-k10-r4-1048576 | 20991 | 18222 | 0.8379 [0.7143-1.209] | unresolved |

isal-avx2 against isal-dispatched:

| key | isal-dispatched | isal-avx2 | speed ratio | verdict |
|---|---|---|---|---|
| encode-k4-r2-1200 | 74339 | 33355 | 0.458 [0.404-0.4809] | slower |
| encode-cold-k4-r2-1200 | 68121 | 30468 | 0.4563 [0.4361-0.5349] | slower |
| decode-k4-r2-1200 | 28226 | 17351 | 0.6062 [0.5679-0.6897] | slower |
| encode-k10-r4-1200 | 34303 | 18938 | 0.577 [0.547-0.6553] | slower |
| encode-cold-k10-r4-1200 | 31787 | 17247 | 0.5779 [0.5156-0.6411] | slower |
| decode-k10-r4-1200 | 8754 | 6942 | 0.7727 [0.6772-0.8421] | slower |
| encode-k10-r4-1500 | 34166 | 22587 | 0.6692 [0.6099-0.7307] | slower |
| encode-cold-k10-r4-1500 | 32337 | 17372 | 0.5298 [0.4932-0.6107] | slower |
| decode-k10-r4-1500 | 10590 | 8423 | 0.8027 [0.7069-0.8519] | slower |
| encode-k32-r8-1200 | 16719 | 10039 | 0.607 [0.5506-0.6331] | slower |
| encode-cold-k32-r8-1200 | 15559 | 8598 | 0.5417 [0.4951-0.5857] | slower |
| decode-k32-r8-1200 | 1469 | 1336 | 0.925 [0.8775-0.9856] | unresolved |
| encode-k64-r16-1200 | 8128 | 5524 | 0.6797 [0.6381-0.7235] | slower |
| encode-cold-k64-r16-1200 | 7316 | 4137 | 0.5636 [0.5503-0.614] | slower |
| decode-k64-r16-1200 | 389.6 | 373.5 | 0.9204 [0.3535-0.9743] | slower |
| encode-k10-r4-65536 | 26435 | 19023 | 0.7476 [0.5587-0.7529] | slower |
| encode-cold-k10-r4-65536 | 25845 | 18294 | 0.6863 [0.6758-0.7455] | slower |
| decode-k10-r4-65536 | 25383 | 16548 | 0.6516 [0.5849-0.7737] | slower |
| encode-k10-r4-1048576 | 19523 | 17606 | 0.9307 [0.7497-0.9866] | unresolved |
| encode-cold-k10-r4-1048576 | 20439 | 18320 | 0.8842 [0.6386-0.9743] | slower |
| decode-k10-r4-1048576 | 20991 | 17580 | 0.9189 [0.5872-0.9526] | slower |

isal-base against isal-dispatched:

| key | isal-dispatched | isal-base | speed ratio | verdict |
|---|---|---|---|---|
| encode-k4-r2-1200 | 74339 | 612.5 | 0.008239 [0.006952-0.01109] | slower |
| encode-cold-k4-r2-1200 | 68121 | 665.9 | 0.01053 [0.009441-0.01123] | slower |
| decode-k4-r2-1200 | 28226 | 666.4 | 0.02405 [0.02154-0.02763] | slower |
| encode-k10-r4-1200 | 34303 | 391.8 | 0.01146 [0.01134-0.01203] | slower |
| encode-cold-k10-r4-1200 | 31787 | 373.1 | 0.0119 [0.01121-0.0125] | slower |
| decode-k10-r4-1200 | 8754 | 342.3 | 0.03659 [0.03374-0.04178] | slower |
| encode-k10-r4-1500 | 34166 | 353.6 | 0.009694 [0.009409-0.01253] | slower |
| encode-cold-k10-r4-1500 | 32337 | 380 | 0.01156 [0.009395-0.01323] | slower |
| decode-k10-r4-1500 | 10590 | 340.4 | 0.03168 [0.02986-0.03377] | slower |
| encode-k32-r8-1200 | 16719 | 186.1 | 0.0107 [0.009263-0.01164] | slower |
| encode-cold-k32-r8-1200 | 15559 | 194.3 | 0.01204 [0.01083-0.01364] | slower |
| decode-k32-r8-1200 | 1469 | 172.1 | 0.1172 [0.105-0.1351] | slower |
| encode-k64-r16-1200 | 8128 | 87 | 0.01082 [0.0102-0.01109] | slower |
| encode-cold-k64-r16-1200 | 7316 | 91.5 | 0.01251 [0.01175-0.01272] | slower |
| decode-k64-r16-1200 | 389.6 | 75.2 | 0.193 [0.1688-0.2158] | slower |
| encode-k10-r4-65536 | 26435 | 337.3 | 0.01226 [0.01151-0.01395] | slower |
| encode-cold-k10-r4-65536 | 25845 | 327.9 | 0.01235 [0.01172-0.01372] | slower |
| decode-k10-r4-65536 | 25383 | 303.4 | 0.01226 [0.01084-0.01332] | slower |
| encode-k10-r4-1048576 | 19523 | 316.6 | 0.01615 [0.01391-0.01831] | slower |
| encode-cold-k10-r4-1048576 | 20439 | 320.2 | 0.0152 [0.01363-0.01707] | slower |
| decode-k10-r4-1048576 | 20991 | 311.4 | 0.01562 [0.01371-0.01692] | slower |
