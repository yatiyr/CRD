#pragma once

// crd-crypto -- an installation's crypto policy state (NET.2; docs/design/network-foundation.md#policy-state).
//
// What a Cerid installation keeps of its policy generations between runs, and what its build brings:
//
//   bootstrap   every build embeds the trust set and the newest signed generation issued when it was built
//               (policy_bootstrap(), generated from assets/crypto-policy by scripts/gen-policy-bootstrap.py; both
//               empty until the key holders' ceremony commits them, and an installation without a trust set refuses
//               every algorithm).
//   saved       the newest generation the installation accepted and its floor, saved as one durable file in the
//               installation's private directory (crd/platform/durable_file.hpp: checked, atomic, one older save
//               kept). Every read and save runs under the directory's lock, so the processes of one installation
//               (an editor and a game) share one floor.
//   start       open() raises the floor to the saved one, then starts the store from the saved generation and the
//               bootstrap (decision S23) and saves what that changed; apply() first catches up with a newer state
//               another process saved, then adopts and saves the new generation.

#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/policy.hpp>

namespace crd::crypto
{
// A build's bootstrap: the trust set (empty or 2 to kMaxTrustKeys keys) and the signed generation it embeds (empty or
// one generation numbered `generation`).
struct PolicyBootstrap
{
    cont::ConstSpan<TrustKey> trust;
    cont::ConstSpan<u8>       generation;
    u64                       number = 0U;
};

// This build's bootstrap.
[[nodiscard]] PolicyBootstrap policy_bootstrap() noexcept;

enum class PolicyStateError : u8
{
    None,
    NotOpen,     // apply() before a successful open()
    Unavailable, // the directory cannot be created or its lock taken
    Damaged,     // the saved state verifies as a file but holds no state this build reads
    Unsaved,     // the state could not be saved: the store holds the change, a restart starts from the older save
};

// "none", "not-open", "unavailable", "damaged", "unsaved".
[[nodiscard]] cont::StringView policy_state_error_name(PolicyStateError error) noexcept;

// What open() did.
struct PolicyOpen
{
    PolicyStateError read      = PolicyStateError::None; // reading the saved state (Damaged: read as none)
    bool             recovered = false; // the newest save failed its checks and the older one was read
    PolicyStart      start;             // the store's answers for the saved generation and the bootstrap
    PolicyStateError saved = PolicyStateError::None; // saving what the start changed
};

// An installation's policy store and its saved state. open() and apply() need the caller's exclusive access;
// store() serves sessions on any thread.
class PolicyInstallation
{
public:
    // `config` as for PolicyStore; its floor_generation is a floor the caller imposes beyond the saved one.
    explicit PolicyInstallation(const PolicyConfig& config) noexcept;

    // Reads the state saved in `directory` (created private when missing) under its lock, raises the floor to the
    // saved one, starts the store from the saved generation and `bootstrap` (S23) and saves when the generation or
    // the floor changed or the newest save was damaged. Unavailable leaves the installation closed and its store
    // empty, so every algorithm is refused.
    [[nodiscard]] PolicyOpen open(cont::StringView directory, cont::ConstSpan<u8> bootstrap, u64 now) noexcept;

    // Adopts one generation received at `now`: under the lock, the floor and the generation another process saved
    // first, then PolicyStore::apply; an adopted generation is saved with the floor (`saved`).
    [[nodiscard]] PolicyError apply(cont::ConstSpan<u8> generation, u64 now, PolicyStateError& saved) noexcept;

    [[nodiscard]] const PolicyStore& store() const noexcept { return m_store; }

private:
    PolicyStore  m_store;
    cont::String m_directory; // empty until open() succeeds
};
} // namespace crd::crypto
