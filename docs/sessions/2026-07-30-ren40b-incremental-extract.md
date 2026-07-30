# 2026-07-30 — REN-40-B, and the shadow bug that four red gates were pointing at

**Row 143 / 40-A (audit) + 40-B (closed) + 40-C2 (data half) in `docs/detours/D-007-gpu-program-system.md`.
Board: `docs/bench/2026-07-30-ren40b-incremental-extract.md`.**

Brief: *audit 40-A, then implement 40-B and 40-C.*

---

## 1. The audit found a real shipped bug, and the handoff's diagnosis was wrong

`crd-scene-render-tests` was **28 passed / 4 failed** at HEAD. The prior handoff had bisected carefully, ruled
out five hypotheses correctly, and then concluded the wrong thing: *"the shadows-on and shadows-off arms are two
different shaders; the gates encode the pre-fix behaviour; re-derive the thresholds."* It also cited
`git show 9f41cbf -- engine/kir/include/crd/kir/ckir_technique.hpp` as evidence — a path that does not exist
under that spelling in that command's output, which is the first thing that did not survive checking.

**What was actually wrong.** REN-39-D3's scale-invariant texel bias recovers each cascade's world→clip scales
from its matrix:

```cpp
const int inv_radius = mxf(g.swizzle(col_x, 0), kf(1.0e-9)); // vp.c0.x
const int inv_range  = sub(kf(0.0), g.swizzle(col_z, 2));    // -vp.c2.z
```

`light_vp = ortho · light_view`, so **row** 0 is `(1/radius)·right` and **row** 2 is `−(1/range)·back`. The scale
is the row's NORM; a single element carries a direction cosine with it. Replaying the cascade fit numerically for
the two gate scenes (`csm.cpp` reimplemented in ~80 lines of Python, both lights, all four cascades) printed the
answer directly:

| scene | `right` | `back` | shipped `texel_w` | shipped bias | correct `texel_w` | correct bias |
|---|---|---|---:|---:|---:|---:|
| light (0,1,0) | (−1,0,0) | (0,1,0) | 1.95e6 | **0** | 0.008–0.19 | 7.5e-5 … 6.5e-4 |
| light (0.5,1,0) | (0,0,−1) | (0.447,0.894,0) | 1.95e6 | **0** | 0.010–0.24 | 9.1e-5 … 6.9e-4 |

`vp.c0.x` was NEGATIVE in one scene and exactly ZERO in the other; both clamped to the `1e-9` floor, so
`texel_w` became 1.95e6 world units and the normal offset **2.2 million units** — every shadow lookup landed
outside every cascade, `any` fell to 0, and the containment fallback declared the pixel LIT. And `−vp.c2.z` was
`(1/range)·back.z = 0` in both, so the depth bias was identically zero.

⛔ **The grazing-angle acne that change was written to kill did vanish — because the shadow vanished with it.**
That is why it read as a success and shipped.

Fixed with `g.vlength(row0)` / `g.vlength(row2)`. **35/35 green, no threshold touched.** The correct derivation
already existed one file away: `csm.cpp::recover_camera` recovers the camera's projection scales as row lengths
for exactly this reason. Memory: [[feedback_matrix_element_is_not_a_scale_use_the_row_norm]].

**A second false claim, removed.** `kSceneRegionSlack = 8` carried a comment saying that deriving the region base
without it "moved the first region from 384 to 376 and **broke four shadow gates**". It did not — those gates
were red for the reason above. Setting the slack to 0 and re-running: 33/33. The constant is gone.

**The rest of 40-A is real**, and re-verified: 5 device gates green on both backends, and under
`--gpu-cull-verify` all five views' survivor counts match the CPU's exactly (camera 6714 == 6714).

## 2. REN-40-B — the extract walk

`sync` at 1M: **80.2 → 1.11 ms on Vulkan** (extract itself 171.4 → 0.9 ms). Two O(entities) costs, both deleted:

1. **The structure signature** hashed `EntityId[n]` + `MeshRenderer[n]` byte by byte for every chunk every frame
   — 40 B per entity, ~40 MB of FNV at 1M — to answer a question that changes only on spawn/despawn. It is now
   5 u64 per CHUNK. Still exact: insert and archetype-move bump the destination chunk's `version_counter`, a
   swap-remove changes the count, the first/last entity ids catch a recycled chunk address. ⛔ The Transform
   version is deliberately excluded — folding it in would make every moving frame structural.
2. **Finding a moved chunk's runs** scanned every run of every group (O(chunks × runs)). A chunk index keyed on
   the chunk's entity-array pointer answers it in one probe; the upload walks a dirty list.

**Gated by counters, not timings.** An asymptotic claim cannot be gated on a millisecond threshold, so
`SyncStats` grew `chunks_visited / chunks_reextracted / entities_extracted / signature_bytes / runs_visited` and
the static-frame gate asserts `entities_extracted == 0`, `runs_visited == 0`,
`signature_bytes <= 64 × chunks_visited`, and `signature_bytes < total_instances` — the last unreachable for any
per-entity signature. **The gate was proven to bite**: reverting `pass_signature` turns it red at 801,456 bytes
against 11,648.

## 3. ⛔⛔ The benchmark footgun that would have produced a false board

The first `--gpu-cull` run of the session reported **`gpu 0.344 ms` at one million instances**. That is not a
result; it is an empty canvas. `forward_csm_gpu.frame.toml` ships as a FILE and is not in the built-in pack, so
without `CRD_ASSETS_DIR` the install logs one `[ERR]` line and the run continues — no cull passes, every indirect
command at the reset's zero, nothing drawn, and a spectacular number. `--gpu-cull` and `--lod` now **exit**.

This is the same shape as the `bounds_off = 104` scar one level up: the failure produced a plausible artifact
with a clean log. The check that caught it was the same one that caught that: `--gpu-cull-verify` comparing the
DEVICE's per-view counts against the CPU's, which read `gpu=0 cpu=6714` five times.

## 4. REN-40-C2 — the data half, and the decision the selection half needs

**Nothing in the engine or the sandbox ever called `set_lod_policy_asset`.** C1's chain builder was unreachable
code — the unreachable-library rule one level down. `--lod [asset]` now installs the policy before the first sync
and the run reports what it built: `5/7 groups have one, max 4 levels, 7104 → 594 tris`.

⛔ It carried a latent buffer overlap: the group buffer sized its INDEX SECTION by `group.index_count` (level 0's
DRAW count) while `mesh->indices` holds the whole chain end to end, so the VERTEX section was laid on top of
levels 1..n. It would have surfaced as a coarse LOD drawing garbage at a distance — i.e. as a decimator bug.
Fixed; with chains built the 20k frame is **bit-identical** (0/921600, max delta 0) to the no-chain frame.

**The selection half is blocked on an architectural decision, not on unknown work.** The row says "one draw item
per (group, view, slot)". But a frame-graph pass binds ONE program for its whole draw list
(`IFrameHost::instance_program` is per-pass), while each slot's VS must read a different visible list
(`visible_off + capacity * ((1 + cascade) * slots + slot)`). So the slot cannot be a per-item property today.
Two ways out:

- **(a)** `slots` passes per view in the authored graph, each with a cook-time `lod_slot` VS variant — 4 forward
  + 16 cascade passes at 4 slots. Fully in the authored-frame-graph spirit.
- **(b)** widen the draw-table row from one word to two, so the VS reads its list base from `table[DrawIndex]`
  exactly as it already reads its region base. One program serves every (view, slot). **Recommended** — it is
  how GPU-driven renderers do this — at the cost of touching the rebase contract every VS uses.

Everything downstream is scoped in the 40-C row: the `CullDesc` fields, the params-block growth (4 → 8 words to
carry each view's pixel height), the reset kernel's (view × slot) unroll off the LOD table, and the selector
`px = r · |row1(vp)| · pixel_height / max(w, eps)` — one formula correct for both the perspective camera and the
ortho cascades. ⛔ `|row1|` is the ROW NORM; reading `vp.c1.y` there is exactly the defect in §1.

## Gates

| suite | result |
|---|---|
| `crd-scene-render-tests` | **35/35** (was 28/32) — includes the two new REN-40-B cases |
| `crd-lod-tests` | 8/8 |
| `crd-vertex-cook-tests` | 24/24 |
| `crd-kir-tests` | 262/262 |
| `crd-technique-cook-tests` | 5/5 |
| `crd-kir-vulkan-tests` | 31/31 |
| `crd-geometry-mesh-processing-tests` | 86/86 |
| `[ren40]` device gates | 3/3 Vulkan, 2/2 DX12 |
| clang-tidy (LLVM 20, warnings-as-errors) | clean on every touched file except `sandbox/src/main.cpp` |

## Still open, named rather than buried

- **DX12 `sync` is 19–21 ms at 1M and it is NOT extract** (0.64–0.97 ms, the same as Vulkan). It is
  `upload_storage`: 18.3–19.7 ms against Vulkan's 0.11 for the same 1,203 dirty instances. Previously buried
  under a 96 ms extract; now DX12's largest CPU term.
- **`sandbox/src/main.cpp` fails `readability-function-size` on `main`** — pre-existing at HEAD (verified by
  stashing the diff and re-running the gate). It needs a decomposition, not a suppression. The two nested-ternary
  findings in the same file were fixed.
- **One DX12 sample in three landed on a structural-rebuild frame** mid-run (`walk 9092c/0re/1000027ent`).
  Excluded from the median as a first-frame profile; whether a rebuild can genuinely trigger mid-run is
  unestablished and worth a look before 40-H.
- **GPU ~87 ms at 1M is untouched.** That wall is LOD, and it is 40-C2's selection half.
- No 18-config sweep was run; this is not a cluster close.
