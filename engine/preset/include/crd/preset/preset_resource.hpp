#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>

namespace crd::preset
{
// Phase 3.0 v1n1 — Preset substrate (ADR-0059).
//
// CRDR container with a per-type type_fourcc (e.g. 'PRQL', 'PRCM') and three
// chunks (sorted ascending by FourCC at write time per CRDR convention):
//
//   'PCHN' (variable) — extends-chain dependency list:
//                         u32 entry_count + u32 reserved
//                         + PresetChainEntry[entry_count]
//                       Used by the hot-reload watcher to follow upstream
//                       changes through the variant chain.
//   'PDAT' (variable) — flat schema payload bytes (sizeof(T)). Variant chain
//                       is pre-resolved at cook time; field ordering matches
//                       the schema struct's declaration order.
//   'PINF' (16 bytes) — PresetInfo: schema_version + flags + payload_size +
//                       reserved. payload_size is a sanity check against the
//                       registered type's sizeof at load time.
//
// Determinism:
//   - Chunk order is FourCC-sorted by CrdrWriter (PCHN < PDAT < PINF).
//   - Field order in PDAT matches schema struct declaration → same source
//     files + same registration order → bit-exact CRDR bytes.
//   - PCHN entries are written in resolution-deepest-first order (matches
//     the ADR-0058 Öbek extends resolver, since both share that resolver).

inline constexpr crd::u32 kFourCC_PINF = crd::resources::make_fourcc('P', 'I', 'N', 'F');
inline constexpr crd::u32 kFourCC_PDAT = crd::resources::make_fourcc('P', 'D', 'A', 'T');
inline constexpr crd::u32 kFourCC_PCHN = crd::resources::make_fourcc('P', 'C', 'H', 'N');

// PINF chunk payload (16 bytes; pinned for binary-stability across versions).
struct PresetInfo
{
    crd::u32 schema_version{};   // matches T::version at cook time
    crd::u32 flags{};            // reserved; v1n always 0
    crd::u32 payload_size{};     // bytes in PDAT — must equal sizeof(T) at load time
    crd::u32 reserved{};
};
static_assert(sizeof(PresetInfo) == 16, "PresetInfo size pinned at 16 bytes");

// One PCHN entry (16 bytes). Used by the hot-reload watcher to detect
// upstream changes through the extends chain.
struct PresetChainEntry
{
    crd::u64 path_hash{};     // FNV-1a 64 of the canonical preset path
    crd::u64 content_hash{};  // FNV-1a 64 of the source bytes at cook time
};
static_assert(sizeof(PresetChainEntry) == 16, "PresetChainEntry size pinned at 16 bytes");

// Type-erased preset payload owned by ResourceManager after a load.
//
// Consumers that know the schema struct can read it via:
//   const auto* schema = reinterpret_cast<const QualityPresetSchema*>(res.bytes().data());
// after asserting `res.fourcc() == QualityPresetSchema::fourcc` and
// `res.bytes().size() == sizeof(QualityPresetSchema)`.
//
// In v1n1 only the substrate ships; concrete schema types and their
// IPresetTarget::apply() overloads land in v1n2 (QualityPreset) and v1n3
// (CameraPreset).
class PresetResource
{
public:
    explicit PresetResource(crd::memory::IAllocator* a) : m_bytes(a), m_chain(a) {}

    PresetResource(const PresetResource&)            = delete;
    PresetResource& operator=(const PresetResource&) = delete;
    PresetResource(PresetResource&&) noexcept        = default;
    PresetResource& operator=(PresetResource&&) noexcept = default;
    ~PresetResource()                                = default;

    [[nodiscard]] crd::u32 fourcc()         const noexcept { return m_fourcc; }
    [[nodiscard]] crd::u32 schema_version() const noexcept { return m_schema_version; }

    [[nodiscard]] crd::containers::ConstSpan<crd::u8> bytes() const noexcept
    {
        return crd::containers::ConstSpan<crd::u8>{m_bytes.data(), m_bytes.size()};
    }

    [[nodiscard]] crd::containers::ConstSpan<PresetChainEntry> chain_dependencies() const noexcept
    {
        return crd::containers::ConstSpan<PresetChainEntry>{m_chain.data(), m_chain.size()};
    }

    // Mutators — used by PresetLoader during deserialisation. Public to keep
    // the loader free of friend declarations; consumers should treat the
    // resource as read-only after load.
    void set_fourcc(crd::u32 v)         noexcept { m_fourcc = v; }
    void set_schema_version(crd::u32 v) noexcept { m_schema_version = v; }
    [[nodiscard]] crd::containers::Array<crd::u8>&         mutable_bytes() noexcept { return m_bytes; }
    [[nodiscard]] crd::containers::Array<PresetChainEntry>& mutable_chain() noexcept { return m_chain; }

private:
    crd::u32                                  m_fourcc{};
    crd::u32                                  m_schema_version{};
    crd::containers::Array<crd::u8>           m_bytes;
    crd::containers::Array<PresetChainEntry>  m_chain;
};

} // namespace crd::preset
