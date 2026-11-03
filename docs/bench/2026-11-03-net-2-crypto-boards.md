# 2026-11-03 — NET.2: crd-crypto against AWS-LC, BoringSSL and OpenSSL 4.0

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [P9](../design/network-foundation.md#provider).
Session: [crypto boards](../sessions/2026-11-03-net-2-crypto-boards.md).

## Machine and arms

- Intel Core i9-14900K (AVX2, VAES, VPCLMULQDQ; no AVX-512), WSL2 Ubuntu 24.04 (kernel 6.6.87.2), one P-core thread
  (`taskset -c 6`), GCC 13.3.0, the `linux-gcc-release` tree (-O3, link-time optimisation for crd-crypto) with
  `-DCRD_BUILD_CRYPTO_VS_PEERS=ON`.
- Arms, all in one process (`crd-crypto-board`): **cerid** (crd-crypto over AWS-LC 5.12.0, assembly on); **aws-lc** (the
  same AWS-LC called directly: crd-crypto's own cost is the gap to it); **boringssl** (0.20261020.0, its CMake Release,
  prefixed `crd_bssl`); **openssl** (4.0.3, its Configure defaults). CPU level line: AWS-LC and BoringSSL dispatch on
  their own capability checks (no Cerid kernel level applies).
- Work: AEAD over 64, 1350 and 16384 bytes; one X25519 key pair and agreement; ML-KEM-768 key generation, encapsulation
  to an encoded key, decapsulation; signing and verifying a 128-byte message from the encoded public key. TLS 1.3: one
  group, AES-128-GCM, a self-signed Ed25519 server key the client pins, one ticket per connection, no certificate
  compression, both ends in the process, each record moved through the caller's buffer; `tls-full` and `tls-resume`
  are one connection to its end (the client reads its new ticket), `tls-record` one record client to server.
- Before timing, every arm's AEAD bytes match crd-crypto's, ML-KEM secrets agree across all 16 pairs, all 48 signature
  pairs verify (tampered ones refused), and every TLS profile negotiates its group, AES-128-GCM and resumption.

## Harness

```
python3 scripts/ab-board.py --cmd board="$B/tests/crypto/crd-crypto-board" \
  --arm cerid='board:^cerid (?P<key>\S+) (?P<value>\S+)$' \
  --arm aws-lc='board:^aws-lc (?P<key>\S+) (?P<value>\S+)$' \
  --arm boringssl='board:^boringssl (?P<key>\S+) (?P<value>\S+)$' \
  --arm openssl='board:^openssl (?P<key>\S+) (?P<value>\S+)$' \
  --reference cerid --cpus 6 --warmup 1 --min-reps 5 --max-reps 9 --log-dir ~/crypto-board/logs
```

Host before and after (`build-bench.py host`): Linux 30.2 of 31.2 GiB free, no other load; Windows 15.2 of 63.7 GiB
free, 0.4 cores of other load (contended by memory only, `--allow-contended` policy).

## Board (nanoseconds per operation; ratio above 1: the arm is faster than crd-crypto)

9 repetitions (the maximum of 9 repetitions), 1 warm-up, CPUs 6, band 2%; speed ratio = reference time / arm time, median [bootstrap 95%].

aws-lc against cerid:

| key | cerid | aws-lc | speed ratio | verdict |
|---|---|---|---|---|
| aead-seal/aes-128-gcm/64 | 57.3 | 56.6 | 1.014 [0.9983-1.027] | unresolved |
| aead-seal/aes-128-gcm/1350 | 186.6 | 184.3 | 1.016 [1.004-1.025] | unresolved |
| aead-seal/aes-128-gcm/16384 | 1564 | 1565 | 0.9978 [0.9919-1.003] | level |
| aead-seal/aes-256-gcm/64 | 64.4 | 62.6 | 1.021 [1.008-1.042] | unresolved |
| aead-seal/aes-256-gcm/1350 | 214.4 | 212.4 | 1.011 [1.002-1.014] | level |
| aead-seal/aes-256-gcm/16384 | 1863 | 1854 | 1.004 [1.001-1.012] | level |
| aead-seal/chacha20-poly1305/64 | 106.8 | 106 | 1.006 [1.005-1.011] | level |
| aead-seal/chacha20-poly1305/1350 | 509.7 | 510 | 1.001 [0.9994-1.008] | level |
| aead-seal/chacha20-poly1305/16384 | 4590 | 4607 | 1.002 [0.9908-1.005] | level |
| aead-open/aes-128-gcm/64 | 56.8 | 55.6 | 1.022 [0.9644-1.036] | unresolved |
| aead-open/aes-128-gcm/1350 | 182.4 | 182.7 | 0.9978 [0.9947-1.006] | level |
| aead-open/aes-128-gcm/16384 | 1560 | 1555 | 1.001 [0.9928-1.004] | level |
| aead-open/aes-256-gcm/64 | 59.3 | 57.9 | 1.028 [1.01-1.035] | unresolved |
| aead-open/aes-256-gcm/1350 | 209.8 | 210.9 | 1.001 [0.9694-1.009] | unresolved |
| aead-open/aes-256-gcm/16384 | 1824 | 1820 | 1 [0.9877-1.007] | level |
| aead-open/chacha20-poly1305/64 | 107.6 | 107.1 | 1.005 [0.9936-1.008] | level |
| aead-open/chacha20-poly1305/1350 | 473.9 | 476.4 | 0.9992 [0.9902-1.008] | level |
| aead-open/chacha20-poly1305/16384 | 4541 | 4554 | 0.9988 [0.9948-1.002] | level |
| x25519/agree | 19136 | 19104 | 1.004 [0.9975-1.012] | level |
| ml-kem-768/keygen | 8286 | 8280 | 1.001 [0.9923-1.005] | level |
| ml-kem-768/encapsulate | 8876 | 8724 | 1.018 [1.006-1.042] | unresolved |
| ml-kem-768/decapsulate | 9203 | 9204 | 1.005 [0.999-1.024] | unresolved |
| ed25519/sign | 5140 | 5170 | 0.9967 [0.9891-1.001] | level |
| ed25519/verify | 21315 | 21336 | 1.001 [0.9948-1.009] | level |
| ecdsa-p256/sign | 11255 | 11268 | 1.003 [0.9925-1.01] | level |
| ecdsa-p256/verify | 34115 | 33901 | 1.005 [0.9997-1.01] | level |
| ml-dsa-65/sign | 93698 | 93891 | 1.028 [0.9614-1.037] | unresolved |
| ml-dsa-65/verify | 27608 | 27451 | 1.004 [0.9952-1.017] | level |
| tls-full/x25519 | 109855 | 107130 | 1.028 [1.018-1.038] | unresolved |
| tls-full/x25519mlkem768 | 141642 | 141250 | 1.01 [0.9923-1.015] | level |
| tls-resume/x25519mlkem768 | 120933 | 109709 | 1.102 [1.098-1.108] | faster |
| tls-record/256 | 497.3 | 455.2 | 1.098 [1.086-1.118] | faster |
| tls-record/16384 | 4243 | 4224 | 1.008 [0.9966-1.013] | level |

boringssl against cerid:

| key | cerid | boringssl | speed ratio | verdict |
|---|---|---|---|---|
| aead-seal/aes-128-gcm/64 | 57.3 | 47.2 | 1.216 [1.21-1.317] | faster |
| aead-seal/aes-128-gcm/1350 | 186.6 | 134.9 | 1.373 [1.358-1.419] | faster |
| aead-seal/aes-128-gcm/16384 | 1564 | 940.7 | 1.664 [1.656-1.666] | faster |
| aead-seal/aes-256-gcm/64 | 64.4 | 51.9 | 1.25 [1.232-1.313] | faster |
| aead-seal/aes-256-gcm/1350 | 214.4 | 154.7 | 1.392 [1.379-1.402] | faster |
| aead-seal/aes-256-gcm/16384 | 1863 | 1117 | 1.661 [1.647-1.681] | faster |
| aead-seal/chacha20-poly1305/64 | 106.8 | 109.3 | 0.9762 [0.9734-0.9828] | unresolved |
| aead-seal/chacha20-poly1305/1350 | 509.7 | 511.4 | 0.9996 [0.996-1.006] | level |
| aead-seal/chacha20-poly1305/16384 | 4590 | 4616 | 1.003 [0.988-1.004] | level |
| aead-open/aes-128-gcm/64 | 56.8 | 46.3 | 1.227 [1.219-1.235] | faster |
| aead-open/aes-128-gcm/1350 | 182.4 | 122.1 | 1.496 [1.483-1.502] | faster |
| aead-open/aes-128-gcm/16384 | 1560 | 884 | 1.764 [1.755-1.776] | faster |
| aead-open/aes-256-gcm/64 | 59.3 | 50.8 | 1.175 [1.16-1.178] | faster |
| aead-open/aes-256-gcm/1350 | 209.8 | 146.4 | 1.439 [1.421-1.45] | faster |
| aead-open/aes-256-gcm/16384 | 1824 | 1063 | 1.723 [1.702-1.735] | faster |
| aead-open/chacha20-poly1305/64 | 107.6 | 107.7 | 0.9944 [0.9926-1.006] | level |
| aead-open/chacha20-poly1305/1350 | 473.9 | 476.6 | 0.999 [0.9904-1] | level |
| aead-open/chacha20-poly1305/16384 | 4541 | 4544 | 0.9973 [0.992-1.002] | level |
| x25519/agree | 19136 | 35172 | 0.545 [0.5417-0.5508] | slower |
| ml-kem-768/keygen | 8286 | 15936 | 0.5215 [0.5198-0.5225] | slower |
| ml-kem-768/encapsulate | 8876 | 18273 | 0.485 [0.4822-0.4912] | slower |
| ml-kem-768/decapsulate | 9203 | 15691 | 0.5898 [0.5852-0.6003] | slower |
| ed25519/sign | 5140 | 13733 | 0.3758 [0.3739-0.3771] | slower |
| ed25519/verify | 21315 | 24342 | 0.8719 [0.8683-0.8895] | slower |
| ecdsa-p256/sign | 11255 | 13040 | 0.8679 [0.8565-0.8749] | slower |
| ecdsa-p256/verify | 34115 | 34098 | 1.002 [0.9942-1.007] | level |
| ml-dsa-65/sign | 93698 | 167836 | 0.5559 [0.5305-0.5989] | slower |
| ml-dsa-65/verify | 27608 | 40833 | 0.6765 [0.6653-0.6782] | slower |
| tls-full/x25519 | 109855 | 145645 | 0.7515 [0.746-0.7598] | slower |
| tls-full/x25519mlkem768 | 141642 | 202986 | 0.6992 [0.6977-0.7011] | slower |
| tls-resume/x25519mlkem768 | 120933 | 160753 | 0.7476 [0.7404-0.7555] | slower |
| tls-record/256 | 497.3 | 392.6 | 1.265 [1.252-1.283] | faster |
| tls-record/16384 | 4243 | 2970 | 1.436 [1.422-1.454] | faster |

openssl against cerid:

| key | cerid | openssl | speed ratio | verdict |
|---|---|---|---|---|
| aead-seal/aes-128-gcm/64 | 57.3 | 65.6 | 0.876 [0.8725-0.9309] | slower |
| aead-seal/aes-128-gcm/1350 | 186.6 | 185.8 | 1.002 [0.8674-1.023] | unresolved |
| aead-seal/aes-128-gcm/16384 | 1564 | 1589 | 0.9844 [0.9789-0.99] | unresolved |
| aead-seal/aes-256-gcm/64 | 64.4 | 70.8 | 0.9007 [0.8784-0.9552] | slower |
| aead-seal/aes-256-gcm/1350 | 214.4 | 212.4 | 1.011 [0.9926-1.026] | unresolved |
| aead-seal/aes-256-gcm/16384 | 1863 | 1869 | 1.002 [0.9167-1.005] | unresolved |
| aead-seal/chacha20-poly1305/64 | 106.8 | 175.7 | 0.6097 [0.605-0.6126] | slower |
| aead-seal/chacha20-poly1305/1350 | 509.7 | 653.7 | 0.779 [0.7744-0.7833] | slower |
| aead-seal/chacha20-poly1305/16384 | 4590 | 5058 | 0.9075 [0.9049-0.915] | slower |
| aead-open/aes-128-gcm/64 | 56.8 | 59.6 | 0.9513 [0.9225-0.9595] | slower |
| aead-open/aes-128-gcm/1350 | 182.4 | 180 | 1.016 [1.013-1.024] | unresolved |
| aead-open/aes-128-gcm/16384 | 1560 | 1578 | 0.9943 [0.8054-0.9976] | unresolved |
| aead-open/aes-256-gcm/64 | 59.3 | 64.4 | 0.9208 [0.9164-0.9279] | slower |
| aead-open/aes-256-gcm/1350 | 209.8 | 208.3 | 1.011 [0.9358-1.017] | unresolved |
| aead-open/aes-256-gcm/16384 | 1824 | 1833 | 0.9989 [0.9906-1] | level |
| aead-open/chacha20-poly1305/64 | 107.6 | 180.3 | 0.5974 [0.5915-0.6011] | slower |
| aead-open/chacha20-poly1305/1350 | 473.9 | 648.1 | 0.7383 [0.7285-0.7407] | slower |
| aead-open/chacha20-poly1305/16384 | 4541 | 5055 | 0.9007 [0.8905-0.9055] | slower |
| x25519/agree | 19136 | 37947 | 0.5058 [0.4992-0.5096] | slower |
| ml-kem-768/keygen | 8286 | 21492 | 0.3851 [0.3818-0.3873] | slower |
| ml-kem-768/encapsulate | 8876 | 21206 | 0.4192 [0.4172-0.4269] | slower |
| ml-kem-768/decapsulate | 9203 | 17683 | 0.5215 [0.5202-0.5272] | slower |
| ed25519/sign | 5140 | 18129 | 0.2846 [0.2823-0.2855] | slower |
| ed25519/verify | 21315 | 57586 | 0.3707 [0.3661-0.3731] | slower |
| ecdsa-p256/sign | 11255 | 13728 | 0.8227 [0.8169-0.825] | slower |
| ecdsa-p256/verify | 34115 | 46543 | 0.7334 [0.7265-0.7357] | slower |
| ml-dsa-65/sign | 93698 | 488131 | 0.1932 [0.1726-0.2117] | slower |
| ml-dsa-65/verify | 27608 | 97938 | 0.2818 [0.2801-0.2833] | slower |
| tls-full/x25519 | 109855 | 257112 | 0.4276 [0.4224-0.4365] | slower |
| tls-full/x25519mlkem768 | 141642 | 335593 | 0.4231 [0.4141-0.4256] | slower |
| tls-resume/x25519mlkem768 | 120933 | 245829 | 0.494 [0.4859-0.5] | slower |
| tls-record/256 | 497.3 | 557.8 | 0.8927 [0.8869-0.9036] | slower |
| tls-record/16384 | 4243 | 4802 | 0.8848 [0.8795-0.8919] | slower |

## Verdict

crd-crypto beats BoringSSL by 1.15x to 2.7x on X25519, ML-KEM-768, Ed25519, ECDSA P-256 signing and ML-DSA-65 (ECDSA
verification level) and on every TLS handshake (1.33x full X25519, 1.43x full X25519MLKEM768, 1.34x resumption); it
beats OpenSSL 4.0 on every case but AES-GCM above 64 bytes (level), its records by 1.12x and 1.13x. Against the AWS-LC
it wraps every primitive, the full handshakes and the 16 KiB records are level or within 3% (unresolved at the 2%
band: the rounds' spread). Gaps:

- **AES-GCM against BoringSSL** (1.2x to 1.8x, and the TLS records with it): the provider's. BoringSSL runs its
  VAES-AVX2 AES-GCM (`aes-gcm-avx2-x86_64`) on this AVX2-only CPU; AWS-LC 5.12.0 has VAES only in its AVX-512 code
  (`aesni-gcm-avx512`) and runs the AES-NI path here (crd-crypto and direct AWS-LC are level). ChaCha20-Poly1305 is level.
- **Resumption against direct AWS-LC** (1.10x): crd-crypto's. Its client ticket store holds bytes (decision S9), so a
  ticket is serialised when kept and parsed with its certificate when offered; the direct arm keeps the live session.
- **256-byte records against direct AWS-LC** (1.10x, about 40 ns a record): crd-crypto's. The session's per-call
  bounds (outgoing bytes, the key-update budget of decision S7) and four provider calls around each record.

Fixed on the way (this board found them): the error scope cleared an empty error queue on every call (80 ns a record),
and the pin check parsed the peer's leaf certificate a second time on every handshake.
