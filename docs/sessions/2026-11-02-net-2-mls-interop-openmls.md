# NET.2: MLS interoperability with OpenMLS

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 61; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [MLS, M3 layer 10g, decisions M4 and
M13](../design/network-foundation.md#mls); [pinned inputs](../design/pinned-inputs.md#acquisition).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!": Cerid's MLS now runs groups with OpenMLS too, and a rule of RFC 9420 section 10 it had
  left unchecked is enforced.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane); no cloud machine ran (0 hours, 0 USD).
- Testing downloads are pre-approved (2026-11-01): Rust 1.99.1's rustc, rust-std and Cargo archives from
  static.rust-lang.org (SHA-256 and sizes matched against the 2026-10-15 stable channel manifest) and OpenMLS 0.9.2's
  crates from crates.io (each SHA-256 in the committed Cargo.lock).

## CI first

Push run 41462446381 (d7371489) failed `linux-gcc-release` and `linux-gcc-relwithdebinfo` at Build. These are this
track's: GCC 13's link-time `-Wnull-dereference` and `-Warray-bounds` in five LTO links (`crd-crypto-tests` and the
`mls`, `mls-commit`, `mls-group` and `mls-tree` fuzz targets). The cause is three nullable lookups dereferenced on
paths GCC cannot rule out:
- `HpkeKey::expand` and `decapsulate` dereferenced `find_kem` unchecked (HPKE's post-quantum KEMs, d03a9582). They
  now refuse a KEM outside the profile (Unsupported) and a key holding none (NotReady).
- `Group::reinit` looked the ReInit's suite up twice and dereferenced the second lookup (ede7306e). It now looks it
  up once.
- `Group::join` dereferenced `prior` whenever the Welcome's usage was a resumption. In the fuzz target's call, IPA-CP
  had cloned it with `prior` a constant null. `OpenedWelcome` now carries the group whose resumption secret it used
  (`resumed()`), and join consults only that.

Each fix makes the code state its invariant; nothing was suppressed. The local reproduction and the fixed links are
under Evidence. The scheduled run 41429790527 (4f30bb98) had 28 of 30 jobs passed, 2 running and none failed.

## What landed

- **The pinned Rust toolchain** (`tools/rust-linux-x64`, [install-rust.py](../../scripts/install-rust.py)): rustc,
  rust-std and Cargo 1.99.1 unpacked into one prefix. `pins.install_tree` gained a part's `top` (an installer
  archive's top directory: only the component directory below it is unpacked, its licences go to `licenses/`, its
  script and metadata stay out) and a pin's `omit` (the per-component `manifest.in`, which would clash). Both
  `rustc --version` and `cargo --version` must name the pin.
- **The crate set pinned by its lockfile** (`tools/openmls-crates`): `check-pins.py` checks that every locked package
  comes from crates.io with a 64-hex checksum, that the manifest requires `openmls = "=0.9.2"`, and that the lockfile
  resolves it. The workflow must install Rust through the helper, naming the version; `linux-gcc-public-checks`
  installs it and turns on `CRD_BUILD_MLS_VS_OPENMLS`. The license manifest names the lockfile.
- **`crd-openmls-peer`** ([main.rs](../../tests/foundation/crypto/interop/openmls_peer/src/main.rs)): OpenMLS 0.9.2
  over its RustCrypto provider, speaking `crd-mls-peer`'s protocol. Each member has its own provider, since OpenMLS
  keys a group's state by its id. The outgoing handshake format is set before each proposal and commit, and both
  formats are read. A resync joiner takes over its former member's signer, because OpenMLS removes the leaf of the
  joiner's own signature key. Built by `cargo build --locked` inside the tree, with the peer's warnings as errors.
- **The driver** runs Cerid against each peer given (`--mlspp`, `--openmls`, `--involving`). The suites are per peer
  (OpenMLS's RustCrypto provider has no P-384 outside its post-quantum feature). The reader now checks section 10 on
  every key package. The `crd-crypto-mls-interop-openmls` CTest has 18 cases.
- **Section 10 enforced in Cerid**: "Extensions included in the extensions or leaf_node.extensions fields MUST be
  included in the leaf_node.capabilities field". `check_key_package` now refuses a package extension of a type the
  leaf's capabilities do not list (Unsupported), GREASE types included, as OpenMLS does. Without it, a Cerid committer
  could add a package that OpenMLS members refuse, and the group would split on the Add. Three creators broke the
  rule, and each was fixed on the spot:
  - the interop peer and the unit tests' members drew a fresh GREASE type for the package extension (now the listed
    one);
  - MLSpp's packages break it by construction (TP-11 below).
  A new unit case covers the rule: unlisted types refused, a listed GREASE type and a default type accepted.
- **The peers' GREASE option**: `member ... [GREASE]` with 0 makes a member with no GREASE leaf or package extension.
  The capabilities' GREASE values stay. Every member of a group with OpenMLS is made this way (TP-10).

<a id="tp-10"></a>
## TP-10: OpenMLS refuses the GREASE extensions a leaf lists

OpenMLS decodes a GREASE extension's type as `ExtensionType::Unknown(v)` and the same codepoint in capabilities as
`ExtensionType::Grease(v)`. `contains_extensions` compares them as different, so every leaf or key package carrying a
listed GREASE extension is refused. That includes OpenMLS's own: a member given one cannot create a group
(`ExtensionsNotInCapabilities`). Registered as [TP-10](../third-party-defects.md#tp-10) and let be. In each suite,
`grease_extensions_refused` reproduces it, adds the same Cerid member without the extensions, and has that member
commit an OpenMLS member. Because MLSpp puts a GREASE extension on every leaf, MLSpp and OpenMLS can never share a
group, so the planned groups of all three implementations were dropped.

<a id="tp-11"></a>
## TP-11: MLSpp leaves its key package's GREASE extensions unlisted

MLSpp's `KeyPackage` constructor adds a GREASE extension of a random type after its leaf is signed, so section 10's
MUST holds only by chance. Registered as [TP-11](../third-party-defects.md#tp-11). The MLSpp peer retypes each such
extension to a listed GREASE type and signs again through MLSpp's public fields and `sign`, and it also returns the
package as MLSpp made it. `unlisted_package_extensions` reads one of those off the wire with an unlisted type, has a
Cerid committer refuse it as `unsupported`, then adds the adapted package of the same member.

## Evidence (this PC, linux-gcc-public-checks through the track's WSL lane)

- `crd-crypto-mls-interop-openmls`: 18 of 18 cases. A traced run made 16,275 requests and 5,052 refusals: 2,301 by
  Cerid as `forged` and 2,538 by OpenMLS, chiefly `ValidationError` and `InvalidCommit`, with the reproduction's
  `UnsupportedExtension` and `ExtensionsNotInCapabilities`. 2,277 messages were processed.
- `crd-crypto-mls-interop-mlspp`: 28 of 28 cases (TP-11's four new).
- `crd-crypto-tls-interop-openssl`: passed.
- `crd-crypto-tests`: 238 cases, 1,436,384 assertions. The 66 MLS cases also pass under ASan and UBSan
  (linux-gcc-asan).
- One source mutation (the section 10 check disabled) was caught by the unit case (2 assertions) and by all four TP-11
  cases.
- Every crypto CTest, the fuzz corpora replays and the CTest guards
  (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`): 43 of 43, on the whole lane tree.
- The CI failure on the track's new `linux-gcc-release` lane (GCC 13.3, as hosted):
  - With the fixes, the five links pass, `crd-crypto-tests` passes there (238 cases) and the five MLS fuzz corpora
    replay clean.
  - The pre-fix sources restored as a control fail the same five links with the hosted run's exact warnings.
- `tidy-files.py` (LLVM 20.1.0) on the changed C++ (`hpke.cpp`, `mls_commit.cpp`, `mls_group.cpp`, both C++ peers,
  `test_mls_group.cpp`) is clean after two findings in the MLSpp peer's new helpers were fixed (`std::ranges::any_of`).
- `cargo clippy -- -D warnings` is clean after three findings fixed (`is_multiple_of`, two clones of a `Copy` type).
- The repository tool tests have two new cases: the Rust layout with `top` and `omit` and its refusals, and the
  lockfile pin's checks.
- Validators: pins, repository, Allman braces, generated sources, environment reads and registers (`mls.protocol`
  implemented; the crypto inventory regenerated and cerid-package's classification restated).

## Next

NET.2's next clause is the key store: a software fallback through DPAPI, the Keychain and the Secret Service, then
TPM 2.0, the Secure Enclave, Android Keystore and YubiKey-class keys. The first step is to read each platform's
interface and choose how keys are wrapped and named, with no secret ever outside a sealed form.
