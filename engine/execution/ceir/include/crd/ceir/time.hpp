#pragma once

// crd-ceir — the TIME-DOMAIN dialect (CEIR-8f, ADR-0116, U-§17). ⛔ A time domain IS an 8a TYPE-CLASS (there is NO
// separate TimeDomainId — that would fork TypeClassId). The six built-ins — wall / sim / frame / audio_sample /
// sequencer / logical — are dialect-defined type-classes under `time`; a time TYPE is an `Extern` type of the domain
// class over ONE underlying numeric/quantity member (the crd-units Time seed). Because two different type-classes with
// identical params are DIFFERENT TypeIds (the ADR-0111 landmine), `time.wall<T> != time.sim<T>` — which IS "mixing is a
// type error" the moment a checker looks (operand-type checking lands at CEIR-3/4; the enforcement is named-forward).
// A plugin clock (`game.turn`) works with ZERO central edits — the open-world proof.

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::time
{
// Register the `time` dialect + its six built-in domain type-classes on `ctx`. Idempotent (re-registration is a no-op).
Dialect* register_dialect(Context& ctx);

// The interned domain type-class for `name` (e.g. "wall") = intern_type_class("time", name). Works for a built-in OR a
// not-yet-registered domain (the id is a content hash); register_dialect gives the built-ins their verify hook.
[[nodiscard]] TypeClassId domain(Context& ctx, containers::StringView name);

// Build a time TYPE in domain-class `cls` over `underlying` (a numeric/quantity type). The Extern type carries the
// domain, so distinct domains are distinct TypeIds. `cls` must be registered (type_extern asserts its verify hook).
[[nodiscard]] TypeId time_type(Context& ctx, TypeClassId cls, TypeId underlying);

// The six built-in domains, in their stable order: wall, sim, frame, audio_sample, sequencer, logical. DIAG.9a: a
// built-in's ordinal is the input seam's channel for input.clock / input.time_step and is stored in run records, so
// the order only grows at the end.
inline constexpr crd::u32 kBuiltinDomainCount = 6U;

// The ordinal of the built-in domain `name` in [0, kBuiltinDomainCount), or false for any other name (a plugin domain
// is a valid type-class but has no ordinal).
[[nodiscard]] bool builtin_domain_index(containers::StringView name, crd::u32& out) noexcept;

// The name of built-in domain `index`, or an empty view when `index` is out of range.
[[nodiscard]] containers::StringView builtin_domain_name(crd::u32 index) noexcept;
} // namespace crd::ceir::time
