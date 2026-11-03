// crd-crypto -- an installation's crypto policy state: the saved generation and floor, and the build's bootstrap
// (NET.2; contract: docs/design/network-foundation.md#policy-state).

#include <crd/crypto/policy_state.hpp>

#include "byte_spans.hpp"
#include "policy_bootstrap.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>

#include <algorithm>

namespace crd::crypto
{
namespace
{
namespace fs = crd::platform::fs;

// The saved state, one durable file (durable_file.hpp: its envelope checks the bytes) with one older save kept:
//   format (u8, kStateFormat) | floor (u64) | generation length (u32, at most kMaxPolicyBytes) | generation
// all big-endian, exact: any other payload is Damaged.
constexpr char  kStateFileName[]  = "policy-state";
constexpr char  kLockFileName[]   = "policy-state.lock";
constexpr u32   kStateKept        = 1U;
constexpr u8    kStateFormat      = 1U;
constexpr usize kStateHeaderBytes = 1U + 8U + 4U;

struct SavedState
{
    u64             floor = 0U;
    cont::Array<u8> generation;
};

[[nodiscard]] fs::Path file_in(cont::StringView directory, cont::StringView name)
{
    return fs::Path(directory) / name;
}

[[nodiscard]] cont::ConstSpan<u8> view(const cont::Array<u8>& bytes) noexcept
{
    return cont::ConstSpan<u8>(bytes.data(), bytes.size());
}

// The saved state of `directory` (empty when there is none); Damaged when files of it exist and none holds a state
// this build reads.
[[nodiscard]] PolicyStateError read_state(cont::StringView directory, SavedState& out, bool& recovered) noexcept
{
    cont::Array<u8>    payload;
    const fs::SaveLoad loaded = fs::load_generation(file_in(directory, kStateFileName), kStateKept, payload);
    recovered                 = loaded.status == fs::SaveStatus::Recovered;
    if (loaded.status == fs::SaveStatus::NotFound)
    {
        return PolicyStateError::None;
    }
    if (loaded.status != fs::SaveStatus::Ok && !recovered)
    {
        return PolicyStateError::Damaged;
    }
    cont::WireReader    reader(view(payload));
    u8                  format = 0U;
    u64                 floor  = 0U;
    u32                 length = 0U;
    cont::ConstSpan<u8> generation;
    if (!reader.take_u8(format) || format != kStateFormat || !reader.take_u64(floor) || !reader.take_u32(length) ||
        length > kMaxPolicyBytes || !reader.take_bytes(length, generation) || !reader.done())
    {
        return PolicyStateError::Damaged;
    }
    out.floor = floor;
    out.generation.resize(generation.size());
    std::copy(generation.begin(), generation.end(), out.generation.data());
    return PolicyStateError::None;
}

[[nodiscard]] PolicyStateError save_state(cont::StringView directory, u64 floor,
                                          cont::ConstSpan<u8> generation) noexcept
{
    cont::Array<u8> payload;
    payload.resize(kStateHeaderBytes + generation.size());
    cont::WireWriter writer(cont::Span<u8>(payload.data(), payload.size()));
    writer.put_u8(kStateFormat);
    writer.put_u64(floor);
    writer.put_u32(static_cast<u32>(generation.size()));
    writer.put_bytes(generation);
    const fs::SaveWrite written = fs::save_generation(file_in(directory, kStateFileName), view(payload), kStateKept);
    return written.ok ? PolicyStateError::None : PolicyStateError::Unsaved;
}
} // namespace

PolicyBootstrap policy_bootstrap() noexcept
{
    return bootstrap::kBootstrap;
}

cont::StringView policy_state_error_name(PolicyStateError error) noexcept
{
    switch (error)
    {
    case PolicyStateError::None:
        return "none";
    case PolicyStateError::NotOpen:
        return "not-open";
    case PolicyStateError::Unavailable:
        return "unavailable";
    case PolicyStateError::Damaged:
        return "damaged";
    case PolicyStateError::Unsaved:
        return "unsaved";
    }
    return "unknown";
}

PolicyInstallation::PolicyInstallation(const PolicyConfig& config) noexcept : m_store(config)
{
}

PolicyOpen PolicyInstallation::open(cont::StringView directory, cont::ConstSpan<u8> bootstrap, u64 now) noexcept
{
    PolicyOpen   opened;
    fs::FileLock lock;
    if (directory.empty() || !fs::create_private_directory(fs::Path(directory)) ||
        !lock.acquire(file_in(directory, kLockFileName)))
    {
        opened.read = PolicyStateError::Unavailable;
        return opened;
    }
    SavedState saved;
    opened.read = read_state(directory, saved, opened.recovered);
    m_store.raise_floor(saved.floor);
    opened.start = m_store.start(view(saved.generation), bootstrap, now);

    // The generation to keep: the bootstrap when it superseded the saved one, the saved one otherwise (also when the
    // store holds none, so a generation expired here stays for an installation whose clock disagrees).
    const cont::ConstSpan<u8> kept = opened.start.bootstrap == PolicyError::None ? bootstrap : view(saved.generation);
    if (opened.recovered || opened.read == PolicyStateError::Damaged || m_store.floor_generation() != saved.floor ||
        !detail::same_bytes(kept, view(saved.generation)))
    {
        opened.saved = save_state(directory, m_store.floor_generation(), kept);
    }
    m_directory = cont::String(directory);
    return opened;
}

PolicyError PolicyInstallation::apply(cont::ConstSpan<u8> generation, u64 now, PolicyStateError& saved) noexcept
{
    saved = PolicyStateError::None;
    if (m_directory.empty())
    {
        saved = PolicyStateError::NotOpen;
        return PolicyError::BadConfig;
    }
    fs::FileLock lock;
    if (!lock.acquire(file_in(m_directory, kLockFileName)))
    {
        // Adopted for this run only: a restart starts from the older save.
        saved = PolicyStateError::Unavailable;
        return m_store.apply(generation, now);
    }
    SavedState latest;
    bool       recovered = false;
    if (read_state(m_directory, latest, recovered) == PolicyStateError::None)
    {
        m_store.raise_floor(latest.floor);
        if (!latest.generation.empty())
        {
            (void)m_store.apply(view(latest.generation), now); // another process's newer generation, if any
        }
    }
    const PolicyError applied = m_store.apply(generation, now);
    if (applied == PolicyError::None)
    {
        saved = save_state(m_directory, m_store.floor_generation(), generation);
    }
    return applied;
}
} // namespace crd::crypto
