# Cerid publishing, packaging and app management: the 2026 state of the art

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists
> only in [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-10-27. **Status:** Recorded research. Every proposed row below is a proposal for the user, not an
accepted change. Nothing in [ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

What do the best engines, packagers, stores, platform holders, standards and laws require and ship in 2026 for building,
exporting, packaging, signing, distributing, updating, selling and operating applications, on every platform Cerid
targets (games from small to MMO, web apps, desktop and mobile applications, engineering simulation, CAD/CAE, DAWs,
cinematics, science and robotics)? How do Cerid's rows compare, and which rows must be added or amended so that an
application built with Cerid ships the gold-standard way, securely, lawfully (with Turkish law as a first-class
jurisdiction) and with every step drivable by agents? The bar is the user's standing order: the proper, complete,
world-class solution, elegant and not bloated, never the "simple" option.

- **Scope against earlier research.** [Part 3](2026-10-26-building-with-cerid-2026-comparison.md) step 14 ("Package
  and ship") named SBOM, provenance, signing, reproducibility and AOT; those became
  [SUPPLY.1](../ROADMAP.md#slice-supply.1) to [SUPPLY.4](../ROADMAP.md#slice-supply.4),
  [SEC.1](../ROADMAP.md#slice-sec.1) and [CEIR-JIT.3](../ROADMAP.md#slice-ceir-jit.3). This document builds on them
  and does not propose them again. The
  [networking research](2026-10-27-networking-and-crash-pipeline-2026-comparison.md) proposes NET-STD
  ([P-1](2026-10-27-networking-and-crash-pipeline-2026-comparison.md#p-1)), a standards register that is not a row yet;
  it is cited here as "(networking proposal)". GPU pipeline caches for shipping builds belong to
  the [PQP-4.renderer](../ROADMAP.md#slice-pqp-4.renderer) amendment and are only linked.
- **Cerid side.** Read, not run: the [ROADMAP](../ROADMAP.md) rows EDITOR.1-EDITOR.7, CR-D007, QUAL.2, SUPPLY.1-4,
  SEC.1, CEIR-JIT.0-3, DX.1-4, EXT.1-5, API-STAB.0, DATA-TRUST.1-2, VCS.1-2, I18N.1, A11Y-GAME, OPS.1-3,
  CRASH-FIX.1-4, NET.5, NETCRASH, 4.2d, NET-SERVER, ANTICHEAT, HW-RESILIENCE.1-4, DOC.2, REFLECT.2, PQP-4.renderer,
  DX12-AGILITY, MAC.1-2, WEB.1-5, JOBS-PORT.2, 6.0-6.4, EMBED.1-2, PLG-8 and PORTFOLIO.1-2 as of 2026-10-27;
  the [renderer/UI execution contract](../design/renderer-ui-execution-contract.md) section "Publishing and application
  acceptance"; [ADR-0038](../decisions/0038-cooked-binary-container-format.md) (the cooked container),
  [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md) (item 3: maintained cryptography
  behind owned interfaces); the user decisions of
  [2026-10-26](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md),
  [2026-10-27 crash to fix](../sessions/2026-10-27-user-decision-crash-to-fix-pipeline.md) and
  [2026-10-27 network foundation](../sessions/2026-10-27-user-decision-network-foundation-window.md); and greps of
  `engine/` and `tools/ceridc` on 2026-10-27. A grep hit is a source fact, not a runtime or quality claim. No build,
  test, benchmark or CI ran for this document.
- **Outside side.** Web searches and fetched pages read on 2026-10-27. Sources are listed at the end as S-numbers (the
  earlier parts use W, B, C and N). **(K)** marks a fact given from prior knowledge and not re-read today; its current
  version or status is uncertain. "Uncertain" means the sources disagree, were secondary only or were older than 2026.
  Every uncertain fact is also collected in [Uncertain facts](#uncertain-facts).
- **Consoles.** PlayStation, Xbox and Nintendo certification requirements are under NDA. Only public information is
  used; everything else needs a licensed developer account, which is a user action
  ([Needs the user](#needs-the-user)).
- **Not legal or tax advice.** Legal and tax facts are researched from law-firm and official summaries so that rows
  can name obligations; every obligation still needs review by qualified Turkish and foreign counsel (a user action).
- **No downloads** were made and no software ran. Comparisons that need one are in
  [Needs a download](#needs-a-download).
- **Order numbers** below are the ROADMAP's first column as of 2026-10-27 (SEC.1 129, DATA-TRUST 409, EDITOR.5 415,
  EDITOR.6 416, EDITOR.7 417, DX.4 435, OPS.1 566, QUAL.2 567, MAC 662, WEB 672, NET-SERVER 901, 6.0 943, 6.1 944,
  6.3 946). Another agent edits the ROADMAP concurrently; placement is by anchor, the number is a hint. The user is
  re-planning the table so that no row stays "Later"; proposals below therefore place every row right after the rows it
  needs, including rows that today read Later.

## The principles every area is judged by

The first six are those of Parts 1-3; (g) and (h) come from ADR-0063, ADR-0130 and the 2026-10-26 hardware decision.

- **(a) Runtime changeability.** Publishing profiles, target descriptors, store metadata, release rules, rollout
  policies, feature flags, live-ops events and compliance rules are authored CHIR/CEIR assets, edited live and applied
  as new generations. Shipped content (CHIR/CEIR behaviour, frame graphs, materials, CKIR kernels) updates without a
  binary update where the platform allows it, and the platform's rules decide where it does not: iOS and consoles forbid
  JIT and restrict downloaded code, so those targets run [CEIR-JIT.3](../ROADMAP.md#slice-ceir-jit.3) AOT objects and
  take only the content updates their store policy permits.
- **(b) crd-hesap is the computational base** for every numeric need (rollout statistics, crash-rate regression tests,
  experiment analysis, delta-encoding cost models).
- **(c) CHIR is the Cerid language** for publishing scripts, release rules and agent actions; there is no second
  scripting or pipeline language (no YAML-only build graph, no Ruby lanes).
- **(d) One owner per capability.** Packaging, signing orchestration, update metadata, store adapters, entitlements and
  the compliance register each have exactly one owning module ([OWN.1](../ROADMAP.md#slice-own.1)); every row below
  names what it consumes and what it does not reimplement.
- **(e) Wide hardware.** The SSE2 x64 floor, older GPUs and Windows/Linux first; packages carry every dispatch level
  and GPU tier they support with fallbacks, and installers run on the oldest supported OS of each target.
- **(f) Budgets from peers.** Beat the best peer (Steam and Epic patch sizes, Unreal IoStore/Zen load times, Unity
  Addressables build times, MSIX/Velopack update latency, App Store and Play review turnaround preparation); parity
  only with recorded evidence.
- **(g) Determinism and provenance.** A release is reproducible bit for bit from its inputs, carries an SBOM and
  provenance, and every artifact a user installs is verifiable back to a signed source revision.
- **(h) Graceful on faulty hardware and hostile networks.** A torn download, a bit flip in a patch, a full disk, a power
  cut during an update or a malicious mirror yields a verified retry, a rollback to the last good install or a clear
  message, never a bricked installation or silent corruption ([HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1)).

<a id="highest-impact-gaps"></a>
## Executive summary: the most important gaps

Publishing is almost entirely plan today. The plan has strong pieces (SBOM, SLSA provenance, signing, reproducible
builds, AOT for JIT-forbidden targets, a package format for plugins, a crash pool and consented telemetry), but no row
owns the path from a finished project to an installed, updatable, sold and supported product on a real store. Ranked by
how much later work each gap blocks:

| Rank | Gap | Cerid today | Proposal |
|---:|---|---|---|
| 1 | No publishing architecture, owner or release data model; export is one sentence in EDITOR.6 | EDITOR.6/.7 one-liners | SHIP.0, SHIP.1 (new) |
| 2 | No secure update and patch system: signed update metadata, deltas, channels, staged rollout, kill switch, rollback | Missing | SHIP.3 (new) |
| 3 | No versioned register of platform and legal obligations, though many changed in 2026 | Missing | COMPLY.1 (new) |
| 4 | Signing plan predates 2026: 460-day certificates, Artifact Signing excludes a Turkish entity, SmartScreen per file | SUPPLY.3 intent | SUPPLY.3 (amend) |
| 5 | crdr v1 has no per-chunk integrity, signed manifest, mounting, delta-friendly layout or streaming install | crdr v1 | SHIP.2 (new) |
| 6 | No matrix of what content may hot-update on which platform (iOS 2.5.2, Play code policy, consoles) | Principle only | SHIP.0, CEIR-JIT.3 (amend) |
| 7 | No Windows or Linux installers and package formats (MSIX, MSI, winget, Store; Flatpak, AppImage, deb/rpm, SLR) | Missing | SHIP.4, SHIP.5 (new) |
| 8 | No store integration; Steamworks would get two owners (4.2d and a store row) | 4.2d names `crd-net-steam` | SHIP.6 (new), 4.2d (amend) |
| 9 | No entitlements, licence server (node-locked, floating, offline, air-gapped) or lawful payment flows | EXT.5 "paid listings" only | SHIP.7 (new) |
| 10 | Store compliance artifacts (ratings, privacy labels and manifests, data safety, KVKK notices, export class, OSS notices) | Missing | COMPLY.2 (new) |
| 11 | Monetisation law: Brazil loot-box ban in force, EU CPC principles, Turkey parental payment approval from 2026-11-01 | Missing | COMPLY.2, SHIP.7 |
| 12 | No publisher console or release automation with human approval gates for irreversible actions | Missing | SHIP.8 (new), AGENT.3 (amend) |
| 13 | Mods and UGC: no distribution, signing, load order or moderation (DSA, UK OSA, Turkey 5651) | EXT.4 packages | SHIP.9 (new) |
| 14 | No one-command publish, store validators, CI templates or clean-machine and device matrices | DX.1 `package` verb | SHIP.10, SHIP.11 (new) |
| 15 | Later platforms have no shipping rows: notarized macOS (Intel ends), iOS/Android stores and EU/Japan rules, consoles, XR, web, enterprise | MAC, WEB, 6.0, 6.1, 6.3 | SHIP-MAC, SHIP-WEB, SHIP-MOBILE, SHIP-CONSOLE, SHIP-XR, SHIP-ENTERPRISE (new) |

Next in impact: client and server version skew windows tied to release channels (NET-SERVER amendment), save-game and
project migration across released versions (DOC.2 amendment), symbol upload and per-release crash regression signals
(CRASH-FIX.2 amendment), ARM64 packages (JOBS-PORT.2 amendment), and a boundary between anti-tamper and anti-cheat.

**Dated facts that already bind a Cerid product in 2026** (details in the areas): EU CRA vulnerability and incident
reporting through ENISA's Single Reporting Platform since 2026-09-11 [S49](#s49); EU AI Act Article 50 transparency
since 2026-08-02 [S47](#s47); Google Play target API 36 for new apps and updates since 2026-08-31 [S05](#s05); Xcode 26
SDKs for App Store uploads since 2026-04-28 [S12](#s12); 16 KB page sizes for Play apps with native code since
2025-11-01 [S06](#s06); Android developer verification in four countries since 2026-09-30 [S07](#s07); code-signing
certificates capped at 460 days since 2026-03-01 [S02](#s02); Brazil's ECA Digital (paid loot boxes for minors) since
2026-03-17 [S43](#s43); Turkey's Internet Law (5651) game-platform duties from 2026-11-01 [S41](#s41); the EU Product
Liability Directive for software placed on the market from 2026-12-09 [S46](#s46).

<a id="cerid-today"></a>
## Cerid today: what publishing builds on

Stated once so the comparisons are read fairly.

- **Cooked container.** `crd-resources` has the crdr chunk container (ADR-0038; `crdr.hpp`: magic `CRDR`, version 1,
  per-chunk zstd compression with a bounded decompression claim and exact-size check). It has no per-chunk checksum
  (HW-RESILIENCE.1 adds one), no signed manifest, no mount or overlay order, no content-defined chunk layout and no
  streaming install priority. `zip_archive.hpp` and `deflate.hpp` exist for interchange.
- **Cookers and the CLI.** `engine/assets/*-cook` modules and `tools/asset_cooker`, `tools/shader-cook`; `ceridc` verbs
  are import, cook, query, instantiate, sequence, render, export_timeline, inspect and diag. There is no package,
  sign, publish or upload verb.
- **Modular exclusion exists at build level.** [REPO.DEV.4](../ROADMAP.md#slice-repo.dev.4) (Done): `CRD_MODULES`
  configures a dependency-closed module subset. This is the base for "ship only used modules"; no export uses it yet.
- **Pinned inputs and offline builds.** [REPO.DEV.6](../ROADMAP.md#slice-repo.dev.6) (Done): `cmake/pins.json` with
  SHA-256 and licence per input and `CRD_INPUT_ARCHIVES` for verified offline copies: the base for SBOMs, licence
  notices and air-gapped release builds.
- **Diagnostics.** DIAG crash capture and versioned bundles with exact symbol identities, plus the planned
  reproduction pack ([CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1)) and crash pool
  ([CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2)) in the NETCRASH window: the base for release health gates.
- **Rows that already name publishing.** EDITOR.6 (presets, module selection, cooked closure, Windows/Linux export),
  EDITOR.7 (clean-machine export, CLI/MCP), the execution contract ("a resource archive alone does not satisfy
  'publish a game'"), SUPPLY.1-4, SEC.1, CEIR-JIT.3, EXT.4 (package format, signature and provenance verification
  before load, live install and rollback), EXT.5 (marketplace, Later), DX.1 (`ceridc package`), DATA-TRUST.2
  (artifact identity versus publisher trust), DX12-AGILITY (Agility SDK redistribution), QUAL.2 (clean-machine release
  evidence), OPS.1 (consented telemetry) and OPS.2 (clean install/export/recovery matrices).

<a id="area-1-build-export"></a>
## 1. Build and export pipeline

### State of the art 2026

- **Unreal.** BuildGraph scripts drive UAT's BuildCookRun; Horde schedules builds and tests and runs the Unreal Build
  Accelerator; Zen Storage Server is the cooked output store for projects with over a million cooked files and the path
  to reliable incremental cooking, and cooked data snapshots let a farm-cooked result be imported locally
  [S33](#s33). 5.7 users report intermittent Zen oplog failures in staging and slower Horde cooks, with snapshot fixes
  in 5.8 [S33](#s33) (forum reports, uncertain). IoStore containers and the Zen loader, chunked by priority, are the
  shipping format (K).
- **Unity.** Build Profiles (Unity 6) hold per-platform settings and scene lists as assets; Addressables with content
  catalogs and remote groups deliver content updates; Build Automation runs in the cloud (K).
- **Godot.** Export templates per platform, presets, PCK files; PCK encryption needs custom-compiled templates with a
  256-bit AES key, and on Android the PCK is not encrypted unless APK expansion is used [S34](#s34).
- **Application frameworks.** Electron Forge and electron-builder (installers, auto-update, notarization hooks), Tauri 2
  (bundler, signed updater manifests), Flutter (per-platform build and store artifacts), Qt Installer Framework
  (online installers with component repositories) (K).
- **Pipeline caches.** Driver pipeline binaries and precompiled shader delivery are the 2025-2026 answer to first-use
  stutter; Cerid's owner is the [PQP-4.renderer](../ROADMAP.md#slice-pqp-4.renderer) amendment.
- **Supply chain.** SLSA 1.2 Build L3 provenance and CycloneDX 1.7 / SPDX 3 SBOMs are already Cerid rows
  ([C26](2026-10-26-building-with-cerid-2026-comparison.md#c26),
  [C27](2026-10-26-building-with-cerid-2026-comparison.md#c27)).
  Sigstore's Rekor v2 reached general availability on 2025-10-10 as a tile-backed log; Cosign 2.6+ and 3.x sign
  and verify against it [S32](#s32).
- **Distributed and cached builds.** Content-addressed caches (Zen, Unity Accelerator, Bazel remote cache, sccache) and
  farm schedulers (Horde, Incredibuild, FASTBuild) (K); Cerid already uses a pinned sccache locally.

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Target profiles as assets | Unity Build Profiles; Unreal platform ini and BuildGraph (K) | Missing: EDITOR.6 names "presets" without a model |
| Per-target cook (formats per GPU tier and platform) | Unreal per-platform cook; Unity per-platform import (K) | Partial: cookers exist; no target descriptor drives them |
| Reachability stripping of content | Unreal cook from maps; Godot export filters | Planned: "cooked asset closure" in EDITOR.6 |
| Module exclusion | Unreal plugin enable lists; Godot custom templates | Has at build level (`CRD_MODULES`); not wired to export |
| Shader precompile, pipeline caches | Unreal PSO bundles; driver pipeline binaries | Planned: PQP-4.renderer amendment |
| AOT vs interpreter vs JIT per target | Unity IL2CPP; Unreal Verse VM (K) | Planned: CEIR-JIT.3 (AOT); no per-target selection rule |
| Deterministic packaging and provenance | SLSA L3, reproducible-builds practice | Planned: SUPPLY.1-4 (binaries); packages and content not named |
| Shared cook cache and farm | Zen, Horde, UBA [S33](#s33) | Partial: local sccache for code; VCS.2 names cook-cache keys; OPS.3 owns distributed jobs |
| Symbols out of the package, into a symbol store | Every shipping engine (K) | Missing as an export step; CRASH-FIX.2 names symbol upload |

### Gaps in area 1

- **No owner for the export pipeline.** EDITOR.6 is an editor row; the pipeline must exist headless first (CI, agents,
  `ceridc`), with the editor as one client. A publishing service module (proposed `crd-ship`, registered in OWN.1)
  owns profiles, target descriptors, cook orchestration, package assembly, signing orchestration and store adapters;
  cookers, `crd-resources`, SUPPLY rows and CRASH-FIX keep their own capabilities.
- **Target descriptors.** A target is platform, architecture set, ISA dispatch levels, GPU tiers, OS floor, store,
  channel, content set, languages, rating set and signing identity reference. Profiles are CHIR assets that compose
  targets (a "steam-release" profile is Windows x64 plus Linux x64 plus Steam depots plus a stable channel).
- **Content and module closure.** Reachability from entry scenes, programs and commands, with explicit keeps; editor
  modules, cookers and debug commands (SCRIPT.4 shipping allowlists) are excluded; the closure report is an artifact.
- **Determinism beyond binaries.** SUPPLY.4 covers compilers; packages (MSIX, Flatpak, crdr packs, installers) need the
  same double-build check: sorted entries, normalised timestamps from `SOURCE_DATE_EPOCH`, stable compression settings.
- **Principles.** (a) Profiles edit live and re-plan only affected steps. (d) The pipeline consumes cookers, SUPPLY,
  PQP-4 caches, CEIR-JIT.3 and CRASH-FIX symbol upload; it reimplements none of them. (f) Peers: Unreal BuildCookRun
  with Zen and Unity Build Profiles for clean and incremental export time.

<a id="area-2-platforms-stores"></a>
## 2. Platforms and stores

### State of the art 2026

- **Windows.** MSIX with App Installer auto-update, MSI for enterprise and Group Policy, signed bootstrapper EXEs,
  winget manifests (K). The Microsoft Store accepts Win32 (including unpackaged), UWP, PWA, .NET MAUI and Electron apps;
  individual registration has been free since 2025-09-10 in nearly 200 markets, Store-hosted MSIX is signed by
  Microsoft and updated automatically [S24](#s24); company accounts' fee is reported both as unchanged and as waived
  under a FastTrack preview (uncertain) [S24](#s24). Windows 10 support ended on 2025-10-14 (K). The GDK is free on
  GitHub as a binary for PC learning; console extensions and Xbox publishing need a licence [S25](#s25).
- **Steam.** SteamPipe depots and branches, builds uploaded through SteamCMD and the ContentBuilder, partial depot
  updates, a local content server for iteration [S21](#s21). Deck Verified checks input, display, seamlessness and
  system support under Proton [S22](#s22) (2021-2023 sources, uncertain in detail). Valve extends Verified to the Steam
  Machine and Steam Frame (summer 2026 launch window), with Frame checks for built-in display legibility and default
  controls; Deck-verified games carry over [S20](#s20). Steam Linux Runtime 4.0 (November 2025) moves to Debian 13 with
  ABI-incompatible libraries (libFLAC, OpenSSL SONAMEs), sdl2-compat replacing SDL 2 and GTK and Qt removed
  [S23](#s23).
- **Epic Games Store.** 0% revenue share on the first USD 1 million per app per year since June 2025, 88/12 after,
  and Epic-hosted webshops for out-of-app purchases [S19](#s19). **GOG** (Galaxy SDK, DRM-free offline installers) and
  **itch.io** (butler uploads, delta patching) (K).
- **macOS.** Developer ID signing, hardened runtime, notarization with `notarytool` and stapling, Mac App Store
  sandbox entitlements (K). macOS Tahoe 26 is the last release for Intel Macs, which get security updates for three
  more years; full Rosetta 2 lasts through macOS 27 and only a gaming subset remains after [S04](#s04).
- **Linux.** Flatpak and Flathub (payments and verified-developer work were in progress in 2023; 2026 status
  uncertain [S57](#s57)), Snap, AppImage, deb/rpm repositories, Steam Runtime containers (pressure-vessel) (K).
- **iOS, iPadOS, visionOS.** Uploads need Xcode 26 and the 26 SDKs since 2026-04-28 [S12](#s12); privacy manifests and
  required-reason APIs with signed third-party SDKs since 2024-05-01 [S16](#s16); new age-rating tiers 13+, 16+ and 18+
  with a mandatory questionnaire (deadline 2026-01-31) [S13](#s13); TestFlight (K). EU: alternative marketplaces and
  web distribution; revised terms announced 2026-08-18 for 2026-10-01 replace the per-install Core Technology Fee with a
  5% Core Technology Commission and cut App Store rates (26% standard, 15% programmes) [S08](#s08) (secondary,
  uncertain in detail). Japan's Mobile Software Competition Act applies since 2025-12-18 (alternative stores,
  notarization of apps outside the App Store, alternative payments) [S10](#s10).
- **Android.** App Bundles and Play App Signing, Play Asset Delivery, Play Integrity (K); target API 36 for new apps and
  updates since 2026-08-31 (extension to 2026-11-01 on request; XR and TV bar API 34) [S05](#s05); 16 KB page support
  for native code since 2025-11-01 [S06](#s06); developer verification for apps installed on certified devices in
  Brazil, Indonesia, Singapore and Thailand since 2026-09-30, global in 2027, with ADB and an "advanced flow" for
  unverified apps [S07](#s07).
- **Consoles (public only).** Xbox: ID@Xbox, GDK with licensed GDKX extensions, Xbox Requirements (XR) (K) [S25](#s25).
  PlayStation: PlayStation Partners, TRC (K). Nintendo: a merged developer portal with an access request per
  platform; Switch 2 launched June 2025 (K) [S26](#s26). Everything else is NDA.
- **XR.** Meta Horizon Store merged App Lab into one store with Virtual Reality Check guidelines [S27](#s27); SteamVR
  and Steam Frame Verified [S20](#s20); visionOS through the App Store; Android XR through Play with an API 34 bar
  [S05](#s05).
- **Web.** PWA install, service workers, immutable hashed assets on a CDN, isolation headers; Cerid's owner is WEB.1-5.
- **Enterprise.** Intune (Win32 and MSIX), MSI with Group Policy and ADMX, Configuration Manager, Jamf and MDM
  configuration profiles on macOS, offline installers, air-gapped update bundles (K).
- **Embedded, kiosk, automotive.** A/B partitions, Uptane-class secure OTA for vehicles, Windows Assigned Access and
  Linux kiosk sessions (K).
- **Servers.** OCI images (distroless, signed with Cosign, SBOM attached), Agones fleets on Kubernetes (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Windows installers and Store | MSIX/App Installer, MSI, winget, Store Win32 [S24](#s24) | Missing |
| Steam depots, branches, Verified | SteamPipe, SteamCMD, Deck/Machine/Frame Verified [S20](#s20) [S21](#s21) | Missing; 4.2d names a `crd-net-steam` network adapter only |
| EGS, GOG, itch.io | EOS BuildPatch, Galaxy, butler (K) | Missing |
| Linux packages and SLR | Flatpak, AppImage, deb/rpm, SLR 3/4 [S23](#s23) | Missing |
| macOS notarized apps | Developer ID, notarytool (K) | Missing; MAC.1 is the runtime and toolchain |
| iOS/Android store builds | Xcode 26 SDK, AAB, API 36, 16 KB [S05](#s05) [S06](#s06) [S12](#s12) | Missing; 6.1 is runtime qualification |
| EU DMA and Japan MSCA distribution | Alternative marketplaces, web distribution [S08](#s08) [S10](#s10) | Missing |
| Consoles | Licensed SDK submission flows | 6.0 platform packs, no submission row |
| XR stores | Horizon Store VRC, Steam Frame, Android XR [S27](#s27) | 6.3 runtime only |
| Enterprise deployment | Intune, GPO/ADMX, Jamf (K) | Missing |
| Server images | OCI, Cosign, Agones (K) | NET-SERVER names fleets, not images |

### Gaps in area 2

- **Every target needs a packaging row, a store adapter and a qualification on the real store.** Windows and Linux
  first (the confirmed release order), then macOS after MAC, web after WEB, mobile after 6.1, consoles after 6.0, XR
  after 6.3, enterprise after macOS.
- **One Steamworks owner.** The Steamworks SDK is one library for depots and DRM, overlay, achievements, cloud saves,
  Workshop, input and the lobby/P2P networking 4.2d needs; the binding is owned once (SHIP.6) and 4.2d's
  `crd-net-steam` consumes it. The same rule holds for EOS (store plus networking) and the console SDKs.
- **Linux specifics.** SteamOS and Fedora Atomic are immutable; Flatpak is the native desktop path there. SLR 4.0's
  ABI breaks matter to engines that link system libraries; Cerid owns its stack, so its Linux build can target SLR 3
  and 4 with only libc, Vulkan and audio/display ABIs from the host: an advantage worth proving.
- **Intel Macs end.** A universal2 build is needed while Tahoe 26 Intel machines get security updates (about three
  years from 2025); arm64-only after. This is a SHIP-MAC policy with dates in COMPLY.1.
- **Android developer verification** turns sideloaded and alternative-store Android distribution into an identity
  obligation for the team (user action) from 2027 globally.
- **Principles.** (a) Store metadata (descriptions, screenshot lists, rating answers, prices) is an authored, localized
  asset applied by store adapters. (e) Each package declares its OS floor and is tested on it. (f) Peers: Steam patch
  size and install time against Unreal and Unity titles of similar size.

<a id="area-3-signing-trust"></a>
## 3. Code signing and trust

### State of the art 2026

- **CA/B Forum.** Ballot CSC-31 (Code Signing Baseline Requirements 3.10.0, adopted 2025-11-17) caps publicly trusted
  code-signing certificates at 460 days for certificates issued from 2026-03-01 [S02](#s02). Private keys have had to
  live in hardware (FIPS 140-2 level 2 or Common Criteria EAL 4+ tokens or HSMs, including cloud HSM services) since
  2023-06-01 (K).
- **Azure Artifact Signing** (formerly Trusted Signing) reached general availability in January 2026; eligibility is
  organisations or individuals in the USA and Canada and organisations in the EU or UK; USD 9.99 per month for 5,000
  signatures, USD 99.99 for 100,000 [S01](#s01). A Turkish company or individual is not in that list (uncertain:
  Microsoft says new regions will follow).
- **SmartScreen.** EV certificates no longer grant immediate reputation; reputation accrues per file hash, so every new
  build starts at zero; Store-signed apps carry full reputation; Smart App Control on Windows 11 blocks unsigned files
  without reputation [S03](#s03). Reputation may not carry over to a renewed certificate; dual-signing with the old and
  new certificate is the reported mitigation [S03](#s03) (secondary).
- **Apple.** Developer ID certificates, notarization, hardened runtime; App Store distribution re-signs (K).
  **Android.** Upload key plus Play App Signing; APK Signature Scheme v3.1 key rotation; v4 for incremental install (K).
- **Linux.** Signed apt and dnf repository metadata, Flatpak OSTree commit signatures, Sigstore for artifacts (K).
- **Timestamping and transparency.** RFC 3161 timestamps keep signatures valid after certificate expiry (K); Rekor v2
  is a tile-backed transparency log [S32](#s32).
- **Updates.** The Update Framework (TUF) roles, thresholds, expiry and rollback and freeze protection, and Uptane for
  vehicles, are the reference designs for signed update metadata (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| HSM-held release keys | CA/B BR hardware rule (K) | Planned: SUPPLY.3 "HSM-held key" |
| Certificate lifecycle under the 460-day cap | CSC-31 [S02](#s02) | Missing: no rotation or renewal automation |
| Cloud signing service eligible for the team | Artifact Signing (US/CA/EU/UK only) [S01](#s01) | Missing; eligibility not recorded |
| SmartScreen reputation plan | Per-hash reputation [S03](#s03) | Missing |
| Apple/Android/Linux signing | Developer ID, Play App Signing, repo keys (K) | Planned: SUPPLY.3 names Apple and Linux; Android missing |
| Signed update metadata | TUF/Uptane (K) | Missing |
| Key compromise response | CA revocation, re-sign, rotate (K) | Missing |
| Transparency log | Rekor v2 [S32](#s32) | Planned: SUPPLY.3 for plugin packages |

### Gaps in area 3

- **SUPPLY.3 needs the 2026 facts:** a renewal and rotation cadence under 460 days, automated; an eligible signing
  path for a Turkish entity (an OV certificate from a public CA with a cloud HSM or hardware token, or a key in a
  managed HSM that the CA supports; Artifact Signing only through an EU, UK, US or Canadian entity); RFC 3161
  timestamps on every signature; dual-signing at renewal; SmartScreen reputation built through Store distribution and
  steady release cadence; Android upload key and Play App Signing with v3.1 rotation.
- **Signing is a service, not a CI secret.** Release keys live in an HSM behind a signing service that CI reaches with
  short-lived OIDC credentials; policy (which commit, which provenance, which approver) is checked before a signature
  is made; every signature is logged.
- **Update trust is missing** (owned by SHIP.3): TUF-class root, targets, snapshot and timestamp roles with threshold
  keys, expiry and rollback protection, covering binaries, content packs, mods and plugin packages (EXT.4 shares it).
- **Key compromise playbook** (SEC.1): revoke, rotate the TUF root, re-sign, push a forced update, tell users, file CRA
  and Turkish reports where they apply.
- **Principles.** (d) Cryptography stays behind owned interfaces with maintained implementations (ADR-0130 item 3).
  (g) A signature binds an artifact to its provenance, not just to a key.

<a id="area-4-updates"></a>
## 4. Updates, patching and live management

### State of the art 2026

- **Auto-updaters.** Sparkle 2 (EdDSA-signed appcasts, sandboxed installer) on macOS; Velopack, which builds installers,
  updates, delta packages and portable packages in one command and migrates Squirrel apps [S30](#s30) (it warned of a
  delta-application bug fixed in 0.0.320); MSIX App Installer; Omaha (Google Update) (K); store-managed updates on
  Steam, Epic, the Microsoft Store and mobile stores.
- **Binary deltas.** bsdiff; Courgette disassembles and adjusts pointers before diffing to make Chrome patches far
  smaller [S31](#s31); Chromium's Zucchini is its successor (K, not confirmed today); zstd `--patch-from`; content-
  defined chunking (FastCDC, casync/desync); Steam and Epic chunk systems (Steam about 1 MiB chunks, K).
- **Channels and rollout.** App Store phased release over seven days and Google Play staged rollouts with halt (K);
  Steam beta branches (unlimited public and private) [S21](#s21); percentage rollouts, automatic halt on crash-rate
  regressions, kill switches and remote config (Firebase Remote Config, LaunchDarkly, OpenFeature as the CNCF
  vendor-neutral flag API) (K).
- **Content updates without a binary.** Unity Addressables remote catalogs; Unreal chunk downloads (K). On iOS,
  guideline 2.5.2 bars downloading code that introduces or changes features; enforcement in 2026 has been inconsistent
  (a vibe-coding app removed, a similar one approved the same week) [S17](#s17); the Developer Program licence allows
  downloaded interpreted code that does not change the app's primary purpose, create a store for other code or bypass
  signing and the sandbox [S17](#s17) (quoted secondhand, uncertain wording). Google Play bars downloading executable
  code (dex, JAR, .so) from outside Play and self-updating outside Play; interpreted code with only indirect API access
  is outside that rule but may not enable policy violations [S18](#s18).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Owned, signed updater | Sparkle 2, Velopack, Omaha (K) [S30](#s30) | Missing |
| Delta patches | Courgette/Zucchini, chunk systems [S31](#s31) | Missing |
| Channels, staged rollout, halt, kill switch | Play staged rollouts, Steam branches (K) [S21](#s21) | Missing |
| Rollback to last good install | A/B slots (Chrome OS, Android A/B) (K) | Missing for installs; RAH-7/EXT.4 have last-good generations for live content |
| Live content hot-fix | Addressables remote catalogs (K) | Strong in principle (CEIR live swap); no platform policy matrix |
| Version skew client/server | Live-service practice (K) | NET.5 names version migration; no channel binding |
| Save and project migration | Schema evolution (REFLECT.2) | Planned: DOC.2/REFLECT.2 N-1, N-2 readers |

### Gaps in area 4

- **SHIP.3 owns the updater** for stores that do not update for the app (direct downloads, enterprise, web shells):
  TUF-class metadata; chunk-level and binary deltas with a measured owned implementation and oracles; resumable,
  rate-limited background downloads; install into a new slot and switch atomically; rollback; repair ("verify
  integrity"); offline bundles. Where a store updates the app, the owned updater is disabled by the target
  descriptor (Play and the App Store forbid self-update).
- **Live content needs a changeability matrix** per platform and store: which asset kinds (data, CHIR/CEIR content run
  by the interpreter, CKIR kernels, native code) may update over the air, under which review rule, with which engine
  ABI range. iOS and consoles get AOT binaries and data-plus-interpreted content within policy; Play forbids downloaded
  native code; desktops allow everything signed. SHIP.0 decides it, COMPLY.1 records the rules, CEIR-JIT.3 compiles
  the AOT side.
- **Release health drives rollout.** Staged percentages advance only when CRASH-FIX.2's per-release crash-free rate
  and OPS.1's consented performance signals pass declared gates; a regression halts the rollout and can roll back.
- **Version skew.** Every release declares compatible server and content ranges; NET-SERVER fleets keep an overlap
  window; content packs declare engine ABI ranges (API-STAB.0).
- **Principles.** (a) Rollout rules, flags and kill switches are authored assets with their own signed generations.
  (b) Rollout statistics (sequential tests on crash-free rates) use `hesap-stats`. (h) A power cut mid-update leaves
  the old slot runnable.

<a id="area-5-packaging-content"></a>
## 5. Packaging formats and content

### State of the art 2026

- **Containers.** Unreal IoStore (`.utoc`/`.ucas`) with Zen; Unity bundles and Addressables; Godot PCK (K).
- **Compression.** zstd (with dictionaries and long-range mode); Oodle (Kraken, Mermaid, Selkie, Leviathan), owned by
  Epic, free in Unreal and still licensed to non-Unreal developers on terms not published [S29](#s29); DirectStorage 1.4
  (public preview, GDC 2026, 2026-03-11) adds Zstandard beside GDeflate with CPU and GPU decompression, an open-source
  GPU Zstd shader initially tuned for chunks of 256 KiB or less, and the Game Asset Conditioning Library [S28](#s28).
- **Encryption and anti-tamper.** Pak encryption keys are always on the client, so encryption deters casual extraction
  only (K); Denuvo-class anti-tamper raises cracking cost for launch windows at the price of performance complaints,
  online activation and preservation problems (K, contested). GOG sells DRM-free; Steam DRM wrapper is optional (K).
- **Streaming install.** Play Asset Delivery install-time, fast-follow and on-demand packs; console "play while
  installing" chunks; Steam and Epic chunk priority (K).
- **Integrity.** Per-chunk hashes in manifests (Steam "verify integrity of game files"), Merkle trees for partial
  verification (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Chunked cooked container | IoStore, PCK (K) | Has: crdr v1 with per-chunk zstd |
| Per-chunk integrity and signed manifest | Merkle manifests (K) | Missing; checksum planned in HW-RESILIENCE.1 |
| GPU decompression path | DirectStorage 1.4 Zstd and GDeflate [S28](#s28) | Missing |
| Mount layers (base, patch, DLC, mods) | Unreal pak priority, Addressables catalogs (K) | Missing |
| Delta-friendly layout | Chunk systems, content-defined chunking (K) | Missing |
| Streaming and on-demand install | PAD, console chunks (K) | Missing (VGE-1 streams pages at runtime, not installs) |
| Install size budget | Peer titles (K) | Missing |

### Gaps in area 5

- **crdr v2** (owner `crd-resources`; ADR-0038 amended): per-chunk checksums from HW-RESILIENCE.1's family, a Merkle
  root in a signed manifest, chunk alignment for unbuffered and DirectStorage I/O, content-defined chunk boundaries for
  large payloads so a small edit patches small, mount layers with explicit priority (base, patch, DLC, language, mod),
  per-chunk codec choice (zstd with trained dictionaries as the owned default; GDeflate and DirectStorage Zstd GPU
  paths where present, CPU fallback always), install priority classes for streaming install, and an install-size board.
  OWN.1 decides whether compression kernels belong to `crd-resources` or a foundation codec module.
- **Honest protection.** Optional per-chunk AES-GCM encryption with keys delivered by entitlements (SHIP.7) is a
  deterrent and a licence-enforcement aid, not security against a determined user; no kernel components; third-party
  anti-tamper only as an optional adapter decided per product, with its costs recorded. Anti-cheat stays in ANTICHEAT.
- **Principles.** (e) GPU decompression has a CPU path at the SSE2 floor. (f) Peers: IoStore/Zen load time, Steam patch
  bytes for a one-asset change, DirectStorage throughput. (h) A corrupt chunk is re-fetched or repaired, never loaded.

<a id="area-6-monetisation"></a>
## 6. Monetisation and entitlements

### State of the art 2026

- **Store IAP.** StoreKit 2 and Google Play Billing; Steam microtransactions and DLC; Microsoft Store add-ons; EOS
  ecommerce (K). Server-side receipt validation is the norm (K).
- **Alternative payments and links.** EU: App Store rates from 2026-10-01 of 26% (15% in programmes), 20%/10% with
  alternative processing, 15%/10% for link-outs, and 5% Core Technology Commission for marketplace and web
  distribution [S08](#s08) (uncertain detail). US: the Ninth Circuit upheld Apple's contempt finding over the
  anti-steering injunction but sent the total commission ban back for a narrower remedy limited to costs genuinely
  necessary [S09](#s09); Epic and Google proposed a settlement in November 2025 that Judge Donato questioned in January
  and April 2026, with a summer 2026 evidentiary hearing [S11](#s11) (outcome not found, uncertain). Japan: alternative
  in-app payments with Apple's option shown, as low as 5% commission in alternative marketplaces [S10](#s10). Korea:
  the 2021 Telecommunications Business Act amendment bars forced in-app payment; 2023 proposed fines of KRW 68 billion
  [S55](#s55) (2025-2026 status uncertain). Turkey: the Competition Authority opened probes into Apple's App Store
  payment rules (2024-06-06) and Google Play Billing (2025-08-22) without a final decision found [S42](#s42).
- **PC stores.** Epic 0% on the first USD 1 million per app per year and Epic webshops [S19](#s19); Steam 30/25/20
  tiers (K).
- **Engineering software licensing.** FlexNet (Revenera), RLM, LicenseSpring, Cryptlex, Keygen, Cryptolens: node-locked,
  floating through a vendor-hosted or on-premises licence server, borrowing, offline activation, usage reporting
  [S56](#s56).
- **Merchant of record and tax.** Paddle, FastSpring and Stripe-owned Lemon Squeezy collect and remit VAT/GST (K).
  EU VAT OSS for B2C digital services (K).
- **Turkey.** A non-resident seller of electronic services to Turkish consumers registers under the special VAT regime
  (since 2018, no threshold, 20%, monthly VAT-3 returns; deadline dates differ by source) [S36](#s36). A Turkish
  company's domestic sales carry ordinary 20% KDV with e-invoice or e-archive documents (K). The digital services tax
  falls to 5% from 2026-01-01 and 2.5% from 2027-01-01 (Presidential Decision 10767), with thresholds of TRY 20 million
  Turkish revenue and EUR 750 million global revenue [S35](#s35), so it binds stores and large platforms, not a small
  studio. Distance sales: 14-day withdrawal, with an exception for instantly delivered digital content only when the
  conditions and disclosures are met; the 2025 amendment strengthens consumer rights [S39](#s39).
- **Fraud.** Chargeback abuse, key reselling, stolen-card purchases of gift cards and keys (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Store entitlement and DLC checks | Steamworks, EOS, StoreKit 2, Play Billing (K) | Missing |
| Virtual currency and IAP flows | Store SDKs; CPC principles [S44](#s44) | Missing |
| Licence keys and licence server | FlexNet, LicenseSpring, Cryptolens [S56](#s56) | Missing; EXT.5 names "licence metadata" for listings |
| Offline and air-gapped activation | Offline floating servers [S56](#s56) | Missing |
| Merchant-of-record and tax adapters | Paddle, FastSpring (K) | Missing |
| Turkish payments and invoicing | Local PSPs, e-archive (K) | Missing |

### Gaps in area 6

- **SHIP.7 owns entitlements** behind one interface: store entitlements, signed licence files, a floating licence server
  that runs on premises and air-gapped (signed request and response files), borrowing with expiry, revocation,
  clock-tamper detection and grace periods; engineering products (CAD/CAE, DAWs, science) need it first-class.
- **Payments are adapters, never stored card data:** store billing, merchant of record, and Turkish payment service
  providers with e-archive invoicing through an integrator for a Turkish seller (user decisions on entity and provider).
- **Consumer-law flows built in:** real-currency prices beside virtual currency, odds disclosure, parental approval of
  payments (Turkey 5651 from 2026-11-01 for game platforms), no loot boxes for minors in Brazil, withdrawal-right
  waivers only with the required consent and disclosure.
- **Principles.** (a) Prices, offers and catalogue rules are authored assets; a price change is a signed generation.
  (d) Entitlements are one owner; EXT.5's marketplace and SHIP.9's paid mods consume it.

<a id="area-7-compliance-legal"></a>
## 7. Store compliance and legal

### State of the art 2026

- **Age ratings.** IARC issues ratings for participating storefronts (Google Play, Microsoft Store, Nintendo eShop and
  others) from one questionnaire (K); PEGI, ESRB, USK, CERO and GRAC are the regional boards (K). Apple runs its own
  system with 13+, 16+ and 18+ tiers [S13](#s13). **Turkey:** amendments to Law 5651 published 2026-05-01 apply from
  2026-11-01 to "game platforms" (entities providing software or infrastructure to display, sell, distribute, download
  or play games online, or license management and user interaction): games must be age-rated (unrated games only in the
  highest age class), parental tools must control account settings and approve paid transactions, foreign platforms
  with over 100,000 daily accesses from Turkey appoint a local representative, and ICTA (BTK) issues the rating details
  in secondary legislation not yet published; fines are TRY 1-10 million, plus bandwidth throttling; the amendment
  places no specific duty on developers or distributors as such [S41](#s41).
- **Age assurance.** UK Online Safety Act children's codes since 2025-07-25 reach in-game chat, voice and UGC
  [S51](#s51). Texas's App Store Accountability Act was enjoined in December 2025, then reportedly took effect
  2026-06-04 after the Fifth Circuit lifted the injunction [S14](#s14) (single secondary source, uncertain); Apple's
  Declared Age Range API serves such laws [S14](#s14). Brazil's ECA Digital (Law 15.211, in force 2026-03-17, Decree
  12.880) bans paid random loot boxes in games aimed at or likely to be accessed by minors, or requires age verification
  or a version without them; ANPD enforcement on studios from August 2026 [S43](#s43). Turkey bars social networks from
  under-15s [S41](#s41). The US amended COPPA Rule applies from 2026-04-22 [S50](#s50).
- **Virtual currencies and loot boxes.** EU CPC Key Principles (2025-03-21, soft law): virtual currency purchases and
  spending fall under consumer law, prices in real money, no pressure, withdrawal rights [S44](#s44). The Digital
  Fairness Act was still a consultation as of May 2026 [S45](#s45). Belgium's ban and other national rules (K).
- **Privacy labels.** Apple privacy nutrition labels and privacy manifests [S16](#s16); Google Play Data safety (K);
  GDPR; **KVKK**: since Law 7499 (2024-03-12) and the By-Law of 2024-07-10, cross-border transfers use adequacy,
  appropriate safeguards (standard contracts notified to the Authority within five business days, unmodified) or narrow
  incidental cases [S37](#s37); notice (aydinlatma metni) and explicit consent are separate, and VERBIS registration
  applies by threshold or for foreign controllers (K).
- **Accessibility.** EAA since 2025-06-28 covers e-commerce services such as storefronts and purchase flows; whether
  in-game chat is in scope is unsettled [S48](#s48); CVAA for communication functions in the US (K); Apple
  Accessibility Nutrition Labels are voluntary now and will become required with notice [S15](#s15); Xbox Accessibility
  Guidelines (K).
- **AI features.** EU AI Act Article 50 (disclosure of AI interaction, machine-readable marking of synthetic media) in
  force since 2026-08-02, marking for pre-existing generative systems by 2026-12-02 [S47](#s47).
- **Security and liability.** CRA reporting since 2026-09-11 (24-hour early warning, 72-hour notification, final
  reports) through ENISA's SRP; full obligations from 2027-12-11 [S49](#s49). The EU Product Liability Directive treats
  software as a product from 2026-12-09, makes missing security updates a possible defect and bars contractual
  exclusion of liability [S46](#s46). Turkey's Cybersecurity Law 7545 (2025-03-19) requires prompt reporting of
  vulnerabilities and incidents by private entities and permits from the regulator for cybersecurity products and their
  export [S38](#s38).
- **Platforms for UGC.** The EU DSA applies to in-game chat, UGC and mod hubs used by EU users: notice and action,
  statements of reasons, complaint handling; micro and small enterprises are exempt from the platform-tier duties only
  [S52](#s52).
- **Export control.** US EAR mass-market encryption: most items self-classify under 5D992.c with annual reports only
  for a subset since 2021 [S54](#s54); Apple asks an export-compliance question per build (K); Turkey is a Wassenaar
  participant (K).
- **Commerce in Turkey.** E-commerce Law 6563 and ETBIS: providers selling on their own platforms or as intermediaries
  register before trading and file annual reports [S40](#s40); a 2026-06-02 Constitutional Court ruling annulled the
  marketplace content-liability shield with effect from 2027-03-02 [S40](#s40) (single secondary source, uncertain).
- **Open-source obligations.** Attribution and licence texts in shipped binaries, source offers for LGPL/MPL components,
  patent and notice clauses (K); Cerid's pins carry licences (REPO.DEV.6).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Obligations register with dates and owners | None public at this depth (engines leave it to studios) | Missing; NET-STD proposed for protocols only |
| Rating questionnaire answers as data | IARC, Apple questionnaire [S13](#s13) | Missing |
| Privacy labels from real data flows | Apple manifests, Play Data safety [S16](#s16) | Missing; OPS.1/CRASH-FIX.1 define consented data |
| KVKK and GDPR notices and transfer records | KVKK By-Law [S37](#s37) | Missing |
| Parental controls and age signals | Apple Declared Age Range, UK OSA [S14](#s14) [S51](#s51) | Missing |
| Loot-box and virtual-currency rules | ECA Digital, CPC [S43](#s43) [S44](#s44) | Missing |
| Accessibility labels and statements | EAA, Apple labels [S15](#s15) [S48](#s48) | A11Y-GAME and I18N.1 build features, not store metadata |
| OSS licence notices per export | (K) | Missing; SBOM in SUPPLY.1 is the input |
| Export-control record | EAR 5D992 [S54](#s54) | Missing |
| CRA, PLD, Turkish 7545 readiness | [S38](#s38) [S46](#s46) [S49](#s49) | SEC.1 covers CRA readiness |

### Gaps in area 7

- **COMPLY.1:** a machine-readable obligations register (jurisdiction, platform, obligation, source, effective date,
  owning row, check, evidence, review date), Turkey first-class, sharing machinery with NET-STD (networking proposal)
  so there is one register substrate with two domains. A CTest fails when an effective date passes without a passing
  check or a review date lapses.
- **COMPLY.2:** generate every store compliance artifact from facts the engine already knows (data flows and consent
  tiers, SBOM, cryptography used, features used, rating answers authored once), plus the runtime kits (consent and
  notice UI, parental controls, age signals, loot-box rule engine, AI disclosure, DSA notice and action).
- **Turkey 5651:** a Cerid-built storefront, launcher with licence management or mod hub matches the "game platform"
  definition [S41](#s41), so the platform kit (age rating, parental tools, payment approval, representative threshold
  monitoring, ICTA request handling within 15 days) belongs in COMPLY.2 and SHIP.9; ICTA's secondary rules are a
  register watch item.
- **Principles.** (a) Rules are authored assets swapped live per jurisdiction. (d) COMPLY owns rules and generators;
  products and stores consume them.

<a id="area-8-operations"></a>
## 8. Operations and app management

### State of the art 2026

- **Release automation.** fastlane lanes over the App Store Connect and Google Play Developer APIs; SteamCMD builds
  [S21](#s21); the Microsoft Store submission API and `msstore` CLI; EOS BuildPatch Tool; itch.io butler (K).
- **Release health.** Crash-free users and sessions per version (Firebase Crashlytics, Sentry, Backtrace), ANR rates as
  Play vitals with store penalties (K).
- **Live ops.** Remote config, feature flags (OpenFeature), A/B tests, event calendars, offers (K).
- **Incident response.** Kill switches, emergency rollback, status pages, store expedited review (K).
- **Support.** Player and customer lookup with privacy controls, entitlement grants, refunds, log and crash pack intake
  (K).
- **Community and mods.** Steam Workshop; mod.io across PC, Mac, PS5, Xbox Series and Switch 2 (Cyberpunk 2077 Update
  2.5, October 2026), with platform content criteria [S53](#s53); CurseForge and Nexus (K).
- **Fleets.** Coordinated client and server releases with compatibility windows; Agones rolling fleet updates (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Store API automation | fastlane, SteamCMD, Store API (K) [S21](#s21) | Missing |
| Release health gates | Crashlytics-class (K) | Planned base: CRASH-FIX.2 pool, OPS.1 telemetry |
| Flags, remote config, experiments | OpenFeature, LaunchDarkly (K) | Missing |
| Publisher console | Steamworks partner site, Play Console (K) | Missing |
| Mod distribution and moderation | Workshop, mod.io [S53](#s53) | Missing |
| Agent-driven release with approvals | (K) | Missing; AGENT.3 has authority and budgets |

### Gaps in area 8

- **SHIP.8:** one release state model and console (CR-D007 workspace, `ceridc`, MCP) over all store adapters, with
  health gates from CRASH-FIX.2 and OPS.1, review intake, store status, flags and experiments as authored assets,
  live-ops calendars, support tooling and incident runbooks; irreversible actions (submit, widen rollout, price change,
  delete a build) need a recorded human approval.
- **SHIP.9:** mods as EXT.4 packages with a mod profile, author signing and platform countersignature, content-only by
  default, CHIR/CEIR behaviour mods in EXT.3's isolated tier, native mods only on trusted desktop targets; hub adapters
  (Workshop through SHIP.6, mod.io, a self-hosted Cerid hub); moderation under DSA, UK OSA and Turkey 5651.
- **Principles.** (c) Release scripts are CHIR commands. (f) Peers: Play Console and Steamworks for time from upload to
  live; mod.io for mod install latency.

<a id="area-9-developer-experience"></a>
## 9. Developer experience

### State of the art 2026

- **One-command flows.** `eas build` and `eas submit` (Expo), `tauri build`, `electron-forge publish`, `fastlane
  release`, Unreal BuildCookRun with `-distribution` (K).
- **CI.** GitHub Actions, GitLab CI and self-hosted runners with OIDC to cloud key services; artifact attestations (K).
- **Test of store builds.** Clean VM matrices, Steam Deck devices, Firebase Test Lab and AWS Device Farm, TestFlight and
  Play internal testing tracks (K).
- **Validators.** Apple's upload validation, Play pre-launch reports, Windows App Certification Kit, Flathub linter (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| `ceridc package/publish` | eas, tauri, forge (K) | Planned: DX.1 `package`; no publish |
| Publishing profiles as assets | Unity Build Profiles (K) | Missing |
| CI templates | Vendor templates (K) | Missing (DX.2 templates are project templates) |
| Pre-submission validators | Store validators (K) | Missing |
| Store-build test matrices | Device farms (K) | QUAL.2 names representative hardware |

### Gaps in area 9

- **SHIP.10:** `ceridc publish --profile <p>` plans (a dry run with the step list, sizes, costs and approvals), builds,
  cooks, packages, signs, validates, uploads and stages; the same command in CR-D007 and MCP; CI templates for GitHub
  Actions and GitLab with OIDC signing; store-rule validators driven by COMPLY.1 entries.
- **SHIP.11:** qualification on clean VMs per OS floor, a Steam Deck, real store private branches and tracks, and device
  farms where accounts exist; Cerid dogfoods by shipping `ceridc`, the MCP server and CR-D007 through the same pipeline.
- **Principles.** (c) Every publishing step is a CHIR command with one schema for CLI, editor and MCP. (d) Validators
  read COMPLY.1 entries instead of embedding rules. (f) Peers: eas, tauri and Unreal BuildCookRun for time from commit
  to a staged store build.

<a id="area-10-staying-current"></a>
## 10. Staying current

### State of the art 2026

Platform rules change every year on a known rhythm (K unless marked): Google Play raises the target API every August
[S05](#s05); Apple raises the minimum SDK every April [S12](#s12); store questionnaires change (Apple age ratings
[S13](#s13)); signing rules tighten (460 days [S02](#s02)); privacy and child-safety laws arrive by country (Brazil,
Turkey, UK, US states [S41](#s41) [S43](#s43) [S51](#s51)); EU acts phase in (CRA, AI Act, PLD, possibly the DFA
[S45](#s45) [S46](#s46) [S47](#s47) [S49](#s49)); runtimes re-base (SLR 4.0 [S23](#s23)); OS support ends (Intel Macs
[S04](#s04), Windows 10). No engine publishes a versioned obligations register; studios track this by hand.

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Versioned register of legal and platform obligations | None public at this depth; studios track by hand | Missing; NET-STD proposed for protocols only |
| Review cadence with event triggers | Platform yearly rhythm [S05](#s05) [S12](#s12) | Missing |
| A check that fails when an effective date passes unmet | None known | Missing |
| One register substrate for protocols and obligations | None known | Missing (networking proposal P-1 covers protocols) |

### Gaps in area 10

- **COMPLY.1 cadence:** quarterly review of every entry, an annual full re-research, and event triggers (Apple WWDC and
  April SDK rule, Google I/O and the August target API rule, Steamworks announcements, Official Gazette of Turkey,
  EUR-Lex). Each review proposes amendments for the user; downloads they need are listed for approval.
- **One substrate with NET-STD** (networking proposal): the same register format, CTest and review machinery, so
  protocol standards and legal or platform obligations stay current the same way.
- **Principles.** (a) Register entries are authored assets; a rule change is a reviewed generation. (d) COMPLY owns the
  rules; every row cites the entries it implements. (g) Every entry carries its source and date.

<a id="gaps-and-proposed-roadmap-changes"></a>
## Gaps and proposed roadmap changes

Proposals only; the user approves before any [ROADMAP](../ROADMAP.md) edit. Positions follow the user's re-plan: every
row sits right after the rows it needs, none stays Later. Every Requires names a row that sits earlier in the table;
proposed IDs are plain text. The family: COMPLY.1 early (obligations already bind), SHIP.0 decision after DATA-TRUST,
the headless pipeline (SHIP.1-SHIP.3) before EDITOR.6 so the editor is a client, Windows and Linux packaging after
EDITOR.7, stores, entitlements, compliance kit, console, mods and the publish command after DX.4, qualification and
the SHIP parent before QUAL.2, and each later platform right after its platform row.

| # | Row | New/Amend | Position | Requires | Proposal |
|---:|---|---|---|---|---|
| 1 | COMPLY.1 | New | After [SEC.1](../ROADMAP.md#slice-sec.1) (129) | SEC.1, OWN.1 | [P-1](#p-1) obligations register and review cadence |
| 2 | SHIP.0 | New | After [DATA-TRUST](../ROADMAP.md#slice-data-trust) (409) | SUPPLY.3, EXT.4, API-STAB.0, DATA-TRUST.2, COMPLY.1 | [P-2](#p-2) decision and ADR |
| 3 | SHIP.1 | New | After [EDITOR.5](../ROADMAP.md#slice-editor.5) (415) | SHIP.0, SUPPLY.2, SUPPLY.4, PQP-4.renderer | [P-3](#p-3) headless target build and export |
| 4 | SHIP.2 | New | After SHIP.1 | SHIP.1, HW-RESILIENCE.1 | [P-4](#p-4) crdr v2 content packaging |
| 5 | SHIP.3 | New | After SHIP.2 | SHIP.2, NET.5, CRASH-FIX.2, DOC.2, API-STAB.0 | [P-5](#p-5) updates, patching, rollout |
| 6 | SHIP.4 | New | After [EDITOR.7](../ROADMAP.md#slice-editor.7) (417) | EDITOR.7, SHIP.3, DX12-AGILITY | [P-6](#p-6) Windows packages and Store |
| 7 | SHIP.5 | New | After SHIP.4 | EDITOR.7, SHIP.3 | [P-7](#p-7) Linux packages and runtimes |
| 8 | SHIP.6 | New | After [DX.4](../ROADMAP.md#slice-dx.4) (435) | SHIP.4, SHIP.5, DX.1 | [P-8](#p-8) PC store adapters, one Steamworks owner |
| 9 | SHIP.7 | New | After SHIP.6 | SHIP.6, SEC.1, COMPLY.1, NET.5 | [P-9](#p-9) entitlements, licences, payments |
| 10 | COMPLY.2 | New | After SHIP.7 | COMPLY.1, SHIP.7, CRASH-FIX.1, I18N.1, A11Y-GAME | [P-10](#p-10) store compliance kit |
| 11 | SHIP.8 | New | After COMPLY.2 | SHIP.7, COMPLY.2, CRASH-FIX.2, AGENT.3, MCP-ADAPT | [P-11](#p-11) publisher console, live ops |
| 12 | SHIP.9 | New | After SHIP.8 | SHIP.8, EXT.3, EXT.4 | [P-12](#p-12) mods and UGC distribution |
| 13 | SHIP.10 | New | After SHIP.9 | SHIP.9, DX.1, AGENT | [P-13](#p-13) `ceridc publish`, CI, validators |
| 14 | SHIP.11 | New | After [OPS.1](../ROADMAP.md#slice-ops.1) (566) | SHIP.10, OPS.1 | [P-14](#p-14) qualification and documentation |
| 15 | SHIP | New | After SHIP.11, before QUAL.2 | SHIP.11 | [P-15](#p-15) parent close |
| 16 | SHIP-MAC | New | After [MAC](../ROADMAP.md#slice-mac) (662) | MAC, SHIP | [P-16](#p-16) notarized macOS, Mac App Store |
| 17 | SHIP-WEB | New | After [WEB](../ROADMAP.md#slice-web) (672) | WEB, SHIP | [P-17](#p-17) web deployment |
| 18 | SHIP-ENTERPRISE | New | After SHIP-WEB | SHIP-MAC, SHIP.7 | [P-18](#p-18) managed and air-gapped deployment |
| 19 | SHIP-CONSOLE | New | After [6.0](../ROADMAP.md#slice-6.0) (943) | 6.0, SHIP | [P-19](#p-19) console submission packs |
| 20 | SHIP-MOBILE | New | After [6.1](../ROADMAP.md#slice-6.1) (944) | 6.1, SHIP, CEIR-JIT.3 | [P-20](#p-20) iOS and Android stores |
| 21 | SHIP-XR | New | After [6.3](../ROADMAP.md#slice-6.3) (946) | 6.3, SHIP-MOBILE, SHIP-MAC | [P-21](#p-21) XR stores |
| 22 | EDITOR.6 | Amend | In place (416) | EDITOR.5, SHIP.3 | [P-22](#p-22) editor client of SHIP |
| 23 | EDITOR.7 | Amend | In place (417) | EDITOR.6 | [P-23](#p-23) clean machine on SHIP packages |
| 24 | EDITOR | Amend | In place (418) | EDITOR.7, SHIP.5 | [P-24](#p-24) parent needs real installers |
| 25 | QUAL.2 | Amend | In place (567) | SHIP | [P-25](#p-25) release evidence on real stores |
| 26 | SUPPLY.1 | Amend | In place (126) | REPO.DEV.6 | [P-26](#p-26) per-target SBOM, notices input |
| 27 | SUPPLY.2 | Amend | In place (128) | SUPPLY.1 | [P-27](#p-27) provenance for content and packages |
| 28 | SUPPLY.3 | Amend | In place (318) | SUPPLY.2 | [P-28](#p-28) 2026 signing facts and signing service |
| 29 | SUPPLY.4 | Amend | In place (127) | SUPPLY.1 | [P-29](#p-29) reproducible packages |
| 30 | SEC.1 | Amend | In place (129) | - | [P-30](#p-30) key and channel compromise, 7545 |
| 31 | CEIR-JIT.3 | Amend | In place (431) | CEIR-JIT.2, EDITOR.7, SHIP.0 | [P-31](#p-31) changeability matrix targets |
| 32 | CRASH-FIX.1 | Amend | In place (121) | DIAG, HW-RESILIENCE.2 | [P-32](#p-32) release identity in packs |
| 33 | CRASH-FIX.2 | Amend | In place (122) | CRASH-FIX.1, NET.5 | [P-33](#p-33) per-release health API |
| 34 | OPS.1 | Amend | In place (566) | DIAG, HW-RESILIENCE.2 | [P-34](#p-34) release health, data inventory |
| 35 | OPS.2 | Amend | In place (967) | OPS.1, SUPPLY.2, SUPPLY.4, SHIP | [P-35](#p-35) install and upgrade matrices |
| 36 | OPS.3 | Amend | In place (968) | OPS.2 | [P-36](#p-36) build and cook farm |
| 37 | EXT.4 | Amend | In place (319) | EXT.3, SUPPLY.3, API-STAB.0 | [P-37](#p-37) shared update metadata, mod profile |
| 38 | EXT.5 | Amend | Move after SHIP.9 | EXT.4, SHIP.7, COMPLY.2, SHIP.9 | [P-38](#p-38) marketplace on SHIP and COMPLY |
| 39 | 4.2d | Amend | In place (900) | NET.5, SHIP.6 | [P-39](#p-39) consumes the Steamworks binding |
| 40 | NET-SERVER | Amend | In place (901) | 4.2d, SHIP | [P-40](#p-40) server images, skew windows |
| 41 | HW-RESILIENCE.1 | Amend | In place (095) | SIMD-DISPATCH | [P-41](#p-41) one checksum family for installs |
| 42 | DOC.2 | Amend | In place (312) | DOC.1, REFLECT.2 | [P-42](#p-42) migration across releases |
| 43 | API-STAB.0 | Amend | In place (300) | CMD.2, REFLECT | [P-43](#p-43) content ABI ranges |
| 44 | DATA-TRUST.2 | Amend | In place (408) | DATA-TRUST.1 | [P-44](#p-44) store and mod publisher identity |
| 45 | AGENT.3 | Amend | In place (325) | AGENT.2 | [P-45](#p-45) irreversible publication authority |
| 46 | ANTICHEAT | Amend | In place (902) | NET.4, 4.2c, NET-SERVER | [P-46](#p-46) boundary with anti-tamper |
| 47 | I18N.1, A11Y-GAME | Amend | In place (365, 378) | I2D-6.d; I2D-8.e | [P-47](#p-47) store listing and label outputs |
| 48 | DX.1 | Amend | In place (432) | EDITOR.6, NATIVE.2, EXT.4 | [P-48](#p-48) `package` produces SHIP outputs |
| 49 | JOBS-PORT.2 | Amend | In place (658) | JOBS-PORT.1, SHIP.5 | [P-49](#p-49) ARM64 packages |
| 50 | 6.0, 6.1, 6.3 | Amend | In place (943, 944, 946) | CEIR-JIT.3 | [P-50](#p-50) no Later; SHIP rows follow |
| 51 | EMBED.2 | Amend | In place (962) | EMBED.1, SHIP.3 | [P-51](#p-51) secure OTA, kiosk |
| 52 | PORTFOLIO.1 | Amend | In place (1015) | OPS, AI, SHIP | [P-52](#p-52) acceptance through real stores |

Totals: 21 new rows (COMPLY.1-2, SHIP.0-SHIP.11, the SHIP parent and six platform rows) and 31 amendment entries
(22-52) covering 34 existing rows (entry 47 amends two rows, entry 50 three). ADR-0038 is amended through SHIP.2.

### Proposal texts

<a id="p-1"></a>
**P-1. COMPLY.1 (new, after SEC.1).** Obligations register and review cadence. A machine-readable register beside the
module and capability registries lists every legal, tax, consumer, privacy, child-safety, accessibility, security,
export-control and platform or store obligation that applies to Cerid or to products built with it: jurisdiction
(Turkey first-class, then the EU and member states, UK, US federal and states, Japan, Korea, Brazil, China and others
as products enter them), platform or store, source (law, regulation, policy page, with date and version), effective
and sunset dates, owning row, the check that proves compliance, evidence, and next review date. Seed entries include
CRA reporting (2026-09-11) and full obligations (2027-12-11), AI Act Article 50, the EU PLD (2026-12-09), the DSA, the
EAA, GDPR, KVKK cross-border rules and VERBIS, Turkish Laws 5651 (2026-11-01 game platforms; ICTA rules pending), 6502
and the Distance Contracts Regulation, 6563 and ETBIS, 7545, the VAT-3 regime and the DST thresholds, Brazil ECA
Digital, UK OSA, COPPA, Texas and Utah app store laws, Play target API and 16 KB rules, Android developer verification,
Apple SDK minimums, privacy manifests, age-rating questionnaires and accessibility labels, CA/B code-signing rules,
EAR 5D992 and Steam, Epic and Microsoft Store policies. It shares the register format, CTest and review machinery with
NET-STD (networking proposal P-1), so there is one register substrate with two domains. A CTest fails when an
effective date passes with no passing check, or a review date lapses. Reviews run quarterly, after the yearly platform
events (April SDK rule, August target API, WWDC, Google I/O) and on Official Gazette and EUR-Lex triggers; an annual
full re-research proposes amendments to the user. Legal interpretation is recorded as counsel-reviewed or not; an
agent never marks an obligation satisfied without evidence. Entries are CHIR assets, queryable over `ceridc` and MCP.

<a id="p-2"></a>
**P-2. SHIP.0 (new, after DATA-TRUST; decision and ADR).** Publishing architecture. Decides: the owning module
`crd-ship` (publishing service library used by `ceridc`, CR-D007 and MCP; registered in OWN.1), the release data model
(product, application, target, profile, build, artifact, package, channel, rollout, entitlement, store listing,
compliance record), the trust model (release keys only in an HSM behind a signing service, OIDC from CI, policy
checked before signing), the update trust model (TUF-class roles shared by SHIP.3, EXT.4 and SHIP.9), and the
**changeability matrix**: per platform and store, which asset kinds (data, CHIR/CEIR interpreted content, CKIR
kernels, native code) may update without a binary, under which policy entry in COMPLY.1, with which engine ABI range
(API-STAB.0). It records what each consumer row keeps: cookers, `crd-resources` (container), SUPPLY (SBOM, provenance,
signing primitives, reproducibility), PQP-4.renderer (pipeline caches), CEIR-JIT.3 (AOT objects), CRASH-FIX (symbols,
health), OPS.1 (telemetry), EXT.4 (package format), DATA-TRUST (trust boundary), DOC.2/REFLECT.2 (migration) and
ANTICHEAT. Peers to beat are named per area. No implementation before acceptance.

<a id="p-3"></a>
**P-3. SHIP.1 (new, after EDITOR.5; headless first).** Target build and export pipeline. Target descriptors and
publishing profiles are CHIR assets edited live; a profile change re-plans only affected steps. Per-target cooking
(texture, audio, mesh and program formats per GPU tier and platform), reachability closure from entry scenes, programs
and commands with explicit keeps and a closure report, module exclusion through REPO.DEV.4's `CRD_MODULES` closure,
editor and debug tooling stripped (SCRIPT.4 shipping allowlists), PQP-4.renderer pipeline-cache seeds, interpreter,
compiled-plan or AOT selection per target from SHIP.0's matrix, symbols split and uploaded to the CRASH-FIX.2 symbol
store, SBOM and provenance attached (SUPPLY.1, SUPPLY.2), deterministic package assembly (SUPPLY.4), a content-addressed
cook cache keyed like VCS.2 and shareable across machines (farm scale-out through OPS.3). Every step is a command with
progress, cancellation and a dry-run plan. Budgets: clean and incremental export time against Unreal BuildCookRun with
Zen and Unity Build Profiles on comparable projects; exported size against the same projects.

<a id="p-4"></a>
**P-4. SHIP.2 (new, after SHIP.1; owner `crd-resources`, ADR-0038 amended).** crdr v2 content packaging. Per-chunk
checksums from HW-RESILIENCE.1's family and a Merkle root in a manifest signed through SUPPLY.3; chunk alignment for
unbuffered and DirectStorage I/O; content-defined boundaries for large payloads so a small edit yields a small patch;
mount layers with explicit priority (base, patch, DLC, language, mod) and conflict reports; per-chunk codec: zstd with
trained dictionaries (owned default), GDeflate and DirectStorage 1.4 Zstd GPU paths where present with a CPU path at
the SSE2 floor (OWN.1 decides whether codecs live in `crd-resources` or a foundation module; Oodle only as a measured
peer if the user licenses it); install priority classes for play-while-installing and on-demand packs; optional
per-chunk AES-GCM with entitlement-delivered keys, documented as a deterrent; repair of corrupt chunks from the source.
Adversarial tests flip bits, truncate and reorder chunks. Budgets: load time against IoStore/Zen, patch bytes for a
one-asset change against Steam and Epic chunking, decompression throughput against DirectStorage.

<a id="p-5"></a>
**P-5. SHIP.3 (new, after SHIP.2).** Updates, patching and rollout. TUF-class signed metadata (root, targets, snapshot,
timestamp roles, thresholds, expiry, rollback and freeze protection) covering binaries, content packs, plugin packages
and mods; chunk-level deltas from SHIP.2 manifests and binary deltas for executables with an owned implementation
(bsdiff and Courgette/Zucchini-class disassembly-aware diffing as oracles); resumable, rate-limited background
downloads; install into a new slot with atomic switch and rollback to the last good slot; repair; offline and
air-gapped update bundles. Channels (internal, alpha, beta, stable, LTS), staged percentages that advance only when
CRASH-FIX.2's per-release crash-free rate and OPS.1's consented signals pass declared gates (sequential tests in
`hesap-stats`), automatic halt and rollback, kill switches and remote config as signed authored generations. Live
content hot-fix applies SHIP.0's matrix: a content pack declares its engine ABI range (API-STAB.0) and migrations
(DOC.2); where a store updates the app, the owned updater is disabled and only matrix-permitted content updates run.
Client and server version windows bind to channels (NET-SERVER amendment). Faults: power cut, full disk, torn
download, malicious mirror, expired metadata, clock skew; each yields a verified retry or the old install.

<a id="p-6"></a>
**P-6. SHIP.4 (new, after EDITOR.7).** Windows packages and the Microsoft Store. MSIX with App Installer updates (and a
sparse package with external location where a classic install is needed), MSI generated by an owned generator with
WiX v6 as the peer and oracle for enterprise and Group Policy (silent install, ADMX policy templates), a signed
bootstrapper, per-user and per-machine installs, clean uninstall, winget manifests and their pull requests, Microsoft
Store submission (Store submission API), the DX12 Agility SDK redistribution from DX12-AGILITY, app-local runtimes,
Smart App Control and SmartScreen behaviour recorded per release. DAW plugin products install VST3 and CLAP bundles to
their standard locations. ARM64 packages join after JOBS-PORT.2. Qualification on clean Windows 11 VMs of each
supported build; peers: Velopack and MSIX for update latency and installer size.

<a id="p-7"></a>
**P-7. SHIP.5 (new, after SHIP.4).** Linux packages and runtimes. Flatpak manifests and Flathub submission (runtime
choice, permissions minimised), AppImage, deb and rpm with signed repositories (apt and dnf metadata), tarballs, and
Steam Linux Runtime 3 (sniper) and 4.0 targets; the build depends only on libc, Vulkan, audio and display ABIs from the
host, proven on immutable systems (SteamOS, Fedora Atomic) and on the oldest supported glibc. VST3 and CLAP plugin
bundles install to their standard paths. Snap only if a product needs it (recorded decision).

<a id="p-8"></a>
**P-8. SHIP.6 (new, after DX.4).** PC store adapters behind one store interface, with one Steamworks owner. The
Steamworks binding (depots per platform, language and DLC, branches, SteamCMD/ContentBuilder upload, achievements,
stats, cloud saves, overlay, Steam Input, optional DRM wrapper, Workshop for SHIP.9, and the lobby and P2P APIs that
4.2d's `crd-net-steam` consumes) is owned here once; EOS (Epic store, BuildPatch upload, and its networking for 4.2d),
GOG Galaxy and offline DRM-free installers, itch.io butler, and the Microsoft Store from SHIP.4 are further adapters.
A Steam Deck, Steam Machine and Steam Frame Verified checklist validator runs in CI (input, display legibility, default
settings, seamlessness, Proton and native SLR support). Package layout is aligned to each store's chunking so patches
stay small; patch bytes per update are a board against Unreal and Unity titles. Store SDKs need user downloads and
partner accounts.

<a id="p-9"></a>
**P-9. SHIP.7 (new, after SHIP.6).** Entitlements, licensing and payments. One entitlement interface over store
ownership and DLC (Steam, EOS, GOG, Microsoft Store; StoreKit 2 and Play Billing through SHIP-MOBILE), Cerid licence
files (signed, Ed25519 with a hybrid post-quantum path when NET.2's primitives exist), and a floating licence server
for engineering products that runs hosted, on premises and air-gapped (signed request and response files), with
borrowing, expiry, revocation, usage reporting, clock-tamper detection and grace periods; node-locked bindings tolerate
hardware changes within declared limits. Payments are adapters (store billing, merchants of record, Turkish payment
service providers with e-archive invoicing through an integrator for a Turkish seller); Cerid never stores card data.
Receipts are validated server-side; refunds and chargebacks revoke entitlements; fraud signals feed SHIP.8. Virtual
currency and offers follow COMPLY.2's rules (real-currency prices, odds, parental approval, minors). Peers: FlexNet and
LicenseSpring for licence features, Steamworks for entitlement latency.

<a id="p-10"></a>
**P-10. COMPLY.2 (new, after SHIP.7).** Store compliance kit. Generators from facts: the data inventory (from OPS.1 and
CRASH-FIX.1 consent tiers and the features a profile enables) produces Apple privacy labels and `PrivacyInfo.xcprivacy`
with required-reason entries, Google Play Data safety answers, KVKK notices (aydinlatma metni), explicit-consent texts
and transfer records (standard-contract notification reminders), GDPR records; the SBOM (SUPPLY.1) produces licence
notices and source offers for each export; cryptography in use produces the EAR 5D992 self-classification record and
the Apple export-compliance answer; rating answers authored once produce IARC, Apple and regional submissions and the
Turkish rating when ICTA publishes its scheme. Runtime kits: consent and notice UI, parental controls with payment
approval (Turkey 5651), age signals (Apple Declared Age Range, Play age signals, UK OSA age assurance where chat or UGC
exists), a per-jurisdiction loot-box and virtual-currency rule engine (Brazil ECA Digital, CPC principles, Belgium and
others), AI disclosure and synthetic-media marking (AI Act Article 50), accessibility statements and store labels
(EAA purchase flows, CVAA communication features, Apple accessibility labels, from A11Y-GAME), DSA notice and action
and statements of reasons (with SHIP.9), and the 5651 platform kit (representative threshold monitoring, ICTA request
handling). EULA, terms and privacy-policy templates are counsel-reviewed assets (a user action), localized with I18N.1.

<a id="p-11"></a>
**P-11. SHIP.8 (new, after COMPLY.2).** Publisher console and live operations. One release state model over all store
adapters, shown in a CR-D007 workspace and served over `ceridc` and MCP: versions, channels, rollout state, store
review status, crash-free rates and regressions (CRASH-FIX.2), consented performance telemetry (OPS.1), reviews and
ratings intake, release notes and changelogs generated from commits and asset changes and localized, store listings
from I18N.1, feature flags and remote config (OpenFeature-compatible provider, flags as CHIR assets), experiments with
consent and analysis in `hesap-stats`, live-ops event calendars, support tooling (privacy-scoped lookup, entitlement
grants, refunds, pack intake) and incident runbooks (kill switch, emergency rollback, expedited review requests).
Irreversible actions (submit, widen a rollout, change a price, delete a build or a mod) require a recorded human
approval under AGENT.3's authority classes; agents prepare, humans approve. Server fleet releases coordinate with
NET-SERVER windows.

<a id="p-12"></a>
**P-12. SHIP.9 (new, after SHIP.8).** Mods and UGC distribution. Mods are EXT.4 packages with a mod profile:
content-only by default, CHIR/CEIR behaviour in EXT.3's isolated tier, native mods only on trusted desktop targets and
never where SHIP.0's matrix forbids them (consoles, iOS); author signatures plus a platform countersignature; load order
and conflicts through SHIP.2 mount layers; compatibility ranges; crash attribution to mods in CRASH-FIX packs; save
compatibility rules. Hub adapters: Steam Workshop (SHIP.6), mod.io, CurseForge where a product needs it, and a
self-hosted Cerid hub; moderation pipeline with DSA notice and action, UK OSA duties where children can access UGC and
Turkey 5651 removal handling; paid mods through SHIP.7.

<a id="p-13"></a>
**P-13. SHIP.10 (new, after SHIP.9).** One-command publishing and CI. `ceridc publish --profile <name>` prints a plan
(steps, sizes, signing identity, stores, approvals needed), then builds, cooks, packages, signs, validates, uploads and
stages; the same command in CR-D007 and as MCP tools; pre-submission validators driven by COMPLY.1 entries (Apple
Info.plist, entitlements and privacy manifest checks, Play target API, 16 KB alignment and AAB rules, Steam Verified
checklist, Microsoft Store and Windows App Certification Kit rules, Flathub linter); CI templates for GitHub Actions and
GitLab CI and self-hosted runners with OIDC to the signing service and no secrets in repositories; local store-build
test matrices (clean VMs, Steam Deck) and device-farm adapters (Firebase Test Lab, AWS Device Farm) where accounts
exist. DX.2's templates ship with publishing profiles.

<a id="p-14"></a>
**P-14. SHIP.11 (new, after OPS.1).** Qualification and documentation. Windows and Linux products (a game, an
engineering viewer, a DAW plugin, a notebook-based tool, a command-line tool) go from project to installed, updated,
rolled back and uninstalled on clean machines at each OS floor and on a Steam Deck, through real private store branches
and tracks where accounts exist; seeded faults (torn updates, malicious mirrors, expired metadata, revoked keys,
corrupt chunks, power cuts) prove recovery; Cerid ships `ceridc`, the MCP server and CR-D007 through the same pipeline.
User guide, operator and incident runbooks (diagnostic-runbook style), design reference, ADR note and MCP surface.

<a id="p-15"></a>
**P-15. SHIP (new parent, after SHIP.11, before QUAL.2).** Windows and Linux publishing closed: all children pass;
later platforms close in their SHIP-* rows.

<a id="p-16"></a>
**P-16. SHIP-MAC (new, after MAC).** Developer ID signing, hardened runtime and minimal entitlements, notarization and
stapling, DMG and pkg, the owned updater (SHIP.3) meeting Sparkle 2's bar (signed appcasts, sandboxed installer), Mac
App Store sandbox builds and TestFlight, Steam macOS depots, AU, VST3 and CLAP plugin bundles notarized, universal2
binaries while Intel Macs on Tahoe 26 receive security updates and arm64-only after (dates in COMPLY.1).

<a id="p-17"></a>
**P-17. SHIP-WEB (new, after WEB).** Web deployment from `ceridc publish`: immutable hashed assets on a CDN, Subresource
Integrity, the content security policy and isolation headers from WEB.1, Brotli and zstd content encoding, service
worker update channels with the SHIP.3 metadata format (WEB.4), staged rollout by edge configuration, PWA manifests and
store listings for installable web apps (Microsoft Store PWA, Play trusted web activity) where products need them.

<a id="p-18"></a>
**P-18. SHIP-ENTERPRISE (new, after SHIP-WEB).** Managed deployment: Intune (Win32 and MSIX), Configuration Manager,
Group Policy and ADMX templates for product settings, Jamf and MDM configuration profiles on macOS, silent installs,
offline installers, air-gapped update bundles verified with SHIP.3 metadata, on-premises licence servers from SHIP.7,
and documentation for IT administrators.

<a id="p-19"></a>
**P-19. SHIP-CONSOLE (new, after 6.0).** Console submission packs per platform holder: package formats through the
licensed SDK tools, entitlement, trophy and achievement adapters behind SHIP.7, patch-size rules, certification
checklists as private register entries kept outside the public repository (NDA), and AOT-only builds from CEIR-JIT.3
with content updates only where the matrix allows. Needs licensed accounts and dev kits (user actions).

<a id="p-20"></a>
**P-20. SHIP-MOBILE (new, after 6.1).** iOS and iPadOS: App Store and TestFlight, current-Xcode SDK rule, privacy
manifests, StoreKit 2 entitlements in SHIP.7, export compliance, EU alternative marketplaces and web distribution with
Apple notarization, Japan MSCA routes, age-range signals. Android: AAB with Play App Signing and v3.1 key rotation, Play
Asset Delivery packs mapped from SHIP.2 install classes, Play Integrity, the yearly target API rule, 16 KB page
alignment, developer verification for sideloaded and alternative-store builds, alternative stores (Galaxy Store and
others), Play Billing and alternative billing where law allows. AOT from CEIR-JIT.3; content updates within SHIP.0's
matrix (no downloaded native code; interpreted content only where policy permits).

<a id="p-21"></a>
**P-21. SHIP-XR (new, after 6.3).** Meta Horizon Store submissions against the Virtual Reality Check guidelines, SteamVR
and Steam Frame Verified, visionOS through SHIP-MOBILE and SHIP-MAC tooling, and Android XR through Play.

<a id="p-22"></a>
**P-22. EDITOR.6 (amend).** The editor's export is a client of SHIP.1 to SHIP.3: publishing profiles are edited in
CR-D007 with live validation, and the editor adds no export logic of its own.

<a id="p-23"></a>
**P-23. EDITOR.7 (amend).** The clean-machine game and engineering viewer run from SHIP.2 packages with SHIP.3 updates
(developer-signed and release-signed variants), proving install, update, rollback and uninstall, not only a copied
folder.

<a id="p-24"></a>
**P-24. EDITOR (amend).** The parent's "genuine standalone game publishing" requires SHIP.5's installers on both
platforms.

<a id="p-25"></a>
**P-25. QUAL.2 (amend).** Release evidence includes SHIP's real-store private branches and tracks, installer and update
matrices and the SHIP.11 fault fleet.

<a id="p-26"></a>
**P-26. SUPPLY.1 (amend).** One SBOM per target package reflecting its module and content closure, embedded in the
package and published beside it; COMPLY.2 generates licence notices and source offers from it.

<a id="p-27"></a>
**P-27. SUPPLY.2 (amend).** Provenance covers cooked content, packages, installers and update metadata, not only
compiled binaries; the SHIP.3 updater and EXT.4 verify it before install.

<a id="p-28"></a>
**P-28. SUPPLY.3 (amend).** 2026 facts as acceptance: certificates at most 460 days with automated renewal and rotation
(CSC-31); RFC 3161 timestamps on every signature; dual-signing at renewal; a signing service holding keys in an HSM or
cloud HSM, reached from CI with short-lived OIDC credentials and a policy check (commit, provenance, approver); an
eligible Windows path for a Turkish entity recorded (Artifact Signing only through an eligible entity, otherwise an OV
certificate from a public CA on hardware), SmartScreen reputation plan (Store distribution, steady cadence); Android
upload key, Play App Signing and v3.1 rotation; Linux repository keys; Rekor v2 for package transparency.

<a id="p-29"></a>
**P-29. SUPPLY.4 (amend).** The double-build comparison covers packages: MSIX, MSI, Flatpak bundles, AppImages,
crdr v2 packs and update metadata, with normalised timestamps and ordering.

<a id="p-30"></a>
**P-30. SEC.1 (amend).** Threat models add the release pipeline, signing service, update channel and mod hub; playbooks
for key compromise (revoke, rotate the TUF root, re-sign, forced update, user notice), CRA SRP filing within 24 and 72
hours, and Turkish Law 7545 incident and vulnerability reporting readiness.

<a id="p-31"></a>
**P-31. CEIR-JIT.3 (amend).** AOT outputs are deterministic, signable and selected per target by SHIP.0's changeability
matrix; on JIT-forbidden targets the interpreter remains for content the policy permits, so live content changes and
AOT binaries coexist.

<a id="p-32"></a>
**P-32. CRASH-FIX.1 (amend).** Build identity in packs includes package hash, store, channel and rollout cohort.

<a id="p-33"></a>
**P-33. CRASH-FIX.2 (amend).** Exposes per-release, per-channel crash-free rates and regression signals as an API that
SHIP.3 rollouts and SHIP.8 consume; SHIP.1 uploads symbols to its store.

<a id="p-34"></a>
**P-34. OPS.1 (amend).** Publishes a data inventory (what is collected, consent tier, retention, transfers) that
COMPLY.2 uses for privacy labels and KVKK/GDPR notices; release health metrics feed SHIP.8.

<a id="p-35"></a>
**P-35. OPS.2 (amend).** Adds install, update, downgrade, rollback and uninstall matrices across N-1 to N-k releases
and every SHIP target, and save-game and project migration tests across releases.

<a id="p-36"></a>
**P-36. OPS.3 (amend).** Build and cook farm jobs (SHIP.1's cook cache scale-out) join the distributed job contract.

<a id="p-37"></a>
**P-37. EXT.4 (amend).** Package update metadata uses SHIP.0's TUF-class model shared with SHIP.3; a mod profile is
defined for SHIP.9; package payloads use crdr v2 when SHIP.2 lands.

<a id="p-38"></a>
**P-38. EXT.5 (amend, moved after SHIP.9).** The marketplace consumes SHIP.7 entitlements and payments and COMPLY.2
(DSA, consumer law, Turkish e-commerce and ETBIS, VAT); it no longer reads Later.

<a id="p-39"></a>
**P-39. 4.2d (amend).** `crd-net-steam` and the EOS adapter consume SHIP.6's SDK bindings; no second binding.

<a id="p-40"></a>
**P-40. NET-SERVER (amend).** Server builds ship as reproducible OCI images (distroless, signed with Cosign, SBOM
attached, provenance verified at deploy); fleet rollouts keep a compatibility window with client channels from SHIP.3;
Agones rolling updates run under NET-SIM.

<a id="p-41"></a>
**P-41. HW-RESILIENCE.1 (amend).** The same checksum family covers installers, patches and update payloads; repair
re-fetches only bad chunks.

<a id="p-42"></a>
**P-42. DOC.2 (amend).** Save-game and project migration across released versions and channels, including downgrade
policy (refuse or migrate back with a recorded loss report) and rollback after an update.

<a id="p-43"></a>
**P-43. API-STAB.0 (amend).** Content packs and mods declare the engine runtime ABI range they need; SHIP.3 and SHIP.9
refuse incompatible installs with a clear message.

<a id="p-44"></a>
**P-44. DATA-TRUST.2 (amend).** Publisher identity covers store packages and mod authors, with countersignatures.

<a id="p-45"></a>
**P-45. AGENT.3 (amend).** An authority class for irreversible external publication (store submit, rollout widen, price
change, deletion) that always requires recorded human approval; agents cannot obtain it from tool results or assets.

<a id="p-46"></a>
**P-46. ANTICHEAT (amend).** Records the boundary: integrity, encryption and optional anti-tamper of packages belong to
SHIP.2; cheat detection stays here; neither ships a kernel driver.

<a id="p-47"></a>
**P-47. I18N.1 and A11Y-GAME (amend).** I18N.1 exports store-listing strings and release notes per locale (Turkish
first-class); A11Y-GAME emits the feature facts behind Apple accessibility labels, Xbox accessibility tags and EAA
statements.

<a id="p-48"></a>
**P-48. DX.1 (amend).** `ceridc package` produces SHIP.1 and SHIP.2 outputs for the project's profiles; `publish` is
SHIP.10's.

<a id="p-49"></a>
**P-49. JOBS-PORT.2 (amend).** After ARM64 qualifies, SHIP.4 and SHIP.5 add Windows and Linux ARM64 packages with their
own clean-machine evidence.

<a id="p-50"></a>
**P-50. 6.0, 6.1 and 6.3 (amend).** No longer Later under the re-plan; SHIP-CONSOLE, SHIP-MOBILE and SHIP-XR follow
them.

<a id="p-51"></a>
**P-51. EMBED.2 (amend).** Embedded and kiosk deployments get A/B images and secure OTA with SHIP.3 metadata
(Uptane-class for vehicles), kiosk lockdown (Windows Assigned Access, Linux kiosk sessions).

<a id="p-52"></a>
**P-52. PORTFOLIO.1 (amend).** Acceptance projects ship through real store private branches and tracks via SHIP.

<a id="needs-a-download"></a>
## Needs a download (user approval)

Not downloaded. Sizes were not checked (that would need at least a HEAD request); each item needs separate user
approval before it is fetched.

| Item | Why | URL | Size |
|---|---|---|---|
| WiX Toolset v6 | SHIP.4 MSI peer and oracle | https://wixtoolset.org (K) | not checked |
| Windows App Certification Kit, MSIX Packaging Tool | SHIP.4 validation | https://learn.microsoft.com/windows/msix/ (K) | not checked |
| winget-create | SHIP.4 manifests | https://github.com/microsoft/winget-create (K) | not checked |
| Velopack (vpk) | SHIP.3/SHIP.4 updater peer | https://github.com/velopack/velopack (K) | not checked |
| Steamworks SDK and SteamCMD | SHIP.6 (needs a partner account) | https://partner.steamgames.com | not checked |
| Epic Online Services SDK, BuildPatch Tool | SHIP.6 | https://dev.epicgames.com (K) | not checked |
| GOG Galaxy SDK | SHIP.6 | https://devportal.gog.com (K) | not checked |
| itch.io butler | SHIP.6 | https://itch.io/docs/butler/ (K) | not checked |
| flatpak-builder, Flathub linter, appimagetool | SHIP.5 | https://docs.flathub.org (K) ; https://appimage.org (K) | not checked |
| Steam Linux Runtime 3 and 4 images | SHIP.5 targets | https://gitlab.steamos.cloud/steamrt (K) | not checked |
| bsdiff; Chromium Zucchini sources | SHIP.3 delta oracles | https://chromium.googlesource.com (K) | not checked |
| python-tuf or go-tuf | SHIP.3 metadata oracle | https://theupdateframework.io (K) | not checked |
| DirectStorage 1.4 SDK and GACL | SHIP.2 GPU decompression peer | https://devblogs.microsoft.com/directx/ | not checked |
| Oodle evaluation SDK (licence by request) | SHIP.2 measured peer only | https://www.radgametools.com/oodle.htm | not checked |
| fastlane | SHIP.8 automation peer | https://fastlane.tools (K) | not checked |
| Android SDK, bundletool | SHIP-MOBILE | https://developer.android.com (K) | not checked |
| Xcode 26 (on a Mac) | SHIP-MAC, SHIP-MOBILE | https://developer.apple.com | not checked |
| Sparkle 2 | SHIP-MAC updater peer | https://sparkle-project.org (K) | not checked |
| Unreal Engine 5.7/5.8, Unity 6, Godot 4.6 | Export and patch-size peers | https://www.unrealengine.com ; https://unity.com ; https://godotengine.org (K) | not checked |
| LicenseSpring or Cryptolens trial SDK | SHIP.7 licensing peer | https://www.licensespring.com ; https://cryptolens.io | not checked |
| OpenFeature SDK | SHIP.8 flag API peer | https://openfeature.dev (K) | not checked |

SBOM, provenance and signing tools (syft, CycloneDX tools, slsa-verifier, Cosign) are already listed by SUPPLY.1-3.

<a id="needs-the-user"></a>
## Needs the user (accounts, purchases, agreements)

| Action | For | Notes |
|---|---|---|
| Decide the publishing legal entity (Turkish company, individual, or also an EU entity) | Every store, signing, tax | Artifact Signing eligibility [S01](#s01); VAT and invoicing |
| Buy a code-signing certificate on hardware or a cloud HSM | SUPPLY.3, SHIP.4 | 460-day cap [S02](#s02); OV suffices for SmartScreen [S03](#s03) |
| Apple Developer Program, Developer ID certificates, App Store Connect API key | SHIP-MAC, SHIP-MOBILE | Mac hardware with Apple silicon |
| Google Play Console account and identity verification; Android developer verification | SHIP-MOBILE | [S07](#s07) |
| Steamworks partner account and app credits; Steam Deck (and later Steam Machine or Frame) hardware | SHIP.6, SHIP-XR | [S20](#s20) |
| Epic, GOG and itch.io developer accounts and agreements | SHIP.6 | [S19](#s19) |
| Microsoft Partner Center (Store) account | SHIP.4 | Free for individuals [S24](#s24) |
| ID@Xbox, PlayStation Partners, Nintendo Developer Portal, dev kits and NDAs | SHIP-CONSOLE | [S25](#s25) [S26](#s26) |
| Meta Horizon developer account | SHIP-XR | [S27](#s27) |
| Counsel review (Turkish and foreign): EULA, terms, privacy policy, KVKK notice and consent, distance-sales terms, loot-box design | COMPLY.2 | Not legal advice here |
| KVKK: VERBIS registration where required; standard-contract notifications for transfers abroad | COMPLY.2 | [S37](#s37) |
| ETBIS registration if selling on an own platform in Turkey; e-invoice/e-archive integrator | SHIP.7, COMPLY.2 | [S40](#s40) |
| Merchant-of-record or payment provider contracts | SHIP.7 | Cerid never stores card data |
| Tax registrations (Turkish KDV; EU OSS or a merchant of record; VAT-3 only for a non-resident seller into Turkey) | SHIP.7 | [S36](#s36) |
| CRA: a contact and an ENISA SRP account for products on the EU market | SEC.1 | [S49](#s49) |
| Export classification (EAR 5D992 self-classification; Apple export answers) | COMPLY.2 | [S54](#s54) |
| Age ratings: IARC through stores, regional boards where required | COMPLY.2 | [S13](#s13) [S41](#s41) |
| Local representative in Turkey if a Cerid-built game platform exceeds 100,000 daily accesses | COMPLY.2 | [S41](#s41) |
| Cloud KMS/HSM, CDN and update-server hosting accounts | SHIP.3, SUPPLY.3 | |
| Device farm accounts (Firebase Test Lab, AWS Device Farm) and test devices | SHIP.10, SHIP-MOBILE | |
| Trademark filings (TURKPATENT, EUIPO) for product names | Store listings | (K) |

<a id="uncertain-facts"></a>
## Uncertain facts

- Azure Artifact Signing's eligible countries beyond the USA, Canada, the EU and the UK, and whether Turkey will be
  added [S01](#s01).
- Whether Chromium's Zucchini replaced Courgette for Chrome updates (K, not confirmed today).
- Flathub payments and verified-developer status in 2026 (sources from 2021-2023).
- The outcome of the summer 2026 Epic v. Google evidentiary hearing [S11](#s11); the remanded Apple commission level
  [S09](#s09).
- The details of Apple's 2026-10-01 EU terms [S08](#s08) (secondary coverage).
- ICTA's secondary rules for Turkish game age ratings, and whether a storefront such as Steam counts as a "game
  platform" under 5651 [S41](#s41).
- The Constitutional Court ruling on Law 6563 Article 9(1) [S40](#s40) (single secondary source).
- Steam client 32-bit deprecation and current Steam Linux Runtime defaults [S23](#s23); current Deck Verified criteria
  text [S22](#s22).
- The current GDK version [S25](#s25); Meta Horizon Store changes in 2026 [S27](#s27).
- The Digital Fairness Act proposal date and content [S45](#s45).
- When Apple's accessibility labels become mandatory [S15](#s15).
- Texas App Store Accountability Act status after June 2026 [S14](#s14).
- The current wording of App Review Guideline 2.5.2 and the licence's interpreted-code clause [S17](#s17).
- Korea's 2025-2026 in-app payment enforcement [S55](#s55); Turkish Competition Authority decisions on Apple and Google
  [S42](#s42).
- Turkish VAT-3 filing and payment days [S36](#s36); Microsoft Store company registration fee [S24](#s24).
- Oodle's standalone licence terms [S29](#s29); Unreal 5.7 Zen and Horde issues [S33](#s33).

<a id="sources"></a>
## Sources

Read 2026-10-27 unless marked (K). Dates are publication dates as shown by the source or its coverage.

<a id="s01"></a>S01. DevClass, [Code signing Windows apps may be easier and more secure with new Azure Artifact service](https://devclass.com/2026/01/14/code-signing-windows-apps-may-be-easier-and-more-secure-with-new-azure-artifact-service/), 2026-01-14; Microsoft Learn, [Artifact Signing FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq).

<a id="s02"></a>S02. DigiCert, [Understanding the new code signing certificate validity change](https://www.digicert.com/blog/understanding-the-new-code-signing-certificate-validity-change); GlobalSign, [Code signing validity changes](https://www.globalsign.com/en/blog/code-signing-validity-changes) (CSC-31, 460 days from 2026-03-01).

<a id="s03"></a>S03. Microsoft Learn, [SmartScreen reputation](https://learn.microsoft.com/et-ee/windows/apps/package-and-deploy/smartscreen-reputation); ToDesktop, [EV certs do not grant immediate reputation anymore](https://www.todesktop.com/blog/posts/windows-apps-psa-ev-certs-do-not-grant-immediate-reputation-anymore); Advanced Installer, [Prevent SmartScreen from appearing](https://www.advancedinstaller.com/prevent-smartscreen-from-appearing.html).

<a id="s04"></a>S04. Gigazine, [macOS Tahoe will be the last OS to support Intel-based Macs, and Rosetta 2 will also end](https://wbgsv0a.gigazine.net/gsc_news/en/20250611-apple-macos-tahoe-26-intel-mac-support), 2025-06-11; MacTrast, [Apple phasing out Rosetta 2 with macOS 28](https://www.mactrast.com/2025/06/apple-phasing-out-rosetta-2-with-release-of-macos-28-as-intel-support-sunsets/amp/).

<a id="s05"></a>S05. Android Developers, [Meet Google Play's target API level requirement](https://developer.android.com/google/play/requirements/target-sdk); Play Console Help, [Target API level requirements](https://support.google.com/googleplay/android-developer/answer/11926878?hl=en-419).

<a id="s06"></a>S06. Android Developers, [Support 16 KB page sizes](https://developer.android.com/guide/practices/page-sizes?hl=en).

<a id="s07"></a>S07. Android Authority, [Android sideloading developer verification first wave rollout](https://androidauthority.com/android-sideloading-developer-verification-first-wave-rollout-3717921); Help Net Security, [Android developer verification requirement](https://www.helpnetsecurity.com/2026/03/31/android-developer-verification-requirement/), 2026-03-31; Android Developers, [Developer verification guides](https://developer.android.com/developer-verification/guides?authuser=3).

<a id="s08"></a>S08. Anadolu Agency, [Apple revamps EU App Store fees to resolve regulatory dispute](https://aa.com.tr/en/science-technology/apple-revamps-eu-app-store-fees-to-resolve-regulatory-dispute/4031035), 2026; TechCentral, [Apple adjusts App Store commissions in EU to comply with the DMA](https://www.techcentral.ie/apple-adjusts-app-store-commissions-in-eu-to-comply-with-digital-markets-act/); MacTech, [Apple announces changes to comply with the DMA](https://www.mactech.com/2025/06/26/apple-announces-lots-of-changes-to-comply-with-the-eus-digital-markets-act/amp/), 2025-06-26.

<a id="s09"></a>S09. Fenwick, [Ninth Circuit largely upholds ruling in Epic v. Apple](https://www.fenwick.com/insights/publications/ninth-circuit-largely-upholds-ruling-in-epic-v-apple); Perkins Coie, [Epic v. Apple: Ninth Circuit weighs](https://perkinscoie.com/insights/update/epic-v-apple-ninth-circuit-weighs).

<a id="s10"></a>S10. CSIS, [Starting up the competition: Japan's Mobile Software Act](https://www.csis.org/blogs/charting-geoeconomics/starting-competition-japans-mobile-software-act); KOMOJU, [Japan's new Mobile Software Competition Act](https://en.komoju.com/blog/general-advice/japan-payment-law-2025/).

<a id="s11"></a>S11. Stash, [Epic v. Google settlement update April 2026](https://www.stash.gg/blog/blog-epic-v-google-settlement-update-april-2026); Video Games Chronicle, [Judge postpones approval of Epic vs Google settlement](https://www.videogameschronicle.com/news/judge-postpones-approval-of-epic-vs-google-settlement-wonders-why-theyre-suddenly-bffs).

<a id="s12"></a>S12. Apple Developer, [SDK minimum requirements](https://developer.apple.com/news/upcoming-requirements/) (in force since 2026-04-28).

<a id="s13"></a>S13. 9to5Mac, [Apple notifies developers of new App Store age rating system](https://9to5mac.com/2025/07/24/apple-notifies-developers-of-new-app-store-age-rating-system/), 2025-07-24.

<a id="s14"></a>S14. 9to5Mac, [Texas judge blocks state app store age verification law](https://9to5mac.com/2025/12/23/texas-judge-blocks-state-app-store-age-verification-law/), 2025-12-23; Gigazine, [Apple App Store age verification in Texas](https://gigazine.net/gsc_news/en/20260604-apple-app-store-age-verification-texas/), 2026-06-04 (secondary).

<a id="s15"></a>S15. Apple, [Overview of Accessibility Nutrition Labels](https://developer.apple.com/help/app-store-connect/manage-app-accessibility/overview-of-accessibility-nutrition-labels).

<a id="s16"></a>S16. PPC Land, [Apple announces updated App Store privacy requirements](https://ppc.land/apple-announces-updated-app-store-privacy-requirements-for-developers/); Apple Developer Forums, [Privacy manifest and SDK signature requirements](https://developer.apple.com/forums/thread/743329).

<a id="s17"></a>S17. MediaNama, [Apple App Review Guideline 2.5.2 and the Anythng removal](https://www.medianama.com/2026/04/223-apple-vibe-coding-app-ban-anythng/), 2026-04; Expo forums, [Expo Updates and Apple's App Store Review Guidelines](https://forums.expo.dev/t/expo-updates-and-apples-app-store-review-guidelines/64495) (quotes guideline and licence text; uncertain currency).

<a id="s18"></a>S18. Google Play Console Help, [Device and Network Abuse](https://support.google.com/googleplay/android-developer/answer/9888379?hl=en).

<a id="s19"></a>S19. Epic Games Store, [New Epic Games Store webshops and revenue share update](https://store.epicgames.com/news/new-epic-games-store-webshops-and-revenue-share-update), 2025-05.

<a id="s20"></a>S20. Gigazine, [Steam Machine and Steam Frame summer 2026 release](https://gigazine.net/gsc_news/en/20260605-steam-machine-frame-2026-summer-release/), 2026-06-05; TweakTown, [Valve reveals Steam Machine launch window](https://tweaktown.com/news/112037/valve-reveals-steam-machine-launch-window-confirms-easy-compatibility-with-steam-deck-verified-games/index.html).

<a id="s21"></a>S21. Valve, [Steamworks: partial depot update](https://partner.steamgames.com/doc/sdk/updating/partial_depot) and [local content server](https://partner.steamgames.com/doc/sdk/uploading/local_content_server); Heathen, [Steam deploy guide](https://kb.heathen.group/steam/deploy) (secondary).

<a id="s22"></a>S22. GamingOnLinux, [Steam Deck hits over 9000 games rated Verified and Playable](https://www.gamingonlinux.com/2023/05/steam-deck-hits-over-9000-games-rated-verified-and-playable/), 2023-05 (criteria summary; older).

<a id="s23"></a>S23. GamingOnLinux, [Valve put up a new Steam Linux Runtime 4.0 with a move towards 64-bit](https://www.gamingonlinux.com/2025/11/valve-put-up-a-new-steam-linux-runtime-4-0-with-a-move-towards-64-bit), 2025-11.

<a id="s24"></a>S24. Windows Developer Blog, [Free developer registration for individual developers on Microsoft Store](https://blogs.windows.com/windowsdeveloper/2025/09/10/free-developer-registration-for-individual-developers-on-microsoft-store/), 2025-09-10; Neowin, [Microsoft Store now lets individual developers join for free](https://www.neowin.net/news/microsoft-store-now-lets-individual-developers-join-for-free/).

<a id="s25"></a>S25. Microsoft, [GDK on GitHub](https://www.github.com/microsoft/GDK); Microsoft Game Dev, [Building Xbox games with Godot](https://developer.microsoft.com/en-us/games/articles/2026/06/building-xbox-games-with-godot/), 2026-06; GameFromScratch, [Microsoft release Game Development Kit on GitHub](https://gamefromscratch.com/microsoft-release-game-development-kit-on-github/).

<a id="s26"></a>S26. Nintendo, [Register as a Nintendo developer](https://developer.nintendo.com/register); MCV/Develop, [Nintendo merges developer programmes into single portal](https://www.mcvuk.com/business-news/publishing/nintendo-merges-developer-programmes-into-single-portal-to-attract-indies/).

<a id="s27"></a>S27. Meta, [A more open ecosystem for developers](https://developers.meta.com/horizon/blog/a-more-open-ecosystem-for-developers/), 2024; Meta, [Get your app ready for the Meta Horizon Store](https://developers.meta.com/horizon/blog/get-apps-ready-app-lab-meta-horizon-store-meta-quest-developers/).

<a id="s28"></a>S28. Microsoft DirectX blog, [DirectStorage 1.4 release adds support for Zstandard](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/), 2026-03-11; Tom's Hardware, [Microsoft debuts DirectStorage 1.4 at GDC 2026](https://tomshardware.com/video-games/pc-gaming/microsoft-debuts-directstorage-1-4-at-gdc-2026-with-zstandard-compression-and-gacl-update-promises-developers-improved-compression-ratios-faster-loading-and-more).

<a id="s29"></a>S29. Epic, [Oodle now free to use in Unreal Engine via GitHub](https://www.unrealengine.com/en-US/blog/oodle-now-free-to-use-in-unreal-engine-via-github); RAD Game Tools, [Oodle](https://www.radgametools.com/oodle.htm).

<a id="s30"></a>S30. Velopack, [repository README](https://github.com/aarnott/velopack); NuGet, [vpk](https://packages.nuget.org/packages/vpk).

<a id="s31"></a>S31. Chromium, [Software updates: Courgette](https://www.chromium.org/developers/design-documents/software-updates-courgette).

<a id="s32"></a>S32. Sigstore, [Rekor v2 GA](https://blog.sigstore.dev/rekor-v2-ga/), 2025-10-10.

<a id="s33"></a>S33. Epic, [Using Zen Storage Server as cooked output store](https://dev.epicgames.com/documentation/unreal-engine/using-zen-storage-server-as-cooked-output-store-for-unreal-engine?lang=ko) and [Cooked data snapshots with Zen](https://dev.epicgames.com/documentation/ja-jp/unreal-engine/cooked-data-snapshots-with-zen-storage-server-for-unreal-engine); Unreal forums, [Zen oplog staging failure in 5.7](https://forums.unrealengine.com/t/staging-step-failed-sending-oplog-request-to-zen-when-using-buildcookrun-cook-stage-together-have-a-workaround-but-is-this-a-bug-or-a-misconfiguration/2731838) and [cooked data snapshots with incremental cook](https://forums.unrealengine.com/t/using-cooked-data-snapshots-with-incremental-cook/2720288/2).

<a id="s34"></a>S34. Godot, [Compiling with PCK encryption key (4.6)](https://docs.godotengine.org/en/4.6/engine_details/development/compiling/compiling_with_script_encryption_key.html).

<a id="s35"></a>S35. EY, [Turkiye revises digital service tax rate for 2026 and 2027](https://globaltaxnews.ey.com/news/2026-0117-turkiye-revises-digital-service-tax-rate-for-2026-and-2027), 2026-01-06; Bloomberg Tax, [Turkey gazettes decision setting digital services tax rate](https://news.bloombergtax.com/daily-tax-report-international/turkey-gazettes-decision-setting-digital-services-tax-rate-for-2026-2027).

<a id="s36"></a>S36. Grant Thornton, [Indirect tax guide: Turkey](https://www.grantthornton.global/en/insights/indirect-tax-guide/indirect-tax---turkey/); International Tax Review, [Turkey: new VAT liability for non-resident electronic service providers](https://internationaltaxreview.com/article/2a68x2a369zalyfkolzpc/turkey-new-vat-liability-for-non-resident-electronic-service-providers).

<a id="s37"></a>S37. Prighter, [By-Law on transfer of personal data abroad, Article 14](https://prighter.com/resources/laws/turkish-kvkk/by-laws/the-procedures-and-principles-for-the-transfer-of-personal-data-abroad/articles/article-14); Erdem & Erdem, [Key considerations for standard contracts](https://erdem-erdem.av.tr/en/insights/public-announcement-on-key-considerations-for-standard-contracts-to-be-used-in-the-cross-border-transfer-of-personal-data).

<a id="s38"></a>S38. Mondaq, [Turkish Cybersecurity Law enters into force](https://www.mondaq.com/turkey/security/1713402/turkish-cybersecurity-law-enters-into-force); Koksal, [Cybersecurity Law No. 7545](https://koksal.av.tr/en/legislation/cybersecurity-law-no-7545/).

<a id="s39"></a>S39. Mondaq, [Announcement on the regulation amending the Regulation on Distance Contracts](https://www.mondaq.com/turkey/consumer-law/1629892/announcement-on-the-regulation-amending-the-regulation-on-distance-contracts), 2025; Mondaq, [Distance sales contracts under Turkish law 2026](https://www.mondaq.com/distance-sales-contracts-under-turkish-law-2026/1846208).

<a id="s40"></a>S40. Mondaq, [Recent developments in Turkish e-commerce law](https://webiis10.mondaq.com/turkey/advertising-marketing-branding/1718440/recent-developments-in-turkish-e-commerce-law); CMS, [E-commerce in CEE: Turkey](https://cms.law/en/int/expert-guides/ecommerce-in-cee/turkey).

<a id="s41"></a>S41. CMS, [Turkiye amends Internet Law with new framework for digital game platforms](https://cms.law/en/tur/legal-updates/tuerkiye-s-amends-internet-law-with-new-framework-for-digital-game-platforms-and-enhanced-obligations-for-social-network-providers), 2026-05-12 (fetched); Mondaq, [New online safety and gaming platform rules](https://www.mondaq.com/turkey/gaming/1796742/new-online-safety-and-gaming-platform-rules); Gurkaynak, [A new era for children and gaming in Turkey](https://www.gurkaynak.av.tr/Content/dosya/3969/a-new-era-for-children-and-gaming-in-turkey-understanding-law-no-5651-s-2026-am.pdf).

<a id="s42"></a>S42. Digital Policy Alert, [Competition Authority investigation into Apple over App Store payment systems](https://digitalpolicyalert.org/event/20453-announced-competition-authoritys-investigation-into-apple-over-its-app-store-payment-systems-alleged-violation-of-law-on-the-protection-of-competition), 2024; Global Competition Review, [Turkey launches eighth antitrust probe into Google](https://globalcompetitionreview.com/article/turkey-launches-eighth-antitrust-probe-google), 2025.

<a id="s43"></a>S43. Demarest, [ECA Digital enters into force on 17 March 2026](https://www.demarest.com.br/en/eca-digital-entrada-em-vigor-em-17-de-marco-de-2026/); Mayer Brown, [Enforcement of Brazil's ECA Digital](https://www.mayerbrown.com/ja/insights/publications/2026/04/enforcement-of-brazils-eca-digital-introduces-new-obligations-for-companies), 2026-04.

<a id="s44"></a>S44. Reed Smith, [Q&As on the EU consumer protection authorities' joint guidance paper](https://www.reedsmith.com/en/perspectives/2025/03/qas-on-the-eu-consumer-protection-authorities-joint-guidance-paper), 2025-03; Baker McKenzie, [CPC key principles on in-game virtual currencies](https://connectontech.bakermckenzie.com/european-consumer-protection-network-issues-new-key-principles-on-in-game-virtual-currencies-impact-for-gaming-and-gambling-entities-in-belgium-the-eu-and-beyond/).

<a id="s45"></a>S45. Lewis Silkin, [European Commission issues responses to digital fairness consultation](https://www.lewissilkin.com/insights/2026/01/14/european-commission-issues-responses-to-digital-fairness-consultation-102m1s7), 2026-01-14; Heuking, [Digital Fairness Act](https://www.heuking.de/en/news-events/newsletter-articles/detail/digital-fairness-act-implementation-obligations-companies-need-to-prepare-for.html), 2026-05-08.

<a id="s46"></a>S46. Reed Smith, [EU Product Liability Directive: software, digital products, cybersecurity](https://www.reedsmith.com/en/perspectives/2025/07/eu-product-liability-directive-software-digital-products-cybersecurity), 2025-07; Gibson Dunn, [EU Product Liability Directive: responding to software, AI and complex supply chains](https://www.gibsondunn.com/eu-product-liability-directive-responding-to-software-ai-and-complex-supply-chains), 2026-03.

<a id="s47"></a>S47. Goodwin, [Not delayed, not deferred: EU AI Act transparency obligations are now in force](https://www.goodwinlaw.com/en/insights/publications/2026/08/alerts-technology-dpc-eu-ai-act-transparency-obligations-now-in-force), 2026-08; Ropes & Gray, [Operationalising the EU AI Act's transparency obligations](https://www.ropesgray.com/en/insights/viewpoints/2026/08/102nfqm/you-talkin-to-me-operationalising-the-eu-ai-acts-transparency-obligations), 2026-08.

<a id="s48"></a>S48. Osborne Clarke, [EU Accessibility Act: time to start implementation projects](https://www.osborneclarke.com/insights/eu-accessibility-act-time-start-implementation-projects-now); Accessible.org, [EAA e-commerce services requirements](https://accessible.org/eaa-ecommerce-services-requirements/) (secondary).

<a id="s49"></a>S49. ENISA, [The CRA Single Reporting Platform is launched](https://www.enisa.europa.eu/index.php/news/the-cra-single-reporting-platform-is-launched); McCann FitzGerald, [CRA reporting obligations apply from 11 September 2026](https://www.mccannfitzgerald.com/knowledge/data-privacy-and-cyber-risk/cyber-resilience-act-reporting-obligations-apply-from-11-september-2026). Earlier summary: [Part 3 C25](2026-10-26-building-with-cerid-2026-comparison.md#c25).

<a id="s50"></a>S50. Federal Register, [Children's Online Privacy Protection Rule](https://www.federalregister.gov/documents/2025/04/22/2025-05904/childrens-online-privacy-protection-rule), 2025-04-22.

<a id="s51"></a>S51. Wiggin, [Age assurance in games: UK legal requirements](https://wiggin.co.uk/insight/age-assurance-in-games-a-guide-to-uk-legal-requirements-recent-enforcement-and-next-steps-for-the-sector/), 2026-05-19; Cooley, [The Online Safety Act: child access duties for the gaming industry](https://www.cooley.com/news/insight/2025/2025-04-09-the-online-safety-act-child-access-duties-for-the-gaming-industry), 2025-04-09.

<a id="s52"></a>S52. Promise Legal, [The Digital Services Act and your game](https://blog.promise.legal/eu-digital-services-act-game-studios/) (secondary); Morrison Foerster, [The EU Digital Services Act](https://www.mofo.com/resources/insights/220920-the-eu-digital-services-act-europes-new-regime).

<a id="s53"></a>S53. Tech Insider, [Cyberpunk 2077 Switch 2 mods on six platforms](https://tech-insider.org/cyberpunk-2077-switch-2-mods-6-platforms-2026/), 2026-10.

<a id="s54"></a>S54. DLA Piper, [Latest regulatory changes reduce burden for software and technology companies](https://www.dlapiper.com/en/insights/publications/2021/04/latest-regulatory-changes-reduce-burden-for-software-and-technology-companies), 2021-04; BIS, [Encryption decision flowchart](https://www.bis.gov/media/documents/new-v2-flowchart2.pdf).

<a id="s55"></a>S55. KED Global, [Apple, Google may face fine over in-app billing in Korea](https://www.kedglobal.com/newsView/ked202310060012), 2023-10-06.

<a id="s56"></a>S56. LicenseSpring, [What is a floating license](https://www.licensespring.com/blog/glossary/what-is-a-floating-license/); Cryptolens, [Floating licenses in an offline setting](https://cryptolens.io/2024/05/floating-licenses-in-an-offline-setting); Revenera, [InstallShield 2026 activation user guide](https://docs.revenera.com/installshield/pdf/InstallShield2026_ActivationUserGuide.pdf).

<a id="s57"></a>S57. Flathub Discourse, [Seeking contractors for work on Flathub](https://discourse.flathub.org/t/seeking-contractors-for-work-on-flathub/1889); GamingOnLinux, [Flathub seeks funding to add payments, donations and subscriptions](https://gamingonlinux.com/2023/02/flathub-seeks-funding-to-add-payments-donations-and-subscriptions), 2023-02 (older).

Prior-knowledge (K) references not re-read today: Unreal BuildGraph, UAT, IoStore and the Zen loader; Unity Build
Profiles, Addressables and Build Automation; Electron Forge and electron-builder; Tauri 2 bundler and updater; Flutter
build; Qt Installer Framework; Bazel remote cache; Incredibuild; FASTBuild; MSIX, App Installer and sparse packages;
MSI and WiX; winget; Windows 10 end of support (2025-10-14); Smart App Control; RFC 3161; The Update Framework;
Uptane; Sparkle 2; Omaha; bsdiff; Zucchini; zstd `--patch-from`; FastCDC; casync and desync; Steam and Epic chunking;
App Store phased release; Play staged rollouts; Firebase Remote Config; LaunchDarkly; OpenFeature; Play Asset
Delivery; Play Integrity; APK Signature Scheme v3.1 and v4; Play App Signing; StoreKit 2; Play Billing; GOG Galaxy;
itch.io butler; ID@Xbox and Xbox Requirements; PlayStation Partners and TRC; Switch 2 launch; IARC, PEGI, ESRB, USK,
CERO and GRAC; Belgium's loot-box position; Google Play Data safety; VERBIS thresholds; CVAA; Xbox Accessibility
Guidelines; Apple export compliance questions; Wassenaar participation of Turkey; Turkish e-invoice and e-archive;
EU VAT OSS; Paddle, FastSpring and Lemon Squeezy; FlexNet, RLM and Keygen; fastlane; Crashlytics, Sentry and
Backtrace; CurseForge and Nexus; Agones; Intune, Configuration Manager, Group Policy and Jamf; Windows Assigned
Access; Denuvo; TURKPATENT and EUIPO. Re-pin each before use.
