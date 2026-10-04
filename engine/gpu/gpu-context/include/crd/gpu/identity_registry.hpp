#pragma once

// DIAG.7a(d2a): the registry that MINTS stable ObjectIdentity values (the device-free half of "mint + name").
//
// Each ObjectKind gets an independent index space -- a `crd::containers::SlotMap` -- so a Resource and a Program can
// share index 0 yet never compare equal (their kind differs). Generations start at 1 and bump on slot reuse, so a
// recycled index never aliases a retired identity: the design's "IDs cannot silently alias when a ... GPU object is
// reused; preserve deleted generation metadata" (design §DIAG.7a, ADR-0133 schema clause 7). That alias-proof
// property is inherited from SlotMap for free -- this type does NOT wrap it in a second mechanism, and it keeps no
// separate "recently retired" log (SlotMap's generation bump IS the retired-provenance primitive; descriptor/range
// provenance is DIAG.7b).
//
// DIAG.7a(d2b) wires mint() at the backend resource/program creation sites and feeds format_debug_name() (in
// <crd/gpu/object_identity.hpp>) to DX12 `SetName` / `vkSetDebugUtilsObjectNameEXT`. WHO OWNS a registry instance
// (per-context / per-backend-module / one shared) is (d2b)'s choice point -- see the session census; this sub-unit
// takes an explicit allocator and no process-global singleton, mirroring Dx12ValidationCapture.
//
// Thread-safe: mint/retire/alive/live_count take an internal mutex, because resource creation and the validation
// callbacks can run concurrently (DIAG.7a(d1) proved concurrent message delivery). No vendor types appear here.

#include <crd/containers/slot_map.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/object_identity.hpp>
#include <crd/memory/allocator.hpp>

#include <mutex>

namespace crd::gpu
{

class IdentityRegistry
{
public:
    explicit IdentityRegistry(memory::IAllocator* allocator = memory::default_allocator())
        : m_slots{crd::containers::SlotMap<crd::u8>(allocator),  // ObjectKind::Resource
                  crd::containers::SlotMap<crd::u8>(allocator),  // ObjectKind::Program
                  crd::containers::SlotMap<crd::u8>(allocator)}  // ObjectKind::Pass
    {
    }

    IdentityRegistry(const IdentityRegistry&)            = delete;
    IdentityRegistry& operator=(const IdentityRegistry&) = delete;
    IdentityRegistry(IdentityRegistry&&)                 = delete;
    IdentityRegistry& operator=(IdentityRegistry&&)      = delete;

    // Mint a fresh, valid identity of `kind`. Generation is 1 for a never-used slot, higher for a reused one.
    [[nodiscard]] ObjectIdentity mint(ObjectKind kind)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto handle = map(kind).insert(crd::u8{0});
        return ObjectIdentity{kind, handle.index, handle.generation};
    }

    // Retire an identity. Returns false for a stale/invalid identity (double-retire rejected). Bumps the slot
    // generation so every outstanding copy of this identity goes stale at once.
    bool retire(const ObjectIdentity& id)
    {
        if (!id.valid())
        {
            return false;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        return map(id.kind).erase(handle_of(id));
    }

    // True iff `id` still names a live slot at its generation.
    [[nodiscard]] bool alive(const ObjectIdentity& id) const
    {
        if (!id.valid())
        {
            return false;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        return map(id.kind).contains(handle_of(id));
    }

    // Live (minted-but-not-retired) count for a kind.
    [[nodiscard]] crd::usize live_count(ObjectKind kind) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return map(kind).size();
    }

private:
    using Map    = crd::containers::SlotMap<crd::u8>;
    using Handle = Map::Handle;

    [[nodiscard]] static crd::usize kind_index(ObjectKind kind) noexcept { return static_cast<crd::usize>(kind); }
    [[nodiscard]] Map&              map(ObjectKind kind) noexcept { return m_slots[kind_index(kind)]; }
    [[nodiscard]] const Map&        map(ObjectKind kind) const noexcept { return m_slots[kind_index(kind)]; }
    [[nodiscard]] static Handle     handle_of(const ObjectIdentity& id) noexcept
    {
        return Handle{id.index, id.generation};
    }

    mutable std::mutex m_mutex;
    Map                m_slots[3]; // indexed by ObjectKind: Resource, Program, Pass
};

// DIAG.7a(d2b): the process-wide registry. Identity uniqueness scope must equal the validation capture's correlation
// scope, and the DX12 capture is PROCESS-WIDE (it registers on every device via a static registry) -- so two DX12
// contexts must not each mint `res:0` into that one capture. A single shared registry (one index space across both
// backends) is therefore stronger than a per-context or per-backend one, and it is the natural correlation anchor.
// Function-local static: constructed on first use with default_allocator() (itself a static that outlives it, so
// destruction order is safe), and NO init parameter (a first-call-wins allocator argument would be a silent scar).
// DEFINED OUT-OF-LINE in identity_registry.cpp (NOT inline) on its own merits: a single definition point for a
// process-wide static, rather than a linkonce_odr Meyers singleton emitted into every calling TU. (A win-clang-cl-
// shipping thin-LTO lld-link crash seen during DIAG.7a(d2b) is a NON-DETERMINISTIC toolchain crash in LTO codegen --
// varying signature, reproduces even in isolation -- not tied to this accessor; see the session doc.)
[[nodiscard]] IdentityRegistry& identity_registry();

} // namespace crd::gpu
