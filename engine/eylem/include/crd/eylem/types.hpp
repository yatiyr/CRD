#pragma once

// Strong-type identifiers + closed enum tags for the eylem public surface.
// Layout pinned via static_assert; see ADR-0062.

#include <crd/core/types.hpp>

namespace crd::eylem
{
// ---------------------------------------------------------------------------
// Strong-type identifiers
//
// 32-bit handles into per-type pools. Layout [generation:8 | index:24] —
// 16M live bodies/colliders/joints with 256 generation values per slot.
// Generation guards against use-after-free across slot recycling.
//
// Slot index 0 is reserved as null. Default-init = null. Strong typing
// prevents passing a BodyId where a ColliderId was expected.
// ---------------------------------------------------------------------------

struct BodyId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr BodyId null() noexcept { return BodyId{0}; }

    [[nodiscard]] static constexpr BodyId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return BodyId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const BodyId& other) const noexcept = default;
};

struct ColliderId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr ColliderId null() noexcept { return ColliderId{0}; }

    [[nodiscard]] static constexpr ColliderId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return ColliderId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const ColliderId& other) const noexcept = default;
};

struct JointId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr JointId null() noexcept { return JointId{0}; }

    [[nodiscard]] static constexpr JointId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return JointId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const JointId& other) const noexcept = default;
};

// API surface freeze pins (ADR-0062 §15).
static_assert(sizeof(BodyId)     == 4, "BodyId must pack to 4 bytes");
static_assert(sizeof(ColliderId) == 4, "ColliderId must pack to 4 bytes");
static_assert(sizeof(JointId)    == 4, "JointId must pack to 4 bytes");

// ---------------------------------------------------------------------------
// Closed enum tags
// ---------------------------------------------------------------------------

// Determinism contract per ADR-0063. The mode the scene runs under decides
// which guards the implementation enforces.
enum class DeterminismMode : crd::u8
{
    // Same-machine same-compiler bit-exact replay. Default.
    Default = 0,
    // Bit-exact across MSVC/clang/gcc x86/ARM. Requires the v0c
    // crd::math::deterministic substrate + commutative cross-thread merges
    // (eylem v9b 9-config replay-hash CI).
    CrossPlatform = 1,
    // Reserved for serialise-stable across engine versions (post-v9).
    BackwardCompat = 2
};

// Continuous collision detection mode (v6+). v1 always uses Discrete.
enum class CCDMode : crd::u8
{
    Discrete   = 0,
    Linear     = 1,   // sweep test against translation only
    Full       = 2    // sweep against full motion (linear + rotational)
};

// How two material properties combine when bodies collide. Matches the
// PhysX / Bullet convention.
enum class CombineMode : crd::u8
{
    Average  = 0,    // (a + b) * 0.5
    Min      = 1,    // min(a, b)
    Max      = 2,    // max(a, b)
    Multiply = 3     // a * b
};

} // namespace crd::eylem
