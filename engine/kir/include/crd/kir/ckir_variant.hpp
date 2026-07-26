#pragma once

// ckir_variant.hpp — REN-37.7 (D-007 row 140): THE VARIANT MATRIX, and the übershader/variant DUAL MODE.
//
// The classical trade is: ONE program with dynamic branches (worst-case registers, everything slightly slow) vs
// SPECIALIZE EVERYTHING (optimal codegen, thousands of PSOs and a cook-time blowup). Most engines pick a point on
// that line. **We do not have to, because we own an IR**: `specialize_variant` + `lower_entry` mechanically reduce
// ONE authored übergraph to an optimal static variant, so the übershader and the variant are the SAME AUTHORED
// ARTIFACT AT TWO LOWERING LEVELS. A second authoring path would be the real mistake — it is how engines end up
// with an editor look that differs from the shipped one.
//
//   SHIP mode   — specialize every DECLARED combination, lower, dedup by content hash. No runtime branching,
//                 PSO set known ahead of time.
//   EDITOR mode — emit the SAME graph with branch options left as UNIFORM reads (uniform control flow is nearly
//                 free — the branch is coherent across the whole draw). One program, instant material tweaks.
//
// ⛔ THE MATRIX IS **DECLARED, NOT DISCOVERED**. Every axis here comes from something an author wrote down: the
// technique's `TechniqueOption` range, the material's `ShadingModel` tag, the pass list a frame graph asks for.
// Nothing enumerates combinations no asset requested. Plus Filament's `variantFilter` idea as a first-class
// NEGATIVE declaration: an app states combinations it guarantees are never needed, and they are never cooked.
//
// ⭐ THE COLLAPSE THAT IS ALREADY FREE. For `PassType::Shadow`, `build_fs_for_pass` sets `n_out = 0` and never
// consumes the surface, so `lower_entry` DCEs the ENTIRE surface computation — every texture fetch, every
// parameter, every interpolant. A THOUSAND materials therefore cook to the SAME empty shadow program and dedup to
// ONE by content hash. What other engines hand-engineer as "depth-only permutations" falls out of lowering plus a
// hash, because we compose in an IR rather than in text. `dedup_ratio()` measures it instead of asserting it.

#include <crd/kir/ckir_serialize.hpp>
#include <crd/kir/ckir_technique.hpp>

#include <crd/containers/array.hpp>

namespace crd::kir::technique
{

inline constexpr int kMaxVariantOptions = 8;

// ── The variant KEY ──────────────────────────────────────────────────────────────────────────────────────────
// ⛔ Every field here is a DECLARED axis. Adding one is a deliberate change with its own gate — an undeclared axis
// is an unbounded matrix, and an axis that silently stops being part of the key is a dedup collision (two
// different programs hashing the same and one of them being dropped).
struct VariantKey
{
    crd::u64            material  = 0; // the authored material's content id
    crd::u64            technique = 0; // the technique's name hash
    cook::PassType      pass      = cook::PassType::Forward;
    material::AlphaMode alpha     = material::AlphaMode::Opaque;
    crd::u32            shading   = 0; // the material's ShadingModel TAG (the material declares intent)
    crd::u32            vertex    = 0; // vertex-layout id
    int                 options[kMaxVariantOptions] = {};
    int                 n_options = 0;
};

namespace detail
{
inline constexpr crd::u64 kFnvOffset = 14695981039346656037ULL;
inline constexpr crd::u64 kFnvPrime  = 1099511628211ULL;

inline void hash_u64(crd::u64& h, crd::u64 v) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        h ^= (v >> (i * 8)) & 0xFFULL;
        h *= kFnvPrime;
    }
}
} // namespace detail

// The key's own hash — used to LOOK UP a cooked program at runtime. Distinct from the CONTENT hash below, and the
// distinction matters: two different keys legitimately map to the SAME content (that is dedup working), so the two
// hashes must never be conflated.
[[nodiscard]] inline crd::u64 variant_key_hash(const VariantKey& k) noexcept
{
    crd::u64 h = detail::kFnvOffset;
    detail::hash_u64(h, k.material);
    detail::hash_u64(h, k.technique);
    detail::hash_u64(h, static_cast<crd::u64>(k.pass));
    detail::hash_u64(h, static_cast<crd::u64>(k.alpha));
    detail::hash_u64(h, k.shading);
    detail::hash_u64(h, k.vertex);
    for (int i = 0; i < k.n_options; ++i) { detail::hash_u64(h, static_cast<crd::u64>(k.options[i])); }
    return h;
}

// The CONTENT hash — over the canonical serialization of the LOWERED graph. `serialize_graph` is padding-free and
// ABI-independent by construction (the scar that made it so), so this is a pure function of the graph's content:
// two variants that lower to the same IR hash identically on any compiler, on any platform.
[[nodiscard]] inline crd::u64 graph_content_hash(const KGraph& g, const KEntry& e, crd::memory::IAllocator* a)
{
    const crd::containers::Array<crd::u8> bytes = serialize_graph(g, e, a);
    crd::u64                              h     = detail::kFnvOffset;
    for (crd::usize i = 0; i < bytes.size(); ++i)
    {
        h ^= bytes[i];
        h *= detail::kFnvPrime;
    }
    return h;
}

// ── Declaring the matrix ────────────────────────────────────────────────────────────────────────────────────
// One axis = one option and the values it is enumerated over. The values come from the technique's declared
// `TechniqueOption` range; a caller may narrow that further (a project that only ever ships 4-tap PCF declares
// exactly that), never widen it.
struct VariantAxis
{
    const char* name       = nullptr;
    const int*  values     = nullptr;
    int         n_values   = 0;
    // ⛔ A SHAPE option changes the GRAPH'S STRUCTURE (a PCF tap count unrolls a different number of samples; a
    // cascade count builds a different number of projections). It can only ever be a static specialization —
    // there is no uniform branch that unrolls a loop. A BRANCH option (`shadows on/off`, `textured`) selects
    // between two computed values and CAN be a uniform read in editor mode.
    // Stating this per-option is what keeps the editor übershader honest: it does not pretend to cover axes it
    // structurally cannot, it pins them at their default and recooks when one changes.
    bool        shape      = false;
};

// A Filament-style NEGATIVE declaration: return true for a key the app guarantees it never needs. Filtering is
// applied BEFORE cooking, so a filtered variant costs nothing at all — not cook time, not memory, not a hash.
using VariantFilterFn = bool (*)(const VariantKey& key, void* user);

enum class VariantMode : crd::u8
{
    Ship = 0, // specialize every declared combination; optimal codegen, no runtime branching
    Editor,   // ONE program from the same graph, branch options left as uniform reads; instant iteration
};

// One cooked entry of the matrix.
struct VariantEntry
{
    VariantKey key{};
    crd::u64   key_hash     = 0;
    crd::u64   content_hash = 0;
    crd::u32   program      = 0; // index into the DEDUPED program set
};

// The result of cooking a declared matrix.
struct VariantMatrix
{
    crd::containers::Array<VariantEntry> entries;   // one per enumerated (unfiltered) combination
    crd::containers::Array<crd::u64>     programs;  // the DISTINCT content hashes, in first-seen order
    crd::u32                             filtered = 0; // combinations a `variantFilter` removed

    explicit VariantMatrix(crd::memory::IAllocator* a) : entries(a), programs(a) {}

    [[nodiscard]] crd::u32 variant_count() const noexcept { return static_cast<crd::u32>(entries.size()); }
    [[nodiscard]] crd::u32 program_count() const noexcept { return static_cast<crd::u32>(programs.size()); }
    // How much the content hash collapsed the declared matrix. 1.0 = every variant is distinct; N = an N-fold
    // collapse. ⛔ REPORTED, never assumed: a collapse that silently stopped happening would show up as a cook
    // time and memory regression with nothing pointing at it.
    [[nodiscard]] double dedup_ratio() const noexcept
    {
        return programs.empty() ? 0.0
                                : static_cast<double>(entries.size()) / static_cast<double>(programs.size());
    }
};

// ⛔ EVERYTHING GRAPH-LOCAL LIVES HERE, AND THE MATRIX ASKS FOR IT PER COOK. A CKIR node id is an index into ONE
// graph, and the matrix cooks each variant into a FRESH graph — so it cannot be handed ids, it must be handed a
// BUILDER it calls with the graph it is about to fill. Passing ids across graphs is not a subtle mistake: the
// builders would walk an operand index that means something else entirely, or nothing at all.
struct VariantEnv
{
    cook::SurfaceInputs in;                  // the per-fragment varyings this renderer supplies
    int                 light_dir   = -1;
    int                 light_color = -1;
    const int*          bindings    = nullptr; // the technique's resolved binding nodes, in ABI order
    int                 n_bindings  = 0;
};

// Build the graph-local environment into `g`. Called once per cooked variant.
using VariantEnvFn = void (*)(KGraph& g, VariantEnv& out, void* user);

// What the matrix cooks over: one material, one technique, the passes to cover, and the declared axes.
struct VariantRequest
{
    crd::u64                material_id = 0;
    const cook::MaterialTemplate* material = nullptr;
    const Technique*        tech        = nullptr;
    const cook::PassType*   passes      = nullptr;
    int                     n_passes    = 0;
    const VariantAxis*      axes        = nullptr;
    int                     n_axes      = 0;
    material::AlphaMode     alpha       = material::AlphaMode::Opaque;
    crd::u32                shading     = 0;
    crd::u32                vertex      = 0;
    VariantFilterFn         filter      = nullptr;
    void*                   filter_user = nullptr;
    VariantMode             mode        = VariantMode::Ship;
    VariantEnvFn            env         = nullptr; // REQUIRED — see VariantEnv
    void*                   env_user    = nullptr;
};

// Builds ONE variant's FS into `g`/`e`. Exposed so a caller can cook the actual program for an entry after the
// matrix has told it which entries are distinct. `option_values` must have `n_axes` entries.
//
// The `SurfaceInputs` and the binding nodes are supplied by the caller because they are RENDERER-shaped (which
// varyings exist, where a pass-frequency value lives) — the matrix is deliberately ignorant of both.
[[nodiscard]] inline bool build_variant(const VariantRequest& req, cook::PassType pass, const int* option_values,
                                        KGraph& g, KEntry& e)
{
    if (req.material == nullptr || req.tech == nullptr || req.env == nullptr) { return false; }
    VariantEnv env;
    req.env(g, env, req.env_user); // graph-local ids, built into THIS graph
    crd::f64 vals[kMaxVariantOptions] = {};
    for (int i = 0; i < req.n_axes && i < kMaxVariantOptions; ++i)
    {
        vals[i] = static_cast<crd::f64>(option_values[i]);
    }
    const cook::VariantOptions opts{req.alpha, 0.5};
    return build_fs_for_pass(*req.material, *req.tech, pass, opts, env.in, g, e, env.light_dir, env.light_color,
                             env.bindings, env.n_bindings, vals, req.n_axes);
}

// ── Cook the declared matrix ────────────────────────────────────────────────────────────────────────────────
// Enumerates passes × the cartesian product of the declared axes, applies the negative filter, builds and LOWERS
// each combination, hashes the lowered IR, and deduplicates.
//
// ⛔ EDITOR MODE COOKS EXACTLY ONE ENTRY PER PASS. Branch options collapse into a single program with uniform
// reads; SHAPE options (which no uniform can express) are pinned at the FIRST declared value and the program is
// recooked when one changes. Stating that is the difference between a dual mode and a lie: the editor path covers
// the axes it can and says so about the ones it cannot.
[[nodiscard]] inline bool cook_variant_matrix(const VariantRequest& req, crd::memory::IAllocator* alloc,
                                              VariantMatrix& out)
{
    if (req.material == nullptr || req.tech == nullptr || req.env == nullptr || req.n_passes <= 0) { return false; }
    if (req.n_axes > kMaxVariantOptions) { return false; }

    // total combinations across the declared axes (1 when there are none)
    crd::u64 combos = 1;
    for (int a = 0; a < req.n_axes; ++a)
    {
        if (req.axes[a].n_values <= 0) { return false; } // an axis with no values is a matrix with no variants
        combos *= static_cast<crd::u64>(req.axes[a].n_values);
    }
    if (req.mode == VariantMode::Editor) { combos = 1; }

    for (int pi = 0; pi < req.n_passes; ++pi)
    {
        for (crd::u64 c = 0; c < combos; ++c)
        {
            VariantKey key;
            key.material  = req.material_id;
            key.technique = 0;
            for (const char* p = req.tech->name; p != nullptr && *p != '\0'; ++p)
            {
                key.technique ^= static_cast<crd::u64>(static_cast<unsigned char>(*p));
                key.technique *= detail::kFnvPrime;
            }
            key.pass      = req.passes[pi];
            key.alpha     = req.alpha;
            key.shading   = req.shading;
            key.vertex    = req.vertex;
            key.n_options = req.n_axes;

            int      values[kMaxVariantOptions] = {};
            crd::u64 rest                       = c;
            for (int a = 0; a < req.n_axes; ++a)
            {
                if (req.mode == VariantMode::Editor)
                {
                    // shape options pin at their first declared value; branch options are uniform at runtime, so
                    // the cooked value is irrelevant and the first is as good as any.
                    values[a] = req.axes[a].values[0];
                }
                else
                {
                    const auto n = static_cast<crd::u64>(req.axes[a].n_values);
                    values[a]    = req.axes[a].values[rest % n];
                    rest /= n;
                }
                key.options[a] = values[a];
            }

            if (req.filter != nullptr && req.filter(key, req.filter_user))
            {
                ++out.filtered;
                continue;
            }

            KGraph g(alloc);
            KEntry e;
            if (!build_variant(req, key.pass, static_cast<const int*>(values), g, e)) { return false; }

            VariantEntry entry;
            entry.key          = key;
            entry.key_hash     = variant_key_hash(key);
            entry.content_hash = graph_content_hash(g, e, alloc);
            entry.program      = static_cast<crd::u32>(out.programs.size());
            for (crd::usize p = 0; p < out.programs.size(); ++p)
            {
                if (out.programs[p] == entry.content_hash)
                {
                    entry.program = static_cast<crd::u32>(p);
                    break;
                }
            }
            if (entry.program == static_cast<crd::u32>(out.programs.size()))
            {
                out.programs.push_back(entry.content_hash);
            }
            out.entries.push_back(entry);
        }
    }
    return true;
}

} // namespace crd::kir::technique
