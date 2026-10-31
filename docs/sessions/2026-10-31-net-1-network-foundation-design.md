# 2026-10-31 — NET.1: the network foundation design, and the track C machine

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Track C of the [parallel tracks](2026-10-31-parallel-tracks.md), first batch. Row: [NET.1](../ROADMAP.md#slice-net.1).

## NET.1: review and design

- **Review.** The tree has no networking code: no `crd-net`, and no socket, Winsock or TLS call under `engine/`,
  `runtime/`, `tools/` or `sandbox/`. The capabilities the foundation must reuse were inventoried with their owners:
  the `crd-containers` checksum family (integrity hashes, never authenticators), the `crd-core` fault seams and
  redaction classes, `crd-time`'s `DeterministicClock`, `crd-perf` bundles (DIAG.5d), DIAG.9a replay, `hesap-comms`
  channel models and `crd-platform` I/O. The research's note that CRC-32 had copies in several modules is stale for
  the foundation: HW-RESILIENCE.1's checksum family now owns them.
- **Design.** The [network foundation contract](../design/network-foundation.md): ten assets, nine adversaries and
  seven trust boundaries; a threat catalogue (T01 to T24) with a mitigation and an owning row for each, covering the
  research amendment's items (metadata privacy and ECH, post-quantum handshakes against the amplification limit,
  relays as abuse targets, LEO and mobile paths, IPv6-only networks, compromised or faulty peers sending plausible
  state, the CRA Article 14 duty); the threat weighting of each product class; principals and signed, expiring,
  attenuating, session-bound capabilities; game and project traffic as separate session types; seven channel classes;
  the transport layering; eight bounded admission stages; browser constraints; the cryptographic provider policy;
  version and extension negotiation with GREASE; monotonic, signed crypto policy generations; seven numbered
  decisions; and a hand-off table to the rows that implement each clause.
- **New owner.** Decision D2 gives cryptography its own foundation module, `crd-crypto` (provider interface, key
  store, CSPRNG, keyed PRF), because SUPPLY signing, policy generations and crash packs need it as well as `crd-net`.
  Both modules need [OWN.1](../ROADMAP.md#slice-own.1) registry entries when they land (track B owns OWN).
- [ADR-0035](../decisions/0035-networking-architecture.md) records the design as its 2026-10-31 amendment.
- **Row state.** NET.1 requires [OWN](../ROADMAP.md#slice-own), which track B holds, so the row is Partial: its design
  is complete and it flips to Done when OWN is Done.

## The track C machine

- `cerid-track-c` (spot n2-standard-8 on Persistent Disk, Ubuntu 24.04, GCC 13.3, CMake 4.4.5) has the pinned tools
  under `~/tools` (Ninja 1.11.1, the Vulkan headers and validation layer, sccache), its environment in
  `~/track-env.sh` (tool paths, `VULKAN_SDK`, the validation layer, `SCCACHE_DIR=~/.cache/sccache`,
  `CPM_SOURCE_CACHE=~/.cpm-cache`) and a linux-gcc-debug tree configured with the compiler cache and PCH off
  (`cmake --preset linux-gcc-debug` plus the sccache launcher and `-DCRD_ENABLE_PCH=OFF`). A cold full build of
  2414 steps took about nine minutes with zero warnings. The machine ran for about 0.4 h and was stopped.
- Three defects of `ci/bench_vm.py` stopped the machine's use. `wait`'s job argument shared its destination with
  `--name`, so every wait on a track machine was refused; main fixed that concurrently (3cf62fa0) and this batch kept
  its fix. The other two were fixed here, with tests in `scripts/test-ci-platform.py` run under Windows and Linux
  Python: on Windows `gcloud` is a batch file, so cmd.exe cut a remote command at its first newline, expanded percent
  signs and acted on pipes after an embedded quote (remote text now travels base64-encoded and runs from a private
  file, and a command longer than cmd.exe's line once encoded is refused with an error that names the bound); and a
  detached command held the SSH call open until it ended (it now starts in its own session with
  `setsid -f`).
