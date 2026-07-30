# REN-40-B — the INCREMENTAL EXTRACT WALK, measured

**Row 143 / 40-B in `docs/detours/D-007-gpu-program-system.md`.** Start line: the REN-40 baseline + the REN-40-A
device-cull board in `docs/bench/2026-07-29-ren40-million-instance-baseline.md`.

The only change is **how the renderer discovers what moved**. Nothing about what is drawn changes, on either arm.

## Machine / config

| | |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER |
| CPU | Intel i9-14900K |
| Build | `win-release` (MSVC 19.5x, `/O2 /GL /arch:AVX2 /fp:precise`) |
| Present | `--present immediate` (vsync off) |
| Clock | `--fixed-dt 16.6667` — the presented-frame counter drives time, so both arms sit at the same pose |
| Scene | `--instances N` grid at 2.0-unit spacing + 24 skinned foxes; ONE grid row bobs per frame |
| Assets | `CRD_ASSETS_DIR=<repo>/assets` — **required**; see "the footgun this board closed" below |

Harness: `crd-sandbox.exe --backend {vulkan|dx12} --present immediate --instances N --fixed-dt 16.6667
[--gpu-cull] --screenshot <f> --screenshot-at 8.0`, median of 3, reading the last steady-state `perf:` line.

## The board — `sync` is the column this slice moves

| backend | instances | arm | **sync** | of which extract | CPU render | GPU |
|---|---:|---|---:|---:|---:|---:|
| Vulkan | 100,000 | CPU cull | **0.25 ms** (was 3.85) | 0.10 | 43.33 | 22.15 |
| Vulkan | 100,000 | device cull | **0.24 ms** (was 3.85) | 0.10 | 31.10 | 22.36 |
| Vulkan | 1,000,000 | CPU cull | **1.11 ms** (was 80.20) | 0.89 | 268.95 | 86.36 |
| Vulkan | 1,000,000 | device cull | **1.17 ms** (was 76.74) | 0.96 | 117.89 | 86.93 |
| DX12 | 1,000,000 | CPU cull | **19.89 ms** (was 96.43) | 0.97 | 287.42 | 85.89 |
| DX12 | 1,000,000 | device cull | **20.64 ms** (was 98.12) | 0.64 | 116.83 | 85.94 |

**Extract at 1M: 171.4 ms (baseline) → 80.2 ms (40-A) → 0.9 ms.** The whole CPU frame at 1M on Vulkan, device
arm: **197.6 ms → 119.1 ms (1.66×)**, and what remains is almost entirely the wait on an 87 ms GPU.

## Why — the counters, not the milliseconds

The `perf:` line now carries `walk <chunks>c/<re-extracted>c/<entities>ent`. At 1M, every steady frame reads:

```
walk 9092c/11re/1203ent
```

9,092 chunks walked, **11** of them re-extracted, **1,203** of 1,000,000 entities touched — the sandbox moves one
grid row (1,000 cells) plus the monuments per frame, and that is exactly what it pays for. The two costs that
were O(entities) are gone:

1. **The structure signature** hashed `EntityId[n]` + `MeshRenderer[n]` byte by byte for every chunk every frame
   — 40 bytes per entity, ~40 MB of FNV at 1M, to answer a question that changes only on spawn/despawn. It is now
   5 u64 per CHUNK (key, count, first/last entity id, the MeshRenderer chunk version).
2. **Finding a moved chunk's runs** scanned every run of every group — O(chunks × runs), quadratic in the scene.
   A chunk index keyed on the chunk's entity-array pointer answers it in one probe, and the upload walks a DIRTY
   LIST instead of every run.

## The gate — counted, not timed

`tests/scene-render/test_scene_render.cpp`, two ctest cases. An asymptotic claim cannot be gated on a
millisecond threshold, so `SyncStats` grew four counters and the gate asserts the property:

| assertion (static frame, 20k instances) | value |
|---|---|
| `entities_extracted == 0` | not one entity re-read |
| `runs_visited == 0` | not one run scanned |
| `signature_bytes <= 64 × chunks_visited` | the signature is O(1) per chunk |
| `signature_bytes < total_instances` | **unreachable for any per-entity signature** |

**The gate was proven to bite.** Reverting `pass_signature` to the old byte hash (and accounting its bytes
honestly) turns both cases red:

```
signature_bytes <= 64 * chunks_visited   ->  801456 <= 11648   FAILED
signature_bytes <  total_instances       ->  801456 <  20000   FAILED
```

801 KB per static frame at 20k instances against 11.6 KB now — **69×**, and it scales linearly, which is the
40 MB/frame at 1M. The scaling case runs the same static frame at 8k and 32k and requires the signature to stay a
fixed 40 bytes per chunk at both sizes.

## Open, stated plainly

- **DX12 `sync` is 19–21 ms at 1M against Vulkan's 1.1 ms, and it is NOT extract** (extract is 0.64–0.97 ms on
  both). It is `upload`: 18.3–19.7 ms of per-frame storage uploads on DX12 versus 0.11 ms on Vulkan, for the same
  1,203 dirty instances. Pre-existing and previously buried under a 96 ms extract; now it is DX12's largest CPU
  term. Not investigated in this slice — filed, not fixed.
- **GPU ~87 ms at 1M is untouched**, as expected: this slice changes who discovers dirt, not what is drawn. That
  wall is LOD (40-C).
- One DX12 sample in three landed on a structural-rebuild frame (`walk 9092c/0re/1000027ent`, extract 128 ms) and
  was excluded from the median as a first-frame profile rather than a steady state. Whether a rebuild can be
  triggered mid-run is **not established** and deserves a look before the 40-H close.

## The footgun this board closed

The first device-cull run of this session reported **`gpu 0.344 ms` at one million instances**. That is not a
result, it is an empty canvas: `forward_csm_gpu.frame.toml` ships as a FILE, not in the built-in pack, so without
`CRD_ASSETS_DIR` the install logged one error line and the run continued with no cull passes, every indirect
command left at the reset's zero, and nothing drawn. `--gpu-cull` (and now `--lod`) **exit** when their asset does
not install. A performance arm that can silently measure an empty frame will eventually be quoted as a win.

## Verdict line (quote this)

> REN-40-B, 1M instances + 24 skinned, 4-cascade CSM, 4070 Ti SUPER: driving the extract from the chunk index
> instead of an O(entities) walk cuts `sync` **80.2 → 1.11 ms on Vulkan** and **96.4 → 19.9 ms on DX12**
> (extract itself 171.4 → 0.9 ms), taking the device-cull CPU frame from 197.6 to 119.1 ms. A steady frame walks
> 9,092 chunks, re-extracts 11, and touches 1,203 of 1,000,000 entities. Gated by counters, not timings, and the
> gate is proven to fail on the old walk (801 KB of signature hashing per static frame at 20k instances vs
> 11.6 KB). GPU time is unchanged at ~87 ms — that wall is LOD. DX12's remaining 19 ms of `sync` is its UPLOAD
> path, not its extract, and is an open bug.
