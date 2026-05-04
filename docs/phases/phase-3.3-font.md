# Phase 3.3 — Font Rendering (`crd-font`)

**Status:** ⏳ planned — begins after Phase 3.2 (animation) ships
**ADRs:** ADR-0047 (font rendering system)
**New modules:** `crd-font` at `engine/font/`
**Depends on:** Phase 2.7 (TextureResource), Phase 2.6 (ResourceManager), `crd-meshgen` (for extruded text)

---

## Goal

Ship a production-quality font rendering system that covers 2D screen-space text (UI labels, HUD,
menus), 3D world-space billboard text (scene annotations, medical labels, debug overlays, DAW channel
names), and 3D extruded text (architectural signage, cinematic logo animation, engineering labels).

All six architectural decisions are locked in ADR-0047. Summary:

1. **Atlas:** MTSDF (4-channel RGBA — RGB=MSDF for sharp corners, A=SDF for smooth effects). Godot 4 approach.
2. **Baking libs:** FreeType 2 + msdfgen ≥ 1.9. Cooker-only for offline path; runtime for dynamic.
3. **Atlas management:** Both offline baked (cooker-only) and dynamic (runtime glyph cache). No deferrals.
4. **Text shaping:** HarfBuzz from v1b — full complex scripts (Arabic RTL, CJK, ligatures, diacritics) from day one.
5. **3D extruded text:** `crd-font::make_text_mesh()` returns `crd::meshgen::MeshData` — FreeType outlines tessellated + extruded.
6. **Module structure:** `crd-font` is separate from `crd-ui` and `crd-renderer`. `crd-ui` depends on it.

---

## Architecture

### `FontResource`

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
    ResourceHandle<TextureResource>                      atlas;      // MTSDF/SDF RGBA8 atlas
    crd::containers::HashMap<crd::u32, GlyphMetrics>     glyphs;     // glyph_id → metrics
    crd::f32  line_height;
    crd::f32  ascender;
    crd::f32  descender;
    crd::f32  sdf_range;      // in atlas pixels; used by MTSDF shader
    crd::u32  atlas_size;     // atlas side length (always square power-of-two)

    explicit FontResource(crd::IAllocator* a);
};
} // namespace crd::font
```

### `ITextShaper` + `HarfBuzzShaper`

```cpp
// engine/font/include/crd/font/text_shaper.hpp
namespace crd::font
{
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
} // namespace crd::font
```

`HarfBuzzShaper` is the production implementation. FreeType provides font data to HarfBuzz via
`hb_ft_font_create()`. HarfBuzz outputs shaped glyph IDs with per-glyph offsets and advances.

### `DynamicFontAtlas` (v1c)

Runtime glyph cache. Rasterizes missing codepoints on demand via FreeType + msdfgen. Internally
grows the GPU texture as new glyphs are added; `FontResource::atlas` handle is swapped atomically.
Required for: DAW plugin text (unknown at build time), robotics annotation with dynamic labels,
user-input CJK.

### CRDR FourCCs

| FourCC | Meaning |
|--------|---------|
| `FONT` | FontResource artifact type |
| `FMTX` | Font metrics chunk (line_height, ascender, descender, sdf_range, atlas_size, glyph_count, atlas_resource_id[16]) |
| `GLPH` | Glyph table chunk — packed `[glyph_id u32, uv_min f32×2, uv_max f32×2, bearing f32×2, advance f32][]` |
| `KERN` | Kerning pair table (optional; `[left_id u32, right_id u32, advance_adjust f32][]`) |
| `CSET` | Baked character set table (list of Unicode range pairs `[first u32, last u32][]`) |

The atlas is a standard TXTR artifact (Phase 2.7) referenced by UUID in the `FMTX` chunk.

### Dependency graph additions

```
crd-meshgen ←── crd-font ←── crd-renderer  (billboard text)
                           ←── crd-ui       (full 2D layout, Phase 5)
```

---

## Demo font

**Noto Sans** (SIL Open Font License 1.1) stored in `assets/source/fonts/`. Covers virtually all
Unicode scripts (Latin, Cyrillic, Arabic, Hebrew, CJK, Devanagari, Thai, and more). Enables HarfBuzz
integration testing across scripts from day one.

---

## Slices

### v1a — FontResource + offline MTSDF cooker + FontResourceLoader

**Scope:**
- `engine/font/` module scaffold: `CMakeLists.txt`, umbrella header `font.hpp`.
- `FontResource` struct (`font_resource.hpp`) + `GlyphMetrics`.
- `FontResourceLoader` registered via `crd::font::register_font_loader(rm)`. Parses `FMTX` + `GLPH`
  chunks, resolves atlas `ResourceHandle<TextureResource>` via transitive `load_sync`.
- CRDR FourCCs `FONT`, `FMTX`, `GLPH`, `KERN`, `CSET` added to `crdr.hpp`.
- Cooker font handler (`.ttf`/`.otf` → `FONT` artifact): FreeType 2 loads the font; msdfgen generates
  4-channel MTSDF atlas; cooker emits one `TXTR` artifact (the atlas) + one `FONT` artifact
  (FMTX + GLPH + optional KERN/CSET chunks). Character set declared in a `.font.toml` sidecar.
- FreeType + msdfgen are **cooker-only** deps in this slice (no runtime call to either library).
- `smoke_font_offline.exe` (headless): cook Noto Sans Latin subset → mount → `load_sync<FontResource>`
  → assert atlas handle Ready + glyph table non-empty → exit 0.

**Tests:**
- FMTX + GLPH round-trip (write chunks, read back, verify metrics match).
- Missing FMTX chunk → `FontResource` load fails gracefully.
- Atlas transitive load via FontResourceLoader.
- `.font.toml` cooker round-trip.

### v1b — HarfBuzz shaping + billboard text renderer + MTSDF shader

**Scope:**
- HarfBuzz added as a runtime dependency (`crd-font` links it).
- `ITextShaper` interface + `HarfBuzzShaper` implementation.
  `HarfBuzzShaper::shape()` calls `hb_ft_font_create()` + `hb_shape()`, converts HarfBuzz
  glyph positions to `ShapedGlyph` array.
- `TextRenderer` class in `crd-font`: takes a `FontResource&`, `ITextShaper&`, and a string;
  produces a quad mesh (one textured billboard quad per glyph) + UV coordinates into the MTSDF atlas.
- MTSDF surface shader (`.vert.glsl` + `.frag.glsl`): median-of-three on RGB channels for the MSDF
  edge; A channel for SDF effect overlay. `sdf_range` passed as push constant or UBO field.
  Registered as `MaterialDomain::Surface` for world-space billboard text.
- `crd-renderer` wired: `ForwardRenderPath` recognises `MaterialDomain::Surface` MTSDF materials,
  submits billboard quads into the opaque or translucent draw list.
- `smoke_font_billboard.exe` (GPU/window): renders "Hello, Cerid!" as world-space billboard text,
  one frame, exit 0. GPU/window smoke — added to the manual GPU list.

**Tests:**
- `HarfBuzzShaper` shapes ASCII string → glyph IDs non-empty, advances sane.
- `HarfBuzzShaper` shapes Arabic RTL string → glyph IDs valid (tests library linkage).
- `TextRenderer` glyph quads generated for a known string + font (count = shaped glyph count).

### v1c — Dynamic atlas (`DynamicFontAtlas`)

**Scope:**
- `DynamicFontAtlas` class: runtime glyph cache. FreeType + msdfgen become runtime deps in this mode.
- Constructor takes a `ResourceManager*`, an `IAllocator*`, and atlas parameters (initial size,
  padding, glyph pixel size). Lazily rasterizes codepoints via `ensure_glyph(glyph_id)`.
- When atlas is full: doubles atlas side, re-packs all cached glyphs, uploads new `TextureResource`,
  atomically swaps `FontResource::atlas` handle.
- `DynamicFontAtlas::bind(FontResource& out)`: overwrites the `atlas` handle and `glyphs` map of
  a caller-supplied `FontResource` with the current live state. Consumers see the same
  `ResourceHandle<TextureResource>` type regardless of offline vs dynamic origin.
- `smoke_font_dynamic.exe` (GPU/window): creates a `DynamicFontAtlas`, renders ASCII + CJK range,
  forces atlas grow (adds enough glyphs to overflow initial atlas), asserts handle still valid,
  exit 0. GPU/window smoke.

**Tests:**
- `DynamicFontAtlas` rasterizes a glyph on first access, cache hit on second.
- Atlas grow: after overflow, glyph metrics remain valid in new atlas.
- `bind()` writes correct atlas handle and glyph map into `FontResource`.

### v1d — Extruded 3D text mesh (`make_text_mesh`)

**Scope:**
- `crd-font` gains `crd-meshgen` as a dependency (for `MeshData` return type).
- Free function:
  ```cpp
  // engine/font/include/crd/font/text_mesh.hpp
  namespace crd::font
  {
  crd::meshgen::MeshData make_text_mesh(
      crd::containers::StringView  text,
      const FontResource&          font,
      crd::f32                     extrude_depth,
      crd::f32                     bevel_size,
      crd::IAllocator*             a);
  } // namespace crd::font
  ```
- FreeType provides glyph outline Bézier curves (`FT_GLYPH_FORMAT_OUTLINE`). The function
  tessellates each glyph contour (ear-clipping or monotone decomposition) and extrudes the resulting
  polygon along Z by `extrude_depth`. Bevel caps optional when `bevel_size > 0`.
- Output is `crd::meshgen::MeshData` (48B/vertex, same format as `MeshResource` — interleaved
  position + normal + UV0 + tangent). Compatible with the standard `MeshResource` upload path.
- `smoke_font_extruded.exe` (GPU/window): renders "CRD" extruded 3D text, one frame, exit 0. GPU/window smoke.

**Tests:**
- `make_text_mesh("A", font, 0.1f, 0.0f, a)` → vertex count > 0, all UVs in [0,1].
- Zero extrude_depth → flat planar polygon (Z coords all == 0).
- Non-ASCII codepoint (e.g. U+00C9 É) → valid mesh produced.

### v1e — Sandbox integration + Noto Sans asset

**Scope:**
- `assets/source/fonts/NotoSans-Regular.ttf` added to the repository. License: SIL OFL 1.1.
- `assets/source/fonts/NotoSans.font.toml` — character set declaration: Latin + Latin Extended +
  Cyrillic + Greek subsets for the offline-baked atlas.
- Cooker CMake target extended: font handler registered, Noto Sans cooked at build time.
- `crd-sandbox` font panel added to the ImGui asset browser: switch loaded font, display glyph atlas
  texture, render a sample text line in the viewport (billboard), toggle dynamic vs offline atlas.
- `--headless` mode: font panel skipped gracefully.

**Definition of done:** sandbox builds with font panel, `smoke_font_offline.exe` (headless) exits 0,
sandbox renders Noto Sans billboard text on screen with no Vulkan validation errors.

---

## Module layout

```
engine/font/
  include/crd/font/
    font.hpp                  ← umbrella header
    font_resource.hpp         ← FontResource, GlyphMetrics
    text_shaper.hpp           ← ITextShaper, HarfBuzzShaper, ShapedGlyph
    dynamic_font_atlas.hpp    ← DynamicFontAtlas (v1c)
    text_mesh.hpp             ← make_text_mesh() (v1d)
  src/
    font_resource_loader.cpp  ← FontResourceLoader (v1a)
    harfbuzz_shaper.cpp       ← HarfBuzzShaper (v1b)
    text_renderer.cpp         ← TextRenderer (v1b)
    dynamic_font_atlas.cpp    ← DynamicFontAtlas (v1c)
    text_mesh.cpp             ← make_text_mesh() (v1d)
  CMakeLists.txt

assets/source/fonts/
  NotoSans-Regular.ttf        ← demo font (SIL OFL 1.1)
  NotoSans.font.toml          ← character set declaration

runtime/examples/
  smoke_font_offline.cpp      ← v1a (headless)
  smoke_font_billboard.cpp    ← v1b (GPU/window)
  smoke_font_dynamic.cpp      ← v1c (GPU/window)
  smoke_font_extruded.cpp     ← v1d (GPU/window)
```

---

## New CMake dependencies

| Library | Version | License | When |
|---------|---------|---------|------|
| FreeType 2 | ≥ 2.13 | MIT/FTL | cooker always; runtime in v1c+ |
| msdfgen | ≥ 1.9 | MIT | cooker always; runtime in v1c+ |
| HarfBuzz | ≥ 8.0 | MIT | runtime from v1b |

---

## Definition of Done (Phase 3.3)

1. All five slices (v1a–v1e) shipped with unit tests.
2. `smoke_font_offline.exe` exits 0 (headless — in CI matrix).
3. `smoke_font_billboard.exe`, `smoke_font_dynamic.exe`, `smoke_font_extruded.exe` exit 0 on a
   Vulkan-capable machine (GPU/window — manual verification).
4. `crd-sandbox` renders Noto Sans billboard text with no Vulkan validation errors.
5. `docs/systems/font.md` written.
6. Six-configuration green: win-debug / win-relwithdebinfo / win-release / win-asan / win-clang-cl / win-tidy.
7. Noto Sans font attribution recorded in `assets/source/fonts/LICENSES.md`.

---

## Open questions

- **Glyph cache eviction policy in `DynamicFontAtlas`:** When the atlas is full and grow is not
  possible (max size reached), evict LRU glyphs or return an error? Lean toward error + force-grow
  limit configurable at construction time. CJK full character set is ~70k glyphs; a 4096² atlas at
  64px/glyph holds ~4096 glyphs → max size must be configurable.
- **Right-to-left cursor advance:** `ShapedGlyph::advance` can be negative for RTL. `TextRenderer`
  must apply signed advance. Verify with Arabic string in v1b tests.
- **`make_text_mesh` tessellation:** Ear-clipping is O(n²) but trivially implementable and correct
  for typical Latin/CJK glyphs. Monotone decomposition is O(n log n) but complex. Profile at v1d
  to decide.

---

## References

- ADR-0047 — Font rendering system (all 6 decisions)
- ADR-0045 — `assets/source/` layout (fonts subdirectory)
- ADR-0043 — MeshResource vertex layout (extruded text uses same 48B/vertex format)
- ADR-0042 — Texture cooked format (atlas stored as TXTR artifact)
- ADR-0046 — MaterialDomain enum (`Surface` for billboard text, `UI` for screen-space text in Phase 5)
- `docs/phases/phase-2.7-asset-import.md` — TextureResource (atlas resource type)
- `docs/phases/phase-3.2-animation.md` — Predecessor phase
- `docs/phases/phase-3.4-audio.md` — Successor phase
- `docs/systems/sandbox.md` — Phase 3.3 sandbox gains font rendering panel
