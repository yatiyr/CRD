# NET.2: RFC 9850 key logging, compiled out of shipping

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-03. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 64). Contract: [key logging, decision
S19](../design/network-foundation.md#key-logging). Everything ran on this PC; no cloud machine ran (0 hours, 0 USD).

## CI first

Complete-tier dispatch 41508851661 (13c815d3): ten jobs failed, 18 were still running when this batch started. All
ten were read: `crd-repository-tools` (the missing `scripts/fuzz.py` key-store entry) on every test lane and the LTO
link of crd-crypto-tests (`test_key_store.cpp` `-Wnull-dereference`) on linux-gcc-release and -relwithdebinfo, the two
batch-62 defects b5a2d1b9 fixed. Push run 41520934339 (57c51e55) contains the fix and was pending; no new dispatch.

## What landed

- **The sink** (`TlsConfig::key_log`, `key_log_user`, `key_log_exporters`; `tls_key_log_compiled()`): the provider's
  RFC 9850 lines for every session and QUIC handshake of a context; the exporter secrets only with `key_log_exporters`.
- **Compiled out of shipping**: `CRD_TLS_KEY_LOG` (crd-crypto, private, from the shipping condition); in shipping the
  provider's key log functions are poisoned in tls.cpp (GCC poison, read by clang too; MSVC C4995 under /WX; only the
  GCC arm ran here, the MSVC and clang-cl shipping lanes are CI's) and a sink is `BadConfig`. crd-crypto-tests gets
  `CRD_TEST_EXPECT_TLS_KEY_LOG` from the same condition and checks it on every lane.
- **Interop**: both peers report their key logs; `tls_interop.py` requires the RFC labels at both ends, equal lines,
  one client random per connection and each secret the suite's hash length. OpenSSL's own `*_TRAFFIC_SECRET_N`
  lines (after a KeyUpdate, outside the registry) are set aside; AWS-LC never logs EARLY_EXPORTER_SECRET or ECH's.
- **Register**: `tls.keylog` implemented (owner crypto, rows NET.2 and NET-DEBUG).

## Evidence

- linux-gcc-debug: `[key-log]` 6 cases, 879 assertions; `[tls],[quic],[early],[ech],[crypto]` 258 cases, 1,446,669
  assertions. The early-data case first failed on its own fixture (a store reused across iterations resumed the first
  connection); fixed in the test.
- linux-gcc-public-checks: `crd-crypto-tls-interop-openssl` 79 of 79, every established case comparing both key logs.
  It first failed five key-update cases on OpenSSL's `CLIENT_TRAFFIC_SECRET_N` lines, now set aside by name.
- linux-gcc-shipping (LTO): `[key-log]` 1 passed (compiled false, every refusal), 5 skipped; the TLS family 72 passed.
- win-debug (MSVC): `[key-log]` 6 cases, 877 assertions; the TLS family 77 cases. linux-gcc-asan: `[key-log]` 6 cases, 877 assertions.
- Teeth, each caught: the exporter filter dropped (TLS and QUIC key log cases fail); a stray
  `SSL_CTX_set_keylog_callback` outside the gate (shipping: "attempt to use poisoned"; debug compiles).
- `wsl-lane.sh dev linux-gcc-debug --target crd-crypto-tests`: 313 CTests and LLVM-20 tidy, clean after three test-code
  fixes. Validators: master-plan, repository, docs, Allman braces, generated, registers, capability owners.

## Next

NET.2's remaining clauses in its row: TPM 2.0, PIV over PKCS#11, the first policy generation, the Tamarin or ProVerif
model, ARM64 assembly and prefixed provider symbols, the boards against OpenSSL and BoringSSL.
