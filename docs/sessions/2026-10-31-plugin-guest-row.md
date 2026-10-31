# 2026-10-31 — The plugin guest row: Cerid products shipped as plugins inside other hosts

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## The user's decision (2026-10-31)

<a id="user-decision"></a>

The user sells products built on Cerid and is its first user: games, animation, digital audio workstations, effect
plugins, sample-library players and engineering applications on several platforms. Their example of a first plugin
product is a spectral resonance-suppression effect powered by Cerid and shipped as a plugin. Asked to record the
missing row, they said (verbatim):

> "ok do not forget and write this to the appropriate place in the roadmap file but I will deal with those later! I
> have stuff to do exactly after SIMD-DISPATCH we arranged it"

## What was missing

Every existing plugin row is host-side: [PLG-2](../ROADMAP.md#slice-plg-2) is the host substrate,
[PLG-3](../ROADMAP.md#slice-plg-3) to [PLG-5](../ROADMAP.md#slice-plg-5) host CLAP, VST3 and VST2,
[PLG-6](../ROADMAP.md#slice-plg-6) and [PLG-7](../ROADMAP.md#slice-plg-7) scan and isolate foreign plugins, and
[PLG-8](../ROADMAP.md#slice-plg-8) adds ARA 2, LV2 and AU. No row shipped Cerid itself as a plugin inside another
host, so [PLG-GUEST](../ROADMAP.md#slice-plg-guest) was added directly after PLG-8, in the plugin block, and not near
SIMD-DISPATCH: the order after SIMD-DISPATCH stays as the user arranged it.

## The row's contract

- One guest facade over the PLG-2 parameter, bus and state model, with CLAP, VST3, AU/AUv3 and AAX wrappers.
- A guest process's rules: no global state shared between instances, hidden symbols, no thread or allocator contention
  with the host, runtime SIMD dispatch inside a foreign process, clean load and unload, repeated instantiation.
- The real-time path allocates and locks nothing under host-chosen buffer sizes and sample rates; automation is
  sample-accurate, latency is reported, and state restores across versions.
- The crd-ui editor embeds in the host's window.
- Qualification in at least two hosts per format, plus each format's validator.
- Shipping through [SUPPLY.3](../ROADMAP.md#slice-supply.3) signing and [SHIP-MAC](../ROADMAP.md#slice-ship-mac)
  notarization, with licence activation for sold products. AAX needs the Avid developer agreement and PACE signing:
  those are account steps for the user.

Only documentation changed: the ROADMAP row and this note. Nothing was built or run beyond the documentation validator.
