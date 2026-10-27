# User decision: the computational geometry backbone runs now (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Decision

Computational geometry is a backbone system used everywhere, so it must not be Later or long range. On 2026-10-27 the
user placed it directly after the RAH block, before every other later system. The [GEOM](../ROADMAP.md#slice-geom)
block there holds FOUNDATION-AUDIT and SCI-CONTRACT.1 (the geometry kernel needs the frame, point and vector contract),
GEOM-K.0 to GEOM-K.4, GEOM-EXACT.1 and .2, GEOM-QUAL, NAV.0 and NAV.3, and the new GEOM parent. NAV.1 and NAV.2 follow
the CGP family (NAV.1 voxelizes through CGP-3); NAV.4 to NAV.7 keep their physics and large-world dependencies.

## What the backbone builds on

- The user's earlier statements (2026-10-26): geometry is a full computational geometry base, including the integer and
  lattice family and navigation; every system uses it; crd-hesap stays the numerical base underneath; nothing is
  duplicated.
- The existing modules in `engine/geometry` (BVH, BVH GPU, convex, curves, decomposition, Delaunay, mesh, mesh
  processing, polygon, primitives, shader helpers, spatial, viz, LOD, meshgen), which GEOM-K.3 retargets to the kernel.
- [ADR-0076](../decisions/0076-geometry-substrate-architecture.md) and the geometry areas of the
  [2026 research](../research/2026-10-26-state-of-the-art-2026-comparison.md).
