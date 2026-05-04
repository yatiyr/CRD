# ADR-0047 — Font rendering system

**Status:** Accepted
**Date:** 2026-05-04
**Tags:** arch, font, renderer, ui, text

---

## Context

Cerid needs both 2D screen-space text (UI labels, HUD, menus) and 3D world-space text (scene
annotations, medical visualization labels, debug overlays, DAW channel names). The font rendering
system must be future-proof for complex scripts (Arabic RTL, CJK, ligatures) and must integrate
cleanly with the resource pipeline, the material domain system, and `crd-ui` (Phase 5).

Six architectural decisions were evaluated against what shipping production engines do.

---

## Decisions

### 1. Atlas technique — MTSDF

**Decision:** MTSDF (Multi-channel + True SDF), 4-channel RGBA atlas.
- RGB channels: MSDF (multi-channel signed distance field, Viktor Chlumský 2016) — sharp corners at
  any scale, correct at 8 px through 200 px from a single 64 px/glyph bake.
- A channel: traditional SDF — used for large-scale smooth glow, outer glow, and drop shadow effects
  where sharp corners are not needed.

**Runtime shader:** median-of-three on RGB channels gives the MSDF edge. A channel gives the SDF
effect overlay. Both in a single sample.

**Used by:** Godot 4.x (their `FontFile` resource). Supersedes plain SDF (Valve 2007) and plain
MSDF (Unity TextMeshPro v1).

**Rejected:**
- SDF (1-channel): corner bleeding at small sizes.
- MSDF (3-channel): corner quality only; no smooth large-scale SDF channel for effects.
- Bitmap: does not scale; one atlas per size.
- Vector tessellation: not real-time capable for dynamic UI text.

### 2. Font baking libraries — FreeType + msdfgen (cooker-only for offline path)

**Decision:** FreeType 2 (font loading/parsing) + msdfgen ≥ 1.9 (MTSDF generation).

- **FreeType** loads TTF, OTF, WOFF, variable fonts. Provides glyph outlines (cubic Bézier curves)
  to msdfgen and glyph metrics to the `FontResource`. Used by Chrome, Firefox, Qt, GTK, Godot,
  Blender. MIT/FreeType license.
- **msdfgen** converts FreeType outlines to MTSDF pixel data. By Viktor Chlumský. MIT license.
  Used by Godot 4, many modern engines.

For the **offline baked path**: FreeType + msdfgen are **cooker-only** dependencies. The runtime
sees only a `TextureResource` (the baked MTSDF atlas) and glyph metric data — it never calls
FreeType or msdfgen directly.

For the **dynamic atlas path** (see Decision 3): FreeType + msdfgen become runtime dependencies
as well, since glyphs are rasterized on demand.

**Rejected:** stb_truetype — SDF-only (no MTSDF), limited font format support, mediocre quality.

### 3. Atlas management — both offline baked and dynamic

**Decision:** Both paths, sequenced in Phase 3.3.

- **v1a–v1b (offline baked):** Atlas baked at cook time for a declared character set. Runtime has
  zero FreeType/msdfgen dependency in this path. The `FontResource` API is identical regardless of
  how the atlas was produced.
- **v1c (dynamic atlas):** `DynamicFontAtlas` — a runtime glyph cache that rasterizes missing
  codepoints on demand via FreeType + msdfgen. Required for: DAW plugin text (unknown at build time),
  robotics annotation with dynamic labels, user-input CJK text. FreeType + msdfgen become runtime
  deps in this mode.

**API contract:** `FontResource::atlas` is a `ResourceHandle<TextureResource>` regardless of path.
The dynamic path internally swaps the GPU texture as new glyphs are added; consumers see the same
handle. The distinction is transparent to the renderer.

### 4. Text shaping — HarfBuzz from Phase 3.3

**Decision:** HarfBuzz from Phase 3.3 v1b. Full complex text shaping from day one:
- Bidirectional text (Arabic, Hebrew RTL via Unicode Bidi Algorithm)
- Ligature substitution (fi, fl, etc.)
- CJK composition and mark positioning
- Complex scripts (Devanagari, Thai, combining diacritical marks)

FreeType provides font data to HarfBuzz via `hb_ft_font_create()`. HarfBuzz outputs shaped glyph
IDs with per-glyph offsets and advances.

**Interface:** `ITextShaper` (for testability; `HarfBuzzShaper` is the production implementation):

```cpp
struct ShapedGlyph
{
    crd::u32          glyph_id;    // font-specific glyph ID (not codepoint)
    crd::math::Vec2f  offset;      // x/y offset from pen position
    crd::math::Vec2f  advance;     // advance for the next glyph
};

class ITextShaper
{
public:
    virtual ~ITextShaper() = default;
    virtual crd::containers::Array<ShapedGlyph> shape(
        crd::containers::StringView text,
        const FontResource& font,
        crd::IAllocator* a) = 0;
};
```

**Rejected:** Latin-only deferred shaping — Cerid targets general-purpose applications including
DAWs (internationalized UIs) and robotics (multilingual annotation). HarfBuzz is the only production
solution for correct text rendering across scripts.

**For Noto Sans** (the included demo font, SIL OFL): covers essentially all Unicode scripts, making
HarfBuzz integration testable from day one of Phase 3.3.

### 5. 3D extruded text — `crd-font::make_text_mesh()` in Phase 3.3 v1d

**Decision:** `crd-font` provides `make_text_mesh(string_view, font, extrude_depth, bevel_size)`
returning `crd::meshgen::MeshData`. FreeType provides glyph outline Bézier curves; the function
tessellates them (ear-clipping or monotone polygon decomposition) and extrudes the resulting polygon
along the Z axis.

`crd-font` gains a dependency on `crd-meshgen` for the `MeshData` return type. This is acceptable:
`crd-meshgen` is low in the dependency graph (depends only on `crd-math`, `crd-containers`,
`crd-memory`).

**Use cases:** architectural signage, game title cards, cinematic logo animation, engineering labels
with depth.

### 6. Module structure — separate `crd-font`

**Decision:** `crd-font` is a separate module at `engine/font/`. It does NOT live inside `crd-ui`
or `crd-renderer`.

- `crd-ui` (Phase 5) **depends on** `crd-font` for full 2D text layout.
- `crd-renderer` uses `crd-font` for world-space billboard text but does not own font logic.
- `crd-font` ships in Phase 3.3, four phases before `crd-ui`.

**Dependency graph additions:**
```
crd-meshgen ←── crd-font ←── crd-renderer (billboard text)
                           ←── crd-ui      (full 2D layout, Phase 5)
```

---

## `FontResource` structure

```cpp
// engine/font/include/crd/font/font_resource.hpp
namespace crd::font
{
struct GlyphMetrics
{
    crd::math::Vec2f  uv_min, uv_max;  // rect in MTSDF atlas (UV space)
    crd::math::Vec2f  bearing;          // offset from baseline to glyph top-left
    crd::f32          advance;          // horizontal advance in pixels at bake size
};

struct FontResource
{
    ResourceHandle<TextureResource>                atlas;        // MTSDF/SDF RGBA8 atlas
    crd::containers::HashMap<crd::u32, GlyphMetrics> glyphs;   // glyph_id → metrics
    crd::f32  line_height;
    crd::f32  ascender;
    crd::f32  descender;
    crd::f32  sdf_range;      // in atlas pixels; used by MTSDF shader
    crd::u32  atlas_size;     // atlas side length (always square power-of-two)

    explicit FontResource(crd::IAllocator* a);
};
} // namespace crd::font
```

## CRDR FourCCs added in Phase 3.3

| FourCC | Meaning |
|--------|---------|
| `FONT` | FontResource artifact type |
| `FMTX` | Font metrics chunk (line_height, ascender, descender, sdf_range, atlas_size, glyph_count, atlas_resource_id[16]) |
| `GLPH` | Glyph table chunk — packed `[glyph_id u32, uv_min f32×2, uv_max f32×2, bearing f32×2, advance f32][]` |
| `KERN` | Kerning pair table (optional; `[left_id u32, right_id u32, advance_adjust f32][]`) |
| `CSET` | Baked character set table (list of Unicode range pairs `[first u32, last u32][]`) |

The atlas is a standard TXTR artifact (from Phase 2.7) referenced by UUID in the `FMTX` chunk.

---

## Demo font

**Noto Sans** (SIL Open Font License 1.1) — covers virtually all Unicode scripts (Latin, Cyrillic,
Arabic, Hebrew, CJK, Devanagari, Thai, and more). Stored in `assets/source/fonts/`. Enables HarfBuzz
integration testing across scripts from day one. License compatible with Cerid's CC0/Apache/MIT/OFL
asset policy.

---

## Consequences

- Phase 3.3 introduces `crd-font`, `FontResource`, `FontResourceLoader`, MTSDF cooker handler, HarfBuzz shaper, billboard text renderer, dynamic atlas, and extruded text mesh utility.
- `crd-font` is the first module to depend on both `crd-meshgen` and `crd-resources`.
- FreeType + msdfgen + HarfBuzz are new CMake dependencies. All three are C libraries with permissive licenses (FreeType: MIT/FTL, msdfgen: MIT, HarfBuzz: MIT).
- The MTSDF shader is a new surface-domain shader variant in `ForwardRenderPath`.
- `crd-ui` (Phase 5) gains `crd-font` as a direct dependency — no duplication.
- All text rendering at runtime (UI, world-space, extruded) goes through the same `FontResource` + MTSDF atlas pipeline.

---

## References

- ADR-0046 — MaterialDomain enum (`Surface` for billboard text, `UI` for screen-space text)
- ADR-0045 — `assets/source/` layout (fonts subdirectory)
- ADR-0043 — MeshResource vertex layout (extruded text uses same 48B/vertex format)
- `docs/phases/phase-3.3-font.md` — Phase 3.3 implementation plan
- `docs/systems/sandbox.md` — Phase 3.3 sandbox gains font rendering panel
