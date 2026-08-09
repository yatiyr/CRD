#pragma once

// crd-ceir — the DETERMINISM + NUMERICAL-SEMANTICS vocabulary (CEIR-4b, §27/§28). Two axes the compiler reasons about:
//   • a per-op-KIND DeterminismClass (a semantic contract: how reproducible is this op?), carried on OpInfo beside its
//     effects (CEIR-4a), declared via the 2a schema, checked against the active CompilerMode; and
//   • a per-op-INSTANCE NumericalSemantics (§28 knobs — IEEE/FMA/FTZ/rounding/…): the SAME add can be strict here and
//     fast there, so it rides the existing per-op ATTRIBUTE machinery (one packed `numerics` int attr; no new grammar).
// The mode↔class and mode↔numerics legality predicates are the ENFORCEMENT primitive an optimization pass will call; no
// pass manager exists until CEIR-6, so 4b delivers the predicates + a module-walk that finds the first violation (the
// CEIR-4a "predicate-now, wire-when-a-pass-exists" precedent).

#include <crd/ceir/effect.hpp> // EffectFamily (effect_legal_in_region composes §26 effects with the region tag)
#include <crd/core/types.hpp>

namespace crd::ceir
{
// ── §27 determinism ──
// The ADR-0098 determinism tiers (§27), plus `Unspecified` (the DEFAULT — an op that makes NO claim). ⛔ Unspecified is
// NOT "Nondeterministic": absence of a claim ≠ a positive claim (the func.call/effects landmine). The legality predicate
// treats Unspecified conservatively (it satisfies only Normal/Fast). ⛔ APPEND real tiers AT END, keep in lockstep with
// the generator's DETERMINISM_TIERS (tuple index = ordinal-1, since Unspecified=0 is not declarable in TOML).
// NOLINTNEXTLINE(performance-enum-size)
enum class DeterminismClass : u8
{
    Unspecified = 0,            // no claim (default) — conservative
    BitExact,                   // identical bits on every target AND backend (the strongest tier)
    DeterministicWithinTarget,  // reproducible on one target across runs
    DeterministicWithinBackend, // reproducible on one backend
    Nondeterministic,           // explicitly not reproducible (unordered atomics, races-by-design)
    ExternalNondeterminism,     // nondeterminism sourced from OUTSIDE (time / random / input / external services)
};
inline constexpr DeterminismClass kLastDeterminismClass = DeterminismClass::ExternalNondeterminism;
// ⛔ Cross-language lockstep: §27 has 5 declarable tiers (ordinals 1..5; Unspecified=0 is the default). If this fires,
// semantics.hpp and the generator's DETERMINISM_TIERS (tools/ceir_opgen/ceir_opgen.py) diverged. Keep both in sync.
static_assert(static_cast<u8>(kLastDeterminismClass) == 5U, "DeterminismClass has 5 tiers (§27); sync with ceir_opgen");

// The COMPILER MODES (§27) — a session-level constraint on which determinism classes are legal (and, for Fast, on
// numerics). NOT module content (never serialized). Default `Normal`.
// NOLINTNEXTLINE(performance-enum-size)
enum class CompilerMode : u8
{
    Normal = 0,             // default — no determinism constraint; standard numerics
    Fast,                   // no determinism constraint; ADMITS aggressive numerics (FMA / fast-math)
    Deterministic,          // requires a deterministic tier (BitExact / WithinTarget / WithinBackend)
    CertifiedDeterministic, // requires BitExact
};

// A strength rank for the op-vs-native consistency check (higher = stronger claim). ⛔ The Nondeterministic ≥
// ExternalNondeterminism ordering is a judgement call — External is a NARROWER source of nondeterminism (outside input),
// so it ranks just below plain Nondeterministic; Unspecified (no claim) ranks lowest. Used only for "native must be
// at-least-as-strong as the op's abstract claim".
[[nodiscard]] constexpr u8 determinism_rank(DeterminismClass c) noexcept
{
    switch (c)
    {
    case DeterminismClass::BitExact: return 5U;
    case DeterminismClass::DeterministicWithinTarget: return 4U;
    case DeterminismClass::DeterministicWithinBackend: return 3U;
    case DeterminismClass::Nondeterministic: return 2U;
    case DeterminismClass::ExternalNondeterminism: return 1U;
    case DeterminismClass::Unspecified: return 0U;
    }
    return 0U;
}

// Does an op of determinism class `c` satisfy the active compiler mode `m`? (§27 "passes may not silently violate an
// active determinism contract".) Normal/Fast constrain determinism not at all (Fast differs from Normal in NUMERICS, not
// determinism); Deterministic needs a deterministic tier; Certified needs BitExact. Unspecified (no claim) fails both
// strict modes — you cannot certify what was never classified.
[[nodiscard]] constexpr bool determinism_satisfies_mode(DeterminismClass c, CompilerMode m) noexcept
{
    switch (m)
    {
    case CompilerMode::Normal:
    case CompilerMode::Fast:
        return true;
    case CompilerMode::Deterministic:
        return c == DeterminismClass::BitExact || c == DeterminismClass::DeterministicWithinTarget ||
               c == DeterminismClass::DeterministicWithinBackend;
    case CompilerMode::CertifiedDeterministic:
        return c == DeterminismClass::BitExact;
    }
    return false;
}

// ── §15 evaluation domains + §32 real-time classes (CEIR-4c) ──
// The §15 evaluation domains (WHEN a computation runs), plus `Unspecified` (the default/no-claim). ⛔ APPEND real domains
// AT END, keep in lockstep with the generator's EVAL_DOMAINS (tuple index = ordinal-1). Carried per-op-KIND on OpInfo
// (the op's domain affinity) AND per-REGION (packed into a `region_exec` attr on the region-owning op).
// NOLINTNEXTLINE(performance-enum-size)
enum class EvalDomain : u8
{
    Unspecified = 0,
    CompileTime,
    CookTime,
    LoadTime,
    HostFrameTime,
    HostSimulationTime,
    HostAudioTime,
    DeviceTime,
    OfflineTime,
    DistributedTime,
    EitherHostOrDevice,
};
inline constexpr EvalDomain kLastEvalDomain = EvalDomain::EitherHostOrDevice;
static_assert(static_cast<u8>(kLastEvalDomain) == 10U, "EvalDomain has 10 §15 domains; sync with ceir_opgen EVAL_DOMAINS");

// The §32 real-time execution classes (a region's DEADLINE class), plus `Unspecified`. NOT a 2a-schema field — it is a
// REGION property (packed into `region_exec`), never declared per-op-kind — so there is deliberately NO generator
// vocabulary to keep in lockstep. `Offline` (a deadline class) and `OfflineTime` (an eval domain) are DISTINCT, on
// different axes. ⛔ APPEND AT END.
// NOLINTNEXTLINE(performance-enum-size)
enum class RealtimeClass : u8
{
    Unspecified = 0,
    FrameCritical,
    SimulationCritical,
    AudioRealTime,
    LatencySensitive,
    Throughput,
    Background,
    Offline,
};
inline constexpr RealtimeClass kLastRealtimeClass = RealtimeClass::Offline;
static_assert(static_cast<u8>(kLastRealtimeClass) == 7U, "RealtimeClass has 7 §32 classes");

// A REGION's execution tag (§15 domain + §32 realtime class). Stored as ONE packed `region_exec` int attribute on the
// region-OWNING op (regions have no attr dict; this rides the existing attr machinery — no new serialized surface, no
// binary version bump). domain in nibble 0, realtime in nibble 1.
struct RegionExec
{
    EvalDomain    domain   = EvalDomain::Unspecified;
    RealtimeClass realtime = RealtimeClass::Unspecified;
    [[nodiscard]] friend constexpr bool operator==(const RegionExec&, const RegionExec&) noexcept = default;
};
[[nodiscard]] constexpr i64 pack_region_exec(const RegionExec& r) noexcept
{
    return static_cast<i64>(static_cast<u64>(r.domain) | (static_cast<u64>(r.realtime) << 4U));
}
// Decode + VALIDATE (a nibble past its enum, or any bit ≥ 8, is a corrupt attr — rejected without touching `out`).
[[nodiscard]] constexpr bool unpack_region_exec(i64 packed, RegionExec& out) noexcept
{
    const auto bits = static_cast<u64>(packed);
    if ((bits >> 8U) != 0U) { return false; }
    const auto d  = static_cast<u8>(bits & 0xFU);
    const auto rt = static_cast<u8>((bits >> 4U) & 0xFU);
    if (d > static_cast<u8>(kLastEvalDomain) || rt > static_cast<u8>(kLastRealtimeClass)) { return false; }
    out.domain   = static_cast<EvalDomain>(d);
    out.realtime = static_cast<RealtimeClass>(rt);
    return true;
}

// The SEEDED §32 legality rule (the extensible point): a filesystem / blocking-network effect — OR an UNMODELED
// `ExternalCall` (CEIR-5c) — may not run in a real-time AUDIO region (`realtime == AudioRealTime` OR the §15 audio/device
// domains `DeviceTime` / `HostAudioTime`). ⭐ CEIR-5c FLIPPED `ExternalCall` from legal-for-now to FORBIDDEN: now that the
// EffectsFn hook resolves a `func.call` to its callee's PRECISE families (a call to a pure func carries NO ExternalCall),
// a *remaining* `ExternalCall` means genuinely-unmodeled code (an unresolved call / an unregistered op), which must not
// run in an audio-RT region. A resolved pure call passes; an unresolved call flags. Returns true iff `f` is legal under `r`.
[[nodiscard]] constexpr bool effect_legal_in_region(EffectFamily f, const RegionExec& r) noexcept
{
    const bool audio_rt = r.realtime == RealtimeClass::AudioRealTime || r.domain == EvalDomain::DeviceTime ||
                          r.domain == EvalDomain::HostAudioTime;
    // ⭐ CEIR-6a: `Synchronization` (a BLOCKING wait — ceir.async's await/join/race) joins the forbidden set. A blocking
    // wait in an audio-real-time callback is the priority-inversion bug §32 exists to forbid; band 6 introduces the
    // engine's first blocking waits, so the flip lands with them (not a new "legal-for-now").
    // ⛔ CEIR-8c (ADR-0113) DELIBERATE classification of the 8 U-§19 families for audio-RT legality — this is the SECOND
    // family consumer (the -Werror=switch guard does NOT cover this predicate, so each is decided by hand):
    //   • TransactionBoundary → FORBIDDEN: a commit/rollback can block or run unbounded (the Synchronization rationale).
    //   • AgentAction        → FORBIDDEN: an agent decision is unbounded external-ish work (the ExternalCall rationale).
    //   • DocumentRead/Write, ConstraintRead/Write, UIRead/Write → LEGAL: bounded domain-state access, exactly analogous
    //     to SceneRead/Write · EcsRead/Write · HostStateRead/Write, which are already legal in an audio-RT region. Over-
    //     forbidding a bounded read/write here would break legitimate real-time patterns, not add safety.
    return !(audio_rt && (f == EffectFamily::FileIO || f == EffectFamily::NetworkIO || f == EffectFamily::ExternalCall ||
                          f == EffectFamily::Synchronization || f == EffectFamily::TransactionBoundary ||
                          f == EffectFamily::AgentAction));
}

// ── §28 numerical semantics (per-INSTANCE) ── one field per §28 line, verbatim. Each field's 0 = INHERIT (from the mode
// / the surrounding default), so a partial attribute composes. Shared `Toggle` for the pure inherit/off/on knobs.
// NOLINTNEXTLINE(performance-enum-size)
enum class Toggle : u8 { Inherit = 0, Off, On };
// NOLINTNEXTLINE(performance-enum-size)
enum class IeeeMode : u8 { Inherit = 0, Strict, Relaxed };
// NOLINTNEXTLINE(performance-enum-size)
enum class DenormMode : u8 { Inherit = 0, Preserve, Flush };
// NOLINTNEXTLINE(performance-enum-size)
enum class RoundingMode : u8 { Inherit = 0, NearestEven, TowardZero, TowardPositive, TowardNegative };
// NOLINTNEXTLINE(performance-enum-size)
enum class OverflowMode : u8 { Inherit = 0, Wrap, Saturate, Trap };
// NOLINTNEXTLINE(performance-enum-size)
enum class IntWrapMode : u8 { Inherit = 0, Wrap, Trap };
// NOLINTNEXTLINE(performance-enum-size)
enum class NanMode : u8 { Inherit = 0, Quiet, Signaling, AssumeNoNaN };

// The twelve §28 knobs. Each ≤ 5 values ⇒ fits a NIBBLE; the whole struct packs into a single i64 attribute (48 bits
// used) that rides the existing attr text/binary machinery — no new grammar, no new serialized surface.
struct NumericalSemantics
{
    IeeeMode     ieee              = IeeeMode::Inherit;     // IEEE mode
    Toggle       fast_math         = Toggle::Inherit;       // fast math
    Toggle       fma               = Toggle::Inherit;       // contraction / FMA
    Toggle       flush_to_zero     = Toggle::Inherit;       // flush-to-zero
    DenormMode   denorm            = DenormMode::Inherit;    // denorm behavior
    RoundingMode rounding          = RoundingMode::Inherit;  // rounding
    OverflowMode overflow          = OverflowMode::Inherit;  // overflow behavior
    IntWrapMode  int_wrap          = IntWrapMode::Inherit;   // integer wrapping/trapping
    NanMode      nan               = NanMode::Inherit;       // NaN semantics
    Toggle       precision_promote = Toggle::Inherit;        // precision promotion
    Toggle       mixed_precision   = Toggle::Inherit;        // mixed precision
    Toggle       stochastic_round  = Toggle::Inherit;        // stochastic rounding
    [[nodiscard]] friend constexpr bool operator==(const NumericalSemantics&, const NumericalSemantics&) noexcept = default;
};

// The per-field VALUE COUNT (for the unpack bounds check — a nibble ≥ the field's count is a corrupt attr, rejected).
inline constexpr u8 kNumericsFieldCount[12] = {3U, 3U, 3U, 3U, 3U, 5U, 4U, 3U, 4U, 3U, 3U, 3U};

// Pack the twelve nibbles into a single i64 (field i at bits [4i, 4i+4)). The exact inverse of `unpack_numerics`.
[[nodiscard]] constexpr i64 pack_numerics(const NumericalSemantics& n) noexcept
{
    const u8 f[12] = {static_cast<u8>(n.ieee),     static_cast<u8>(n.fast_math),      static_cast<u8>(n.fma),
                      static_cast<u8>(n.flush_to_zero), static_cast<u8>(n.denorm),    static_cast<u8>(n.rounding),
                      static_cast<u8>(n.overflow), static_cast<u8>(n.int_wrap),       static_cast<u8>(n.nan),
                      static_cast<u8>(n.precision_promote), static_cast<u8>(n.mixed_precision),
                      static_cast<u8>(n.stochastic_round)};
    u64 bits = 0U;
    for (u32 i = 0; i < 12U; ++i) { bits |= static_cast<u64>(f[i]) << (4U * i); }
    return static_cast<i64>(bits);
}

// Decode a packed i64 back into `out`, VALIDATING every field (a nibble ≥ its field count, or any bit at/above 48, is a
// corrupt/garbage attribute). Returns false without touching `out` on any violation — the "decoder arm" for this path
// (the binary layer stores it as an opaque i64; the semantics check lives here).
[[nodiscard]] constexpr bool unpack_numerics(i64 packed, NumericalSemantics& out) noexcept
{
    const auto bits = static_cast<u64>(packed);
    if ((bits >> 48U) != 0U) { return false; } // only the low 48 bits (12 nibbles) are defined
    u8 f[12] = {};
    for (u32 i = 0; i < 12U; ++i)
    {
        f[i] = static_cast<u8>((bits >> (4U * i)) & 0xFU);
        if (f[i] >= kNumericsFieldCount[i]) { return false; } // out-of-range enum ⇒ reject
    }
    out.ieee              = static_cast<IeeeMode>(f[0]);
    out.fast_math         = static_cast<Toggle>(f[1]);
    out.fma               = static_cast<Toggle>(f[2]);
    out.flush_to_zero     = static_cast<Toggle>(f[3]);
    out.denorm            = static_cast<DenormMode>(f[4]);
    out.rounding          = static_cast<RoundingMode>(f[5]);
    out.overflow          = static_cast<OverflowMode>(f[6]);
    out.int_wrap          = static_cast<IntWrapMode>(f[7]);
    out.nan               = static_cast<NanMode>(f[8]);
    out.precision_promote = static_cast<Toggle>(f[9]);
    out.mixed_precision   = static_cast<Toggle>(f[10]);
    out.stochastic_round  = static_cast<Toggle>(f[11]);
    return true;
}

// Does an instance's numerics `n` satisfy the active mode `m`? Certified/Deterministic forbid the knobs that break
// reproducibility (Relaxed IEEE, fast-math ON). ⛔ Fast/Normal ADMIT everything — Fast must NOT forbid FMA/fast-math (the
// fmad scar: bit-exact flags cripple GEMM). FMA itself is allowed even under Certified (contraction is deterministic when
// applied consistently — a bit-exact GEMM/FFT wants it).
[[nodiscard]] constexpr bool numerics_satisfies_mode(const NumericalSemantics& n, CompilerMode m) noexcept
{
    if (m == CompilerMode::Deterministic || m == CompilerMode::CertifiedDeterministic)
    {
        return n.ieee != IeeeMode::Relaxed && n.fast_math != Toggle::On;
    }
    return true; // Normal / Fast: no numerics constraint (Fast admits the aggressive knobs)
}
} // namespace crd::ceir
