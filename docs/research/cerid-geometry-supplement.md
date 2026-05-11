# Cerid — `crd-geometry` substrate research SUPPLEMENT

**Date:** 2026-05-11
**Extends:** `docs/research/cerid-geometry.md` (11,523 words; closed
2026-05-11) and ADR-0076 (Accepted 2026-05-11).
**Phase plan extended:** `docs/phases/phase-3.1.7-geometry.md`.
**Companion:** `docs/research/cerid-sdf.md` (Phase 3.1.5) — the SDF
substrate this supplement explicitly cross-pollinates with.

> Supplement document. The base dossier is the source-of-truth for the
> *what* and *why* of every algorithm + sub-module + slot decision.
> This supplement adds three orthogonal dimensions the base dossier
> deliberately did not cover. Reading the base first is required for
> context.

---

## 1. Why this supplement exists

The base dossier locked the substrate architecture, the 10 sub-module
split, the ~25-slice plan, and the Phase 3.1.7 slot. It did *not*
catalog the canonical analytic-distance reference literature (Inigo
Quilez et al.), it did *not* enumerate the academic textbook
foundation behind each slice, and it did *not* commit to a concrete
SIMD strategy + `crd::containers` integration contract per
sub-module. Three asks from the user surfaced these gaps:

1. **iq's body of work + similar SDF/distance-function libraries** —
   the canonical published collection of analytic point-to-shape
   distance formulas, applicable simultaneously to
   `crd-geometry-primitives` (CPU analytic queries) and a future
   GPU-side GLSL/HLSL helper library shared between `crd-geometry`
   and `crd-sdf`.
2. **Computational-geometry textbook foundation** — the canonical
   academic texts that ground the algorithms we ship, with each book
   mapped to the slice/sub-module it most informs.
3. **Performance contract — SIMD + container integration per
   sub-module** — explicit per-sub-module SIMD lane-width choice,
   AoSoA layout decisions, and the `crd::containers` /
   `crd::math::simd::Soa` substrate commitment.

**Non-goal — what this supplement does NOT do:** does not re-survey
Bullet/PhysX/Embree/CGAL/Geogram (base dossier §3); does not re-list
the algorithmic scope (base §4); does not re-derive the slice plan
(base §8); does not re-make the Phase 3.1.7 slot decision (base §10);
does not edit ADR-0076 or the phase plan (it *proposes* edits in §6
that the user can later apply). It adds reference + performance +
shader-bridge content the base dossier deliberately scoped out.

Where this supplement proposes a structural addition (the
`crd-geometry-shader-helpers` 11th sub-module, the SIMD-specific
substrate primitives, the per-sub-module performance budgets), §6
collects them as concrete, applyable edits to ADR-0076 and the phase
plan.

---

## 2. Inigo Quilez + the SDF/distance-function literature

### 2.1 Inigo Quilez's contributions

**Background.** Inigo Quilez (iq) was a senior graphics engineer at
Pixar (2014–2016) and Oculus / Meta Reality Labs (2016–present). He
co-founded **Shadertoy** (shadertoy.com) in 2013 with Pol Jeremias —
the canonical web-based GLSL fragment-shader sandbox where the
modern raymarching-of-SDFs aesthetic was, in large part, invented
and refined publicly. His personal site **iquilezles.org** publishes
~80 articles spanning analytic distance functions, sphere tracing,
smooth blending, domain manipulation, soft shadows, ambient
occlusion, fractals, intersection formulas, palette construction,
and procedural noise.

For Cerid's purposes, the iquilezles.org articles split into three
buckets:

**Bucket A — analytic distance functions (directly relevant to
`crd-geometry-primitives`).**

- *"distance functions"* (3D) — the canonical reference, ~30
  primitive shapes: sphere, box, round-box, torus, capped-torus,
  link, capsule, cylinder, capped-cylinder, infinite-cylinder, cone,
  capped-cone, infinite-cone, plane, hexagonal-prism, triangular-
  prism, vertical-capsule, vertical-cylinder, line-segment,
  ellipsoid, octahedron, pyramid, rhombus, solid-angle, cut-sphere,
  cut-hollow-sphere, death-star, round-cone (two-radius), round-cone-
  capped, ellipsoid (bound), revolved-vesica.
- *"distance functions 2D"* — ~25 2D shapes including circle, box,
  oriented-box, segment, rhombus, isosceles-trapezoid, parallelogram,
  triangle (equilateral + isosceles + general), uneven-capsule,
  regular-pentagon, regular-hexagon, regular-octagon, hexagram,
  star-5, regular-star, pie, arc, horseshoe, vesica, oriented-vesica,
  moon, rounded-cross, ellipse, parabola, parabola-segment, blobby-
  cross, tunnel.
- *"intersectors"* — analytic ray-vs-shape intersection (sphere,
  ellipsoid, box, plane, disk, hexagonal-prism, capsule, cylinder,
  capped-cylinder, torus, triangle, sphere4, etc.) — directly
  relevant to `crd-geometry-primitives` raycast intersection tests.
- *"ellipsoid functions"* — closed-form gradient + bound formulas.
- *"distance-to-curve"* — Bezier / quadratic / cubic distance.
- *"sdf bounding volumes"* — analytic AABB derivation per primitive.

**Bucket B — operators for combining and manipulating SDFs (directly
relevant to the future shader-helper library + `crd-sdf` ops).**

- *"smooth minimum"* — three operator families:
  - **Polynomial smin (cubic / quadratic)**:
    `float h = clamp(0.5 + 0.5*(b - a)/k, 0.0, 1.0);`
    `return mix(b, a, h) - k*h*(1.0 - h);`
  - **Exponential smin**:
    `return -log(exp(-k*a) + exp(-k*b)) / k;`
    (more expensive, smoother, generalises to N-way smin trivially:
    `-log(sum(exp(-k*x_i)))/k`)
  - **Power smin**: `pow(pow(a, -k) + pow(b, -k), -1/k)` — generalises
    to smooth-max via sign flip.
- *"smooth boolean ops"* — `opSmoothUnion`, `opSmoothSubtraction`,
  `opSmoothIntersection` — the smin-plus-clamp triad.
- *"domain repetition"* — `vec3 q = mod(p + 0.5*c, c) - 0.5*c;`
  infinite tiling of an SDF.
- *"limited domain repetition"* — `vec3 q = p - c*clamp(round(p/c),
  -l, l);` finite-count tiling.
- *"domain mirroring / symmetry"* — `p.x = abs(p.x);` exploits
  bilateral symmetry, then evaluate one half.
- *"domain warping"* — twisting, bending, displacement.
  `mat2 m = mat2(cos(k*p.y), -sin(k*p.y), sin(k*p.y), cos(k*p.y));`
  for twist; analogous for bend / non-linear scaling.
- *"position-from-distance"* — gradient extraction via finite
  differences:
  `vec3 n = normalize(vec3(d(p+e.xyy)-d(p-e.xyy), d(p+e.yxy)-d(p-e.yxy), d(p+e.yyx)-d(p-e.yyx)));`

**Bucket C — rendering algorithms over SDFs (relevant to a future
`crd-sdf` GPU sampler + `crd-renderer` future DFAO/DFGI passes —
LISTED but EXPLICITLY OUT OF SCOPE for `crd-geometry-shader-helpers`).**

- *"raymarching"* — the canonical sphere-tracer outer loop.
- *"soft shadows"* — distance-function shadow rays.
- *"ambient occlusion"* — distance-function AO.
- *"interior" / "menger-sponge" / "mandelbox" / "mandelbulb"* —
  fractal SDFs (research-tier; reserved).
- *"palette"* + *"easing"* — colour utilities (out of scope).

**Concrete formulas that `crd-geometry-primitives` v0 must ship with
direct attribution to iquilezles.org.** Each formula appears below in
its GLSL form (the canonical published form on iq's site); the C++
side ships the same expressions over `crd::math::Vec3f` (and over
`crd::math::simd::Vec4f`/`Vec8f` lane-batches per §4.1):

```glsl
// Sphere.       iquilezles.org / distance functions.
float sdSphere(vec3 p, float r) { return length(p) - r; }

// Box (exterior + interior, branch-free).  iquilezles.org / distance functions.
float sdBox(vec3 p, vec3 b) {
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Round box.   iquilezles.org / distance functions.
float sdRoundBox(vec3 p, vec3 b, float r) {
    vec3 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

// Torus (major radius t.x, minor radius t.y).  iquilezles.org.
float sdTorus(vec3 p, vec2 t) {
    vec2 q = vec2(length(p.xz) - t.x, p.y);
    return length(q) - t.y;
}

// Capsule between endpoints a,b with radius r.  iquilezles.org.
float sdCapsule(vec3 p, vec3 a, vec3 b, float r) {
    vec3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba*h) - r;
}

// Vertical capsule: half-length h, radius r, axis = +y.  iquilezles.org.
float sdVerticalCapsule(vec3 p, float h, float r) {
    p.y -= clamp(p.y, 0.0, h);
    return length(p) - r;
}

// Cylinder (axis +y, height 2h, radius r).  iquilezles.org.
float sdCappedCylinder(vec3 p, float h, float r) {
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(r, h);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

// Cone (axis +y, half-angle encoded in c = (sin, cos), height h).  iquilezles.org.
float sdCone(vec3 p, vec2 c, float h) {
    vec2 q = h*vec2(c.x/c.y, -1.0);
    vec2 w = vec2(length(p.xz), p.y);
    vec2 a = w - q*clamp(dot(w, q)/dot(q, q), 0.0, 1.0);
    vec2 b = w - q*vec2(clamp(w.x/q.x, 0.0, 1.0), 1.0);
    float k = sign(q.y);
    float d = min(dot(a, a), dot(b, b));
    float s = max(k*(w.x*q.y - w.y*q.x), k*(w.y - q.y));
    return sqrt(d)*sign(s);
}

// Hex prism (axis +y, half-extents h.xy = (radius, height)).  iquilezles.org.
float sdHexPrism(vec3 p, vec2 h) {
    const vec3 k = vec3(-0.8660254, 0.5, 0.57735);
    p = abs(p);
    p.xy -= 2.0*min(dot(k.xy, p.xy), 0.0)*k.xy;
    vec2 d = vec2(length(p.xy - vec2(clamp(p.x, -k.z*h.x, k.z*h.x), h.x))*sign(p.y - h.x), p.z - h.y);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

// Ellipsoid (bound — not an exact SDF, but tight + cheap).  iquilezles.org.
float sdEllipsoid(vec3 p, vec3 r) {
    float k0 = length(p / r);
    float k1 = length(p / (r*r));
    return k0*(k0 - 1.0) / k1;
}

// Octahedron (exact).  iquilezles.org.
float sdOctahedron(vec3 p, float s) {
    p = abs(p);
    float m = p.x + p.y + p.z - s;
    vec3 q;
    if (3.0*p.x < m)      q = p.xyz;
    else if (3.0*p.y < m) q = p.yzx;
    else if (3.0*p.z < m) q = p.zxy;
    else return m*0.57735027;
    float k = clamp(0.5*(q.z - q.y + s), 0.0, s);
    return length(vec3(q.x, q.y - s + k, q.z - k));
}
```

```glsl
// Polynomial smooth-min (cubic).   iquilezles.org / smooth-minimum.
float sminPoly(float a, float b, float k) {
    float h = clamp(0.5 + 0.5*(b - a)/k, 0.0, 1.0);
    return mix(b, a, h) - k*h*(1.0 - h);
}

// Exponential smooth-min (N-way friendly).   iquilezles.org.
float sminExp(float a, float b, float k) {
    return -log(exp(-k*a) + exp(-k*b)) / k;
}

// Boolean operators.   iquilezles.org / smooth-minimum.
float opUnion(float a, float b)        { return min(a, b); }
float opSubtraction(float a, float b)  { return max(-a, b); }
float opIntersection(float a, float b) { return max(a, b); }

float opSmoothUnion(float a, float b, float k) { return sminPoly(a, b, k); }
float opSmoothSubtraction(float a, float b, float k) {
    float h = clamp(0.5 - 0.5*(b + a)/k, 0.0, 1.0);
    return mix(b, -a, h) + k*h*(1.0 - h);
}
float opSmoothIntersection(float a, float b, float k) {
    float h = clamp(0.5 - 0.5*(b - a)/k, 0.0, 1.0);
    return mix(b, a, h) + k*h*(1.0 - h);
}
```

**Why this matters for `crd-geometry`.** Every sub-§4.13 primitive
op is `(closest_point, signed_distance, intersects)`. iq's published
catalogue covers the `signed_distance` slot for ~30 3D shapes
exactly; the analytic gradient (via the finite-difference normal
formula above, or via the closed-form gradient where it exists) is
the `closest_point` direction; the slab/ray-vs-shape intersectors
cover `intersects`. **The mathematical content is published,
attributed, and effectively the canonical reference.** Cerid's
contribution is the SIMD-batched, deterministically-rounded,
container-allocator-disciplined CPU implementation — not the
underlying mathematics. Each formula maps directly to a
`crd-geometry-primitives` function.

### 2.2 Similar SDF / distance-function reference catalogues

- **Mercury Demogroup — `hg_sdf`** (MIT, 2015,
  github.com/MercuryDemo/HG_SDF). The other published GLSL primitives
  library; a single `hg_sdf.glsl` header. Notable additions over iq:
  rounded-corner box variants, "fGDF" generalised distance functions,
  the mirror/repeat/twist macros (`pMirror`, `pMod1`, `pR`, etc.) as
  one-line transformations of `p` rather than wrapping functions.
  Cerid should adopt the iq + Mercury *union* of formulas.
- **Hart 1996 — *"Sphere Tracing: A Geometric Method for the
  Antialiased Ray Tracing of Implicit Surfaces"*** (The Visual
  Computer). The foundational paper on raymarching SDFs. Sets up the
  mathematical justification (the SDF's Lipschitz-1 property gives a
  conservative step size) on which all of iq's site rests.
- **Hart 1989 — *"Ray Tracing Deterministic 3-D Fractals"***
  (SIGGRAPH). The sphere-tracing precursor; relevant to v9
  research-tier fractals.
- **Wright 2015 — *"Distance Field Soft Shadows"*** (Unreal Engine
  documentation + GDC presentation). The Unreal mesh-DFAO + mesh-DF-
  shadow technique. Consumes per-mesh baked SDFs (the `crd-sdf` v2
  output); the shader-helper library's "soft shadow" primitive
  (out of scope for v0 but listed) maps directly to this.
- **Frisken et al. 2000 — *"Adaptively Sampled Distance Fields: A
  General Representation of Shape for Computer Graphics"***
  (SIGGRAPH). The ASDF paper — narrow-band sparse SDF representation.
  Already cited by `crd-sdf` (v3 narrow-band sparse); listed here for
  completeness on the SDF side of the geometry/sdf boundary.
- **Sanchez et al. 2017 — *"Boolean Operations on Triangulated
  Solids and their Application"***. Exact-arithmetic Boolean as a
  comparison point to the polygonal Vatti path in `crd-geometry-
  polygon`.
- **Akenine-Möller et al. 2018 — *"Real-Time Rendering"* 4th ed.,
  Ch. 17 (volumetric and translucent rendering) + Ch. 25 (collision
  detection)** (book already cited in base §12.1; mentioned here in
  the SDF-rendering context). Distance-field shadows + DFAO get
  ~6 pages of treatment.
- **Ebert, Musgrave, Peachey, Perlin, Worley — *"Texturing &
  Modeling: A Procedural Approach"*** (3rd ed., Morgan Kaufmann
  2003). The "implicit surfaces / hypertextures" chapters predate
  iq's site by ~15 years and contain the algebraic foundation: union
  / intersection / blob-min as algebraic ops on implicit functions.
- **Bloomenthal et al. — *"Introduction to Implicit Surfaces"***
  (Morgan Kaufmann 1997). Earlier still — implicit-surface
  modelling pre-Marching-Cubes vintage, but the algebraic union /
  intersection / smooth-blend operators are identical to what
  `crd-geometry-shader-helpers` ships.
- **John C. Hart's body of work on procedural surfaces** — papers
  through the 1990s + 2000s establishing sphere-tracing's
  mathematical foundations. Relevant when documenting the conservative-
  step assumption that the C++ closest-point queries inherit from
  the SDF math.
- **Shadertoy as a corpus** — beyond iq's own articles, ~10000+
  community SDF shaders (search: `tag:sdf`). Notable: Pasko's HyperFun
  (academic CSG of SDFs), Adamson's SDF font work (relates to MTSDF
  in `crd-font` future), Mercury's demoscene productions. Useful as
  inspiration / test-cases; not as ship-source.

### 2.3 Cross-pollination — the proposed `crd-geometry-shader-helpers`

This is the headline architectural ask of this supplement:

> **Propose:** ship a NEW sub-module
> `crd-geometry-shader-helpers` (or, alternatively, co-locate as
> `crd-sdf-shader-helpers` — see Open Question below) that emits
> GLSL/HLSL versions of every analytic primitive in
> `crd-geometry-primitives` plus every smooth-blending / domain
> operator from §2.1 Bucket B. Both Cerid sides — CPU
> (`crd-geometry-primitives`) and GPU (`*-shader-helpers`) — derive
> from the same canonical formula list maintained as a single
> generator-input file in the cooker.

**Concrete contract.** For each primitive:

1. **C++ analytic implementation** in
   `engine/geometry/primitives/include/crd/geometry/primitives/sdf_primitives.hpp`.
   Scalar version over `crd::math::Vec3f`, plus packed-lane versions
   over `crd::math::simd::Vec4f` (4-point batch) and
   `crd::math::simd::Vec8f` (8-point batch on AVX2). Returns
   `f32` (signed distance) and optional `Vec3f` (gradient = closest-
   point direction).
2. **GLSL include header** at
   `runtime/shaders/crd_geometry.glsl` (and HLSL counterpart at
   `runtime/shaders/crd_geometry.hlsl`). Same function names + same
   signatures (modulo lane-width — GPU is naturally 1-wide per
   thread, SIMT does the batching).
3. **SPIR-V conformance test** built into the v0 test suite: invoke
   the GLSL primitive through a compute shader on a fixed seed of
   query points, compare against the C++ scalar output, assert
   per-element error within 1 ULP at `f32`. The cooker emits both
   sides from a single source manifest; CI fails if they drift.

**Why a co-located library.** Three consumers need it:

1. **`crd-geometry`** — already drafted §4.13 ships the C++ side; the
   GLSL side is needed by the editor's GPU previewing of analytic
   colliders (Phase 7).
2. **`crd-sdf`** — Phase 3.1.5 v0 ships analytic primitives as one of
   its storage backends (the "free / closed-form in shader" row of
   the §3.2 storage-backend table in `cerid-sdf.md`). Today this
   would mean copy-pasted formulas; with the shared library, the
   sdf substrate consumes the same GLSL header.
3. **`crd-renderer`** — future DFAO / DF-soft-shadow / DF-volumetric-
   fog passes (Phase 3.5+) sphere-trace against scene SDFs;
   sphere-tracing the analytic background ("the world is a ground
   plane") needs the analytic distance functions in the shader
   sidecar. Future MTSDF font rendering (Phase 3.5+ `crd-font`) needs
   the smin / opSmoothUnion ops to blend glyph stems.

**Same-formula contract — not just "similar".** The shader-helper
library and `crd-geometry-primitives` ship the **literal identical**
expression tree. The cooker validates this via:

- A single source-of-truth manifest (`crd_geometry_primitives.toml`)
  per primitive, listing the formula in a small expression IR
  (essentially a typed AST over `+ - * / sqrt min max abs clamp dot
  length normalize sign`).
- A C++ generator (in `tools/asset_cooker/`) that emits `.cpp`
  scalar + `.cpp` SIMD + `.glsl` + `.hlsl` from the same IR. No
  hand-written formula duplication across CPU and GPU.

**Why not "just inline the formulas in shaders".** Because the
*same* analytic primitive shows up in: (a) collider conditioning
(`crd-eylem` runtime), (b) editor 3D preview (Phase 7), (c) sdf
analytic-storage backend (`crd-sdf` v0), (d) font MTSDF (`crd-font`
future), (e) volumetric SDF rendering (`crd-renderer` future), (f)
acoustic occlusion approximation (`crd-audio` Phase 3.4 future).
Six independent copy-pastes of `sdSphere(p, r) = length(p) - r` is
the cheap version; the ULP-divergence consequence when one consumer
"helpfully" rearranges to `length(p - centre) - r` and sphere-tracing
diverges from physics-collision by 0.0001 across ten frames is the
expensive version. The single-source contract eliminates the
divergence by construction. (See the Open Question deferred to
Phase 3.1.5 close in §6 below — *which* substrate owns the helpers
file is open; *that* it's a single source is not.)

---

## 3. Computational-geometry textbook foundation

The base dossier §12.1 cited 10 books. This section catalogues the
academic foundation that grounds each slice — for textbooks already
in §12.1, the line below is **the slice-mapping** (which sub-module
each book most informs); for new books not in §12.1, full citations
appear in §7.

**Books already cited in base §12.1 — slice-mapping.**

- **de Berg, Cheong, van Kreveld, Overmars — *Computational
  Geometry: Algorithms and Applications* (3rd ed., 2008)** — the
  standard graduate textbook. Maps to: **v6 polygon ops** (Ch. 2
  line-segment intersection / Bentley-Ottmann, Ch. 3 polygon
  triangulation, Ch. 13 Voronoi via Fortune's sweep), **v8 Delaunay**
  (Ch. 9 Voronoi, Ch. 14 Delaunay), **v5 spatial accelerators**
  (Ch. 5 range trees, Ch. 12 BSP). Used as the proof-of-correctness
  reference for v6 + v8 algorithm choice; gives optimal complexity
  bounds we cite when justifying e.g. choosing Bentley-Ottmann
  (`O((n+k) log n)`) over a naive `O(n²)` in v6e.
- **Joseph O'Rourke — *Computational Geometry in C* (2nd ed., 1998)** —
  practical / implementation-grounded counterpart to de Berg. Maps
  to: **v3 hull** (Ch. 3 — gives hull-merge plausibility checks),
  **v6 polygon ops** (Ch. 1 — point-in-polygon canonical
  derivation). Useful as the "this is what the code looks like"
  reference, complementary to de Berg's "this is why it's correct".
- **Christer Ericson — *Real-Time Collision Detection* (2004)** —
  THE physics-engine geometry book. Maps to: **v0 primitives**
  (Ch. 5 — every distance/intersection formula, with rounding/
  edge-case discussion), **v1 BVH** (Ch. 6 — bounding volumes;
  Ch. 7 — hierarchies), **v2 GJK + EPA** (Ch. 9 — the Cerid GJK
  tiebreak rule comes from §9.5.4 explicitly). The Ericson "tiebreak
  rules" cited in base §5.1 are the §9.5 prose: "When the new vertex
  is in front of multiple simplex sub-features, drop the vertex that
  contributes the least to the resulting closest-point computation."
- **David Eberly — *3D Game Engine Design* (2nd ed., 2007)** — the
  exhaustive primitive-pair catalogue. Maps to: **v0 primitives** (~70
  primitive-pair distance/intersection methods, more than Ericson;
  the dossier's §4.13 "GTE catalogue" *is* this book's Ch. 14–17).
  When v0c needs the OBB-vs-OBB SAT 15-axis decomposition, Eberly is
  the worked-example reference.
- **van den Bergen — *Collision Detection in Interactive 3D
  Environments* (2003)** — the alternative GJK / EPA reference. Maps
  to: **v2 GJK + EPA** as the *alternative* tiebreak (Cerid pins the
  Ericson convention per ADR-0076 §4.1; van den Bergen documents the
  rejected variant for traceability).
- **Botsch, Kobbelt, Pauly, Alliez, Lévy — *Polygon Mesh Processing*
  (2010)** — the mesh-processing book. Maps to: **v7 mesh
  processing** (Ch. 7 simplification → QEM; Ch. 4 smoothing →
  Taubin; Ch. 6 remeshing → isotropic remesh; Ch. 5 parameterisation
  → LSCM; Ch. 8 hole filling → Liepa). Each v7 sub-slice has a
  corresponding chapter; the book's API conventions inform our
  half-edge data-structure design (the libigl-influenced layout
  mentioned in base §11.2).
- **Akenine-Möller, Haines, Hoffman et al. — *Real-Time Rendering*
  (4th ed., 2018)** — Maps to: **v0 primitives** (Ch. 22 intersection
  test methods is a curated subset of Eberly), **v1 BVH** (Ch. 19.1
  spatial data structures + culling), **v9a GPU LBVH** (Ch. 26.5
  light transport — references LBVH for ray traversal).

**New books not in base §12.1 — added as foundations for specific
slices.**

- **Franco P. Preparata & Michael Ian Shamos — *Computational
  Geometry: An Introduction* (Springer, 1985)** — the foundational
  text that *defined* computational geometry as a field. Pre-dates
  de Berg by 23 years; covers convex hulls, intersections, proximity
  problems, Voronoi diagrams, lower bounds. Maps to: **v3 Quickhull**
  (Ch. 3 hull algorithms — incremental, gift-wrapping, divide-and-
  conquer; gives the lower-bound `Ω(n log n)` argument that motivates
  Quickhull's expected-case optimality), **v8 Delaunay** (Ch. 5
  proximity → Voronoi). Useful as the "why these algorithms are
  optimal" foundation; cite this when arguing in ADR comments why
  Quickhull rather than Chan's algorithm in v3b.
- **Herbert Edelsbrunner — *Algorithms in Combinatorial Geometry*
  (Springer, 1987)** — academic depth on hyperplane arrangements,
  configuration spaces, and the combinatorial side. Maps to: **v8
  Delaunay** as the rigorous theory reference for higher-dimensional
  Delaunay.
- **Herbert Edelsbrunner — *Geometry and Topology for Mesh
  Generation* (Cambridge, 2001)** — mesh-generation theory. Maps to:
  **v8 3D Bowyer-Watson** (the harder one — gives the Voronoi-cell
  topology guarantees needed when extending 2D Delaunay flip-based
  proofs to 3D's "edge flips don't suffice in 3D" obstruction).
- **Goldman — *An Integrated Introduction to Computer Graphics and
  Geometric Modeling* (CRC, 2009)** — the broader CG / CAGD
  perspective. Maps to: **future Phase 3.2+ animation / character
  geometry** (Bezier, B-spline, NURBS, subdivision-surface
  exposition), **v7b Loop subdivision** (Ch. 23 subdivision surfaces
  is the cleanest derivation).
- **Foley, van Dam, Feiner, Hughes — *Computer Graphics: Principles
  and Practice* (3rd ed., Addison-Wesley, 2014)** — the canonical CG
  textbook. Maps to: **v6c Sutherland-Hodgman + v6 polygon clipping
  generally** (Ch. 36 clipping is the original-author treatment by
  Sutherland et al.); also Cohen-Sutherland and Liang-Barsky line
  clipping (relevant if the future `crd-renderer` 2D path needs a
  scissor-style clipper).
- **Si — TetGen author papers + book chapters in *Tetrahedral Mesh
  Generation*** — the rigorous reference for v8 3D Bowyer-Watson +
  tetrahedral quality measures (radius-edge ratio, sliver removal)
  that any robust 3D Delaunay implementation must address. Cited at
  paper level in base §12.7; flagged here as "the tet-mesh primary
  source" for v8c justification.

**Pattern.** Each of the ten v-slices has a primary textbook +
primary paper anchor. The slice plan in base §8 + ADR-0076 §7 cites
the *paper*; this supplement adds the *textbook* providing the
bigger-picture grounding. Slice-by-slice anchor table:

| Slice | Primary textbook | Primary paper |
|---|---|---|
| v0 primitives | Eberly *3D Game Engine Design* + Ericson *RTCD* | Möller-Trumbore 1997, Akenine-Möller 2001 |
| v1 BVH | Ericson Ch. 6–7 + Akenine-Möller Ch. 19 | Wald 2007 (binned SAH), Catto GDC 2019 (refit) |
| v2 GJK/EPA | Ericson Ch. 9 + van den Bergen 2003 | GJK 1988, EPA (van den Bergen 2001) |
| v3 hull | Preparata-Shamos Ch. 3 + O'Rourke Ch. 4 | Barber 1996 (Quickhull) |
| v4 mesh queries | Botsch *PMP* Ch. 1–3 + Ericson Ch. 5 | Jacobson 2013 (winding number) |
| v5 spatial | Akenine-Möller Ch. 19 + de Berg Ch. 5 | Ulrich 2000, Beckmann 1990, Teschner 2003 |
| v6 polygon | de Berg Ch. 2–3 + O'Rourke Ch. 1 + Foley Ch. 36 | Vatti 1992, Bentley-Ottmann 1979 |
| v7 mesh proc | Botsch *PMP* Ch. 4–8 | Garland-Heckbert 1997, Loop 1987, Liepa 2003 |
| v8 Delaunay | Edelsbrunner 2001 + de Berg Ch. 9, 14 | Bowyer 1981 + Watson 1981 |
| v9 GPU/decomp | Akenine-Möller Ch. 26 (GPU ray) | Karras 2012, Mamou 2014 (V-HACD) |

---

## 4. Performance contract — SIMD + `crd::containers` integration

Base dossier §6 mentions allocator discipline and "GPU-first
reservations" but doesn't commit to a per-sub-module SIMD strategy.
This section locks one. Numbers are *targets*; per-slice DoD measures
against them.

### 4.1 SIMD strategy per sub-module

| Sub-module | SIMD posture | Lane width | Reference |
|---|---|---|---|
| `-primitives` (v0) | **SIMD-natural.** Ray-vs-AABB, ray-vs-sphere, ray-vs-triangle (Möller-Trumbore) all vectorise to packed-lane queries. | `Vec4f` (4-point batch) baseline; `Vec8f` (8-point) on AVX2 builds. | Embree's primitive ISPC bindings + iq's GLSL primitives compiled to SIMT — same math. |
| `-bvh` (v1) | **SIMD-critical.** Quad-BVH = 4 children/node = 1 SIMD ray-vs-4-AABB per node; Wide-BVH (8 children) = AVX2 8-wide. | `Vec4f` for BVH4, `Vec8f` for BVH8. | Embree BVH4/BVH8 traversal. |
| `-bvh` build | **Partly SIMD.** Sort is not SIMD; binned-SAH cost evaluation is `Vec4f` (4 bins simultaneously). | `Vec4f`. | Wald 2007. |
| `-convex` GJK | **Partial SIMD.** Support function for primitive shapes vectorises (4-direction simultaneous). Simplex update is branchy (line/triangle/tetrahedron projections); leave scalar. | `Vec4f` for support; scalar simplex. | Catto GDC 2010, Erin Catto's reference. |
| `-convex` SAT | **SIMD-natural.** Box-vs-box: `Vec4f` for 3-axis projection batch. | `Vec4f`. | Akenine-Möller 2001. |
| `-convex` Quickhull | **Furthest-point scan = SIMD reduction.** Hull merge is graph-traversal — scalar. | `Vec4f`/`Vec8f` for the scan; scalar for merge. | Barber 1996. |
| `-mesh` (v4) | **SIMD batch queries.** Closest-point-on-N-triangles → 4-or-8 triangle batch per leaf. Möller-Trumbore over `Vec8f`. Winding-number per-triangle accumulation = `Vec4f`. | `Vec4f`/`Vec8f`. | Embree per-leaf SIMD. |
| `-spatial` (v5) | **Mostly NOT SIMD.** KD-tree + R-tree traversal is irregular branching. **Spatial-hash bucket scan IS SIMD-natural** (batched cell-collision tests). | scalar traversal; `Vec4f` for spatial-hash bucket sweeps. | Teschner 2003. |
| `-polygon` (v6) | **Minimal SIMD.** Branchy structure, irregular memory access. Only Bentley-Ottmann's event-queue compare-key prefix can vectorise. | scalar. | — |
| `-mesh-processing` (v7) | **SIMD for QEM.** `Vec4f` matrix ops on the 4×4 quadric error matrix (16 floats per vertex). Subdivision + remesh: graph traversal — scalar. | `Vec4f` for QEM cost; scalar for the rest. | Garland-Heckbert 1997. |
| `-delaunay` (v8) | **Minimal SIMD.** Incremental-insert is sequential; circumsphere predicate per insertion is `Vec4f`-friendly but not the bottleneck. | scalar with `Vec4f` predicate hot-spot. | Bowyer 1981. |
| `-gpu` (v9) | **Irrelevant — already SIMT.** | per-warp. | Karras 2012. |
| `-decomposition` (v9) | **Cooker-only.** Not perf-critical. | scalar. | Mamou 2014. |

**Performance budgets** (target numbers; per-slice DoD measures
against them on a Zen 4 reference CPU):

- BVH4 traversal: target ~8 ns per ray-vs-4-AABB node test on AVX2;
  Embree publishes ~6 ns; we accept a ~30% gap given determinism
  constraints.
- GJK: 2–6 iterations typical; target 50–200 ns per pair. Same
  envelope as Box2D v3's published numbers.
- Quickhull: target `O(n log n)` expected; 100k points to closed
  hull in <30 ms. qhull reference: ~25 ms on identical input.
- Mesh closest-point (BVH-accelerated, 100k-tri mesh): target
  <1 µs per query batch of 8 points (`Vec8f` lane).
- QEM simplification: 1M-tri mesh to 100k tris in <3 s (Garland-
  Heckbert reports ~2 s on a workstation; we accept ~50% headroom
  for determinism + container overhead).
- LBVH GPU build (v9a): 1M primitives in <8 ms on RTX 3060. Karras
  2012 reports 4–6 ms on Fermi; we accept the determinism overhead.

### 4.2 `crd::containers` integration

Concrete patterns for using Cerid's containers in geometry algorithms.
This binds the substrate to the engine's container discipline (per
CLAUDE.md "no STL in hot paths") with named conventions that every
sub-module follows.

- **`crd::containers::Array<T>`** is the canonical owning storage.
  No `std::vector`. Used for:
  - BVH node array (`Array<BvhNode>` constructed against a persistent
    TLSF allocator at build time)
  - Mesh vertex / index buffers in `TriangleMeshView` ownership
    contexts (consumers may pass non-owning spans)
  - Quickhull candidate-point stack
  - Query result accumulators (per-frame, against
    `crd::jobs::frame_alloc`)
- **`crd::containers::ConstSpan<T>`** is the input-parameter
  convention. No `T*` + `size_t` separately. Every public algorithm
  function takes `ConstSpan` not pointer-pairs. Mirrors `crd-resources`
  +`crd-eylem` already-established convention.
- **`crd::containers::HashMap<K, V>`** for: half-edge mesh's
  vertex-to-edge map, polygon-Boolean's edge-event queue,
  Delaunay's circumsphere cache, mesh-processing adjacency cache.
  No `std::unordered_map`. Cerid's HashMap uses FNV-1a hashing
  (deterministic per ADR-0063) which removes the hash-randomisation
  source of cross-platform divergence that plagues `std::unordered_map`.
- **`crd::containers::String` / `StringView`** for: error messages,
  debug labels, stat-record names. No `std::string`.
- **`crd::containers::sort` / `stable_sort` / `nth_element`** for:
  BVH binned-SAH primitive-sort step, KD-tree median-find, Quickhull
  hull-point ordering, polygon Boolean event-queue ordering. No
  `std::sort` (the existing `crd-no-std-sort-check` CI guard catches
  it; an analogous guard auto-fires on `engine/geometry/**` per
  ADR-0076 §4.6).
- **`crd::math::simd::Soa<TChunk, Lane>`** for: AoSoA-packed AABB
  columns in BVH (`Vec8f.min_x` / `.min_y` / `.min_z` / `.max_x` /
  `.max_y` / `.max_z` arrays — 6 chunks × N nodes, hot for traversal),
  AoSoA point cloud for batched closest-point queries, AoSoA
  triangle batch for per-leaf mesh raycast.
- **`crd::memory::IAllocator*`** constructor-argument convention.
  Recommended sub-allocator pairing per algorithm:
  - `BvhTree<AABB>(persistent_alloc)` — node array against TLSF
    persistent. Build-time temporaries (centroid array, bin
    histogram) against a `LinearAllocator` scratch (one
    `frame_alloc()` block reused across the whole build).
  - `QuickhullState(scratch_alloc)` — candidate set + face list
    against scratch; output hull copied to caller's persistent
    allocator on completion.
  - Half-edge mesh: persistent allocator (long-lived geometry
    representation); processing pipelines (QEM cost cache,
    adjacency rebuild) get scratch.
  - Mesh closest-point query: per-call `frame_alloc` slab for
    candidate-triangle accumulator; result span returned views
    that slab.

### 4.3 Determinism pins specific to SIMD geometry

Extends ADR-0076 §4 (which already pins GJK / SAH / hull / Vatti
tiebreaks). The SIMD-side additions:

1. **SIMD reductions use the fixed pairwise-binary-tree from
   `crd::math::simd::reduce_*`** — never lane-order summation, which
   is non-associative under FP and produces compiler-dependent
   results. The reduction primitive lands in `crd-math::simd` (it's
   already partial; the missing pieces are the lex-tiebreak
   reductions below).
2. **SIMD comparisons return all-bits-set masks; tiebreaks select
   via `select_lane(mask, candidate_a, candidate_b)`** with
   deterministic candidate-ordering pinned per algorithm (e.g.,
   "prefer lower-index lane on equal values" — Cerid convention).
3. **BVH binned-SAH uses INTEGER bin counts**, not FP histograms.
   The only FP operation in cost computation is per-bin cost; that
   uses `crd::math::deterministic::*` (no `std::log`).
4. **Quickhull's "furthest-point" SIMD reduction uses
   `reduce_argmax_with_lex_tiebreak`** — a substrate primitive that
   pins the X-then-Y-then-Z lex tiebreak across SIMD lanes when two
   lanes have equal furthest-distance. **This primitive does not
   exist in `crd::math::simd` today** (verified on
   `engine/math/include/crd/math/simd/`); v0 of `crd-geometry` lands
   it as a `crd-math::simd` extension before v3 needs it.

---

## 5. Cross-references — what the base dossier covered

This supplement does **not** re-survey the industry landscape covered
in base dossier §3 (Bullet / PhysX / Jolt / Box2D v3 / Havok / Unity
DOTS / Unreal / Godot / CGAL / Geogram / libigl / GTE / Open3D /
OpenMesh / MeshLab / PMP / Embree / nanoRT / Boost.Geometry / S2 /
FCL / HPP-FCL / OMPL / Sutherland-Hodgman / Vatti / Greiner-Hormann /
Boost.Polygon / Clipper2 / V-HACD / HACD / CoACD / TetGen / fTetWild /
MeshFix / qhull). It does **not** re-list the algorithmic scope (base
§4). It does **not** re-derive the slice plan (base §8). It does
**not** re-make the Phase 3.1.7 slot decision (base §10).

This supplement **adds**:

- iq's body of work + Mercury hg_sdf + Hart 1996 + Wright 2015 as
  the **analytic-primitives canonical reference set** — extends base
  §4.13 ("the GTE catalogue") with the published-shader-formula
  catalog + smooth-blending operators.
- **Textbook-foundation slice-mapping table** (§3 above) — extends
  base §12.1 books-list with a per-slice grounding table; adds 5 new
  textbooks not in base §12.1.
- **Per-sub-module SIMD strategy** + lane-width choice + performance
  budget (§4.1) — extends base §6.6 ("GPU-first reservations") into
  CPU-side SIMD commitments.
- **`crd::containers` substrate-binding patterns** (§4.2) — extends
  base §6.5 ("Allocator discipline") into named per-algorithm
  conventions.
- **SIMD-specific determinism pins** (§4.3) — extends ADR-0076 §4
  with reduction-tree and lex-tiebreak commitments.
- **Proposed `crd-geometry-shader-helpers`** as an 11th sub-module
  (§2.3) — extends base §7 module split.

---

## 6. Proposed updates to ADR-0076 + the phase plan

This section lists concrete, applyable edits the supplement implies.
The supplement **does not edit** ADR-0076 or `phase-3.1.7-geometry.md`
directly; it proposes the edits below for the user to apply.

### 6.1 ADR-0076 additions

- **§1 Decision** — add as 11th sub-module:

  > **`crd-geometry-shader-helpers`** (cooker-emitted, ~2 KLOC GLSL
  > + ~2 KLOC HLSL + manifest + generator) — GLSL/HLSL primitive
  > library mirroring `crd-geometry-primitives` formulas + the
  > smooth-blending / domain operators from iq's published catalogue
  > (smin, opSmoothUnion / Subtraction / Intersection, domain repeat /
  > mirror / warp). Cooker emits CPU + GPU sides from a single
  > formula-IR manifest; CI conformance test asserts ULP-bounded
  > match across the two sides. Consumed by `crd-sdf` (analytic-
  > storage backend), `crd-renderer` Phase 3.5+ (DFAO, DF-soft-
  > shadow, volumetric SDF), `crd-font` (MTSDF), editor (analytic-
  > collider preview).

- **§4 Determinism contract** — add SIMD-specific pins (this
  supplement §4.3 verbatim):
  1. SIMD reductions use `crd::math::simd::reduce_*` pairwise binary
     tree (not lane-order accumulation).
  2. SIMD argmax / argmin tiebreaks pin lex-order on candidate
     coordinates via the new `reduce_argmax_with_lex_tiebreak` primitive.
  3. Binned-SAH histograms use INTEGER counts; the only FP op
     (per-bin cost) goes through `crd::math::deterministic`.
  4. The shader-helper library's GLSL/HLSL output is held to a
     1-ULP-at-`f32` conformance budget against the C++ scalar
     reference.

- **§5 API design philosophy** — add as new bullet:

  > **SIMD substrate commitment.** Hot-path algorithms use
  > `crd::math::simd::Vec4f` / `Vec8f` / `Soa<TChunk, Lane>`
  > exclusively for batched lanes; never raw intrinsics. AoSoA layout
  > is the default storage for batched primitive collections (BVH
  > leaves, point clouds, triangle batches). Per-sub-module SIMD
  > posture documented in research supplement §4.1.

- **§7 Slice list** — add three SIMD-specific sub-slices:
  - **v0e — iq-formulary primitives substrate** (~1 KLOC) —
    polynomial / exponential smin operators + domain ops + the
    shader-helpers cooker generator skeleton (no GPU side yet).
  - **v1g — BVH4 quad-topology SIMD ray-traversal** (~500 LOC) —
    promote the v1d quad-BVH variant from "behind a parameter" to
    the default + ship the SIMD ray-vs-4-AABB intersect kernel.
  - **v4g — per-leaf SIMD triangle intersection** (~300 LOC) —
    `Vec8f` Möller-Trumbore over 8 triangles per BVH leaf.

  (And note: the shader-helper library lands at v0e *generator
  skeleton* + iterates per-slice-with-new-primitives; the GLSL/HLSL
  output side ships as a v9e slice consumed first by `crd-renderer`
  Phase 3.5+.)

### 6.2 `phase-3.1.7-geometry.md` additions

- New row in the module-split table for `crd-geometry-shader-helpers`.
- Slice-list updated with v0e, v1g, v4g sub-slices.
- New "Performance budgets" section per supplement §4.1 numbers.
- New "Reference reading" section pointing to the supplement
  dossier for SIMD + iq + textbook grounding.

### 6.3 Open question added (deferred to Phase 3.1.5 close)

> **Q. Where does the shader-helper library live — `crd-geometry-
> shader-helpers` (Cerid owns it from `crd-geometry`'s side, sdf
> + renderer + font consume) or `crd-sdf-shader-helpers` (Cerid owns
> it from `crd-sdf`'s side, geometry + renderer + font consume)?**
>
> Architectural argument for **`crd-geometry-shader-helpers`**: the
> *math* is geometric (point-to-shape distance + closest-point);
> SDF representation is *one consumer* of geometric math.
> Architectural argument for **`crd-sdf-shader-helpers`**: GPU-side
> SDF rendering (DFAO, DF-soft-shadow) is the *primary* consumer of
> shader-emitted distance functions; physics + geometry use the C++
> side, not the GPU side.
>
> **Decision deferred to Phase 3.1.5 close** — when `crd-sdf`'s
> shader requirements are concrete, the consumer-weighted answer
> becomes clear. Until then, treat the library as Cerid-owned + name-
> agnostic; the cooker-side generator implementation is identical
> regardless.

---

## 7. References

References below add to base §12. Pre-existing citations in base §12
(Ericson, Eberly, O'Rourke, de Berg, Botsch, van den Bergen,
Akenine-Möller, all paper citations from §12.2–§12.10) are NOT
re-listed.

### 7.1 New books

- Preparata, F. P. & Shamos, M. I. (1985) — *Computational Geometry:
  An Introduction*. Springer-Verlag.
- Edelsbrunner, H. (1987) — *Algorithms in Combinatorial Geometry*.
  Springer-Verlag.
- Edelsbrunner, H. (2001) — *Geometry and Topology for Mesh
  Generation*. Cambridge University Press.
- Goldman, R. (2009) — *An Integrated Introduction to Computer
  Graphics and Geometric Modeling*. CRC Press.
- Foley, van Dam, Feiner, Hughes et al. (2014) — *Computer Graphics:
  Principles and Practice* (3rd ed.). Addison-Wesley.
- Ebert, Musgrave, Peachey, Perlin & Worley (2003) — *Texturing &
  Modeling: A Procedural Approach* (3rd ed.). Morgan Kaufmann.
- Bloomenthal, J. (ed.) et al. (1997) — *Introduction to Implicit
  Surfaces*. Morgan Kaufmann.

### 7.2 Online references — Inigo Quilez's articles

All at **iquilezles.org/articles/**. Canonical analytic-distance-
function and rendering reference. Content licensed CC-BY-NC-SA;
implementations Cerid derives are clean-room re-expressions of the
mathematics, which is itself unencumbered.

- *distance functions* — 3D primitive distance functions (~30 shapes)
- *distance functions 2D* — 2D primitive distance functions (~25
  shapes)
- *intersectors* — ray-vs-shape analytic intersection
- *smooth minimum* — polynomial / exponential / power smin
- *interior distances* — interior SDF behaviour + branch-free forms
- *distance to bezier* — quadratic + cubic Bezier closest-point
- *sdf bounding volumes* — analytic AABB derivation per primitive
- *raymarching distance fields* — sphere-tracing the canonical
  algorithm + accuracy / step-size analysis
- *soft shadows* — distance-function shadow rays
- *terrain raymarching* — heightfield + distance-field hybrid
- *normals SDF* — gradient extraction by finite differences

### 7.3 Other shader / SDF reference catalogues

- Mercury Demogroup — **hg_sdf** GLSL library, MIT licensed,
  github.com/MercuryDemo/HG_SDF.
- Hart, J. C. (1996) — *Sphere Tracing: A Geometric Method for the
  Antialiased Ray Tracing of Implicit Surfaces*. The Visual Computer
  12(10).
- Hart, J. C. (1989) — *Ray Tracing Deterministic 3-D Fractals*.
  SIGGRAPH.
- Wright, D. (2015) — *Distance Field Soft Shadows*. Unreal Engine
  presentation + documentation.
- Frisken, S., Perry, R., Rockwood, A. & Jones, T. (2000) —
  *Adaptively Sampled Distance Fields: A General Representation of
  Shape for Computer Graphics*. SIGGRAPH.
- Pasko, A., Adzhiev, V., Sourin, A. & Savchenko, V. (1995) —
  *Function Representation in Geometric Modeling: Concepts,
  Implementation and Applications*. The Visual Computer 11(8).
  (HyperFun reference.)

### 7.4 Cerid-internal cross-references (extending base §12.12)

- ADR-0064 — `crd-sdf` substrate (sibling substrate; consumer + co-
  owner candidate of `*-shader-helpers`).
- `docs/research/cerid-sdf.md` §3.2 (storage backends — analytic
  primitives row is the consumer of the shader-helper library).
- `docs/phases/phase-3.1.5-sdf.md` (the Phase 3.1.5 close is the
  deferred-decision deadline for the shader-helpers ownership open
  question in §6.3).
- `engine/math/include/crd/math/simd/vec4f.hpp`,
  `engine/math/include/crd/math/simd/vec8f.hpp`,
  `engine/math/include/crd/math/simd/soa.hpp` — the SIMD substrate
  the geometry algorithms build on. The `reduce_argmax_with_lex_
  tiebreak` extension lands here as a v0 prerequisite.
- `engine/containers/include/crd/containers/*` — the container
  substrate per §4.2.

---

**End of supplement.** Word count target ~6–8K. Companion to base
dossier; both should be read together when ADR-0076 implementation
begins at Phase 3.1.7 kickoff.
