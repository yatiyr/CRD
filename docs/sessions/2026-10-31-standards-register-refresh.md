# Register review of 2026-10-31 (light scan, standards register)

<!-- doc-role: record -->

Sources: 63 planned, 63 read on 2026-10-31; the register changes below were applied. This review edits no ROADMAP row: the amendments are proposals for the user.

## Register changes

- quic.transport: QUIC Versions QUIC version 1 = 0x00000001 confirmed by IANA (an unnamed record citing RFC 9000)
- quic.datagram: QUIC Transport Parameters max_datagram_frame_size = 0x20 confirmed by IANA
- quic.grease-bit: QUIC Transport Parameters grease_quic_bit = 0x2ab2 confirmed by IANA
- quic.version-negotiation: QUIC Transport Parameters version_information = 0x11 confirmed by IANA
- quic.v2: QUIC Versions QUIC version 2 = 0x6b3343cf confirmed by IANA (an unnamed record citing RFC 9369)
- http.h3: TLS ALPN Protocol IDs HTTP/3 = h3 confirmed by IANA
- webtransport.h3: draft-ietf-webtrans-http3-16 posted 2026-07-06 (-16 posted 2026-07)
- webtransport.h2: draft-ietf-webtrans-http2-15 posted 2026-07-06 (no revision recorded)
- quic.reliable-reset: draft-ietf-quic-reliable-stream-reset-11 posted 2026-09-06 (-11)
- quic.ack-frequency: draft-ietf-quic-ack-frequency-14 posted 2026-02-05 (-14)
- quic.receive-timestamps: draft-ietf-quic-receive-ts-03 posted 2026-07-20 (no revision recorded)
- quic.nat-traversal: draft-bruynooghe-n0-quic-nat-traversal-00 posted 2026-07-06 (-00 posted 2026-07)
- masque.connect-udp-listen: draft-ietf-masque-connect-udp-listen is rfc-editor (was iesg)
- tls.v13: RFC 8446 is obsoleted by RFC 9846
- tls.hybrid-kex: TLS Supported Groups X25519MLKEM768 = 0x11EC confirmed by IANA
- tls.hybrid-kex: TLS Supported Groups SecP256r1MLKEM768 = 0x11EB confirmed by IANA
- tls.hybrid-kex: TLS Supported Groups SecP384r1MLKEM1024 = 0x11ED confirmed by IANA
- crypto.ecdh: TLS Supported Groups x25519 = 0x001D confirmed by IANA
- crypto.ecdh: TLS Supported Groups secp256r1 = 0x0017 confirmed by IANA
- crypto.ecdh: TLS Supported Groups secp384r1 = 0x0018 confirmed by IANA
- tls.ech: TLS ExtensionType Values encrypted_client_hello = 0xfe0d confirmed by IANA
- tls.certificate-compression: TLS ExtensionType Values compress_certificate = 27 confirmed by IANA
- crypto.eddsa: TLS SignatureScheme ed25519 = 0x0807 confirmed by IANA
- crypto.ecdsa: TLS SignatureScheme ecdsa_secp256r1_sha256 = 0x0403 confirmed by IANA
- crypto.ecdsa: TLS SignatureScheme ecdsa_secp384r1_sha384 = 0x0503 confirmed by IANA
- crypto.aes-gcm: AEAD Algorithms AEAD_AES_128_GCM = 1 confirmed by IANA
- crypto.aes-gcm: AEAD Algorithms AEAD_AES_256_GCM = 2 confirmed by IANA
- crypto.chacha20-poly1305: AEAD Algorithms AEAD_CHACHA20_POLY1305 = 29 confirmed by IANA
- pki.merkle-tree-certificates: draft-ietf-plants-merkle-tree-certs-07 posted 2026-10-07 (-00 posted 2026)
- media.rtp-over-quic: draft-ietf-avtcore-rtp-over-quic-14 posted 2025-03-21 (-14)
- media.rtp-over-quic: draft-ietf-avtcore-rtp-over-quic is expired (was active)
- media.moq-transport: draft-ietf-moq-transport-22 posted 2026-10-01 (-20 posted 2026-08-31)
- capture.qlog: draft-ietf-quic-qlog-main-schema-14 posted 2026-07-06 (-14 posted 2026-07)
- capture.pcapng: draft-ietf-opsawg-pcapng-06 posted 2026-09-19 (no revision recorded)

## Read in the consolidated text

None.

## Proposed ROADMAP amendments

- masque.connect-udp-listen: the watched draft-ietf-masque-connect-udp-listen is now rfc-editor (was iesg); decide whether NET-REACH adopts it
- tls.v13 (NET.2): the pinned RFC 8446 is now obsoleted by RFC 9846 (was current); amend the rows that implement it before the full re-research this triggers
- media.rtp-over-quic: the watched draft-ietf-avtcore-rtp-over-quic is now expired (was active); decide whether NET-LIVE adopts it

## Triggers a full re-research

- tls.v13

## Sources not read

None.

## Read by hand

- the Object identifiers values (https://csrc.nist.gov/projects/computer-security-objects-register/algorithm-registration) for pki.ml-dsa
- the status of FIPS 204 on the NIST CSRC publications list (https://csrc.nist.gov/publications) for crypto.ml-dsa
- the status of FIPS 186-5 on the NIST CSRC publications list (https://csrc.nist.gov/publications) for crypto.ecdsa
- the status of NIST IR 8547 ipd on the NIST CSRC publications list (https://csrc.nist.gov/publications) for nist.ir-8547
- the status of https://www.w3.org/TR/trace-context/ on its W3C Technical Reports page (https://www.w3.org/TR/trace-context/) for ops.trace-context
- confirm the calendar events marked unverified and add the next IETF meetings (https://www.ietf.org/meeting/upcoming/)

## Records read

Retrieved on 2026-10-31 by `scripts/register-review.py fetch`.

| Source | Bytes | SHA-256 |
|---|---:|---|
| [draft-bruynooghe-n0-quic-nat-traversal](https://datatracker.ietf.org/doc/draft-bruynooghe-n0-quic-nat-traversal/doc.json) | 1176 | `0b510d15d707da27986851f7d567242d3418b8562af90de9307e9ed41ad4fc03` |
| [draft-ietf-avtcore-rtp-over-quic](https://datatracker.ietf.org/doc/draft-ietf-avtcore-rtp-over-quic/doc.json) | 5754 | `6a66d994f88d1860a7d938334f39d40f3bedaf3f49daa204961ca6933ce4f7da` |
| [draft-ietf-masque-connect-udp-listen](https://datatracker.ietf.org/doc/draft-ietf-masque-connect-udp-listen/doc.json) | 6009 | `d43fcb7ec3cda17f98cebdb17a81d75004c0d33fcaad0a48cd5935f44d75058a` |
| [draft-ietf-mlcodec-opus-dred](https://datatracker.ietf.org/doc/draft-ietf-mlcodec-opus-dred/doc.json) | 3833 | `9de24d1e9a9c69a17051ad4e68b0f3c31f82d5eb52cc7e5c9fc56e786acef402` |
| [draft-ietf-moq-transport](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/doc.json) | 7126 | `2436ca6ac5b724cd879d396c9cbf441f852fae7323bb9248e177e30b72b5293e` |
| [draft-ietf-opsawg-pcapng](https://datatracker.ietf.org/doc/draft-ietf-opsawg-pcapng/doc.json) | 3947 | `658a1ca3c24d713b34392e3598ff5e33421e3585d9cdc318f3aa6a8f94303bc5` |
| [draft-ietf-plants-merkle-tree-certs](https://datatracker.ietf.org/doc/draft-ietf-plants-merkle-tree-certs/doc.json) | 6041 | `155ff6fd18bad9e0ed5312ac9166e42223b955c4dc0551a2c3976d26b0749f27` |
| [draft-ietf-quic-ack-frequency](https://datatracker.ietf.org/doc/draft-ietf-quic-ack-frequency/doc.json) | 5213 | `644be7a21107cc12e6bca1f680ffeca8d908a43988c3c4c3d0ac18e5f0ddf400` |
| [draft-ietf-quic-address-discovery](https://datatracker.ietf.org/doc/draft-ietf-quic-address-discovery/doc.json) | 2733 | `5c120935a2aed960d30878f8238750533bc40a8c156b87ae1aa8f7050eb9cf4f` |
| [draft-ietf-quic-load-balancers](https://datatracker.ietf.org/doc/draft-ietf-quic-load-balancers/doc.json) | 7888 | `3e5b9e9a73a51d1a5a5b6313f97157700688cc750c17eedad7fcb86bca8840a2` |
| [draft-ietf-quic-multipath](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/doc.json) | 9634 | `536e6d0c67ca4d076fa174bb36ee515c8da56aaf225b882825a64c7f43890bb8` |
| [draft-ietf-quic-qlog-main-schema](https://datatracker.ietf.org/doc/draft-ietf-quic-qlog-main-schema/doc.json) | 5956 | `e5bf08171c132eab088efe49fa865a1185e0efae4a3decc10d046132eaae4d47` |
| [draft-ietf-quic-receive-ts](https://datatracker.ietf.org/doc/draft-ietf-quic-receive-ts/doc.json) | 2634 | `3ec57ac8a4d8151a7196a22b3f5e3bdba9c023daf1b573e08f49612d49a71233` |
| [draft-ietf-quic-reliable-stream-reset](https://datatracker.ietf.org/doc/draft-ietf-quic-reliable-stream-reset/doc.json) | 4762 | `21a6351980b2a643ca17a7074410066d51c325c434542e059e30c71f3223d62b` |
| [draft-ietf-webtrans-http2](https://datatracker.ietf.org/doc/draft-ietf-webtrans-http2/doc.json) | 5364 | `67b7ea0e536214d3841626a3081c186a21125f85a1e12fa06f62fc9e0da7e76d` |
| [draft-ietf-webtrans-http3](https://datatracker.ietf.org/doc/draft-ietf-webtrans-http3/doc.json) | 5509 | `0a0c8d8f8e1badfabb68818f01f3532610dfa974712d1f8f694e1d6c798ab422` |
| [iana-aead-parameters](https://www.iana.org/assignments/aead-parameters/aead-parameters.xml) | 6637 | `a9ac0404fafeaf11abdb5dc5e35dc6705b885e8e88df92042b4f341f992965c6` |
| [iana-quic](https://www.iana.org/assignments/quic/quic.xml) | 40575 | `eeed8e306919bd2bd87e7a8aea59a9724f66570cd33f2e4e4fccfb9a2c5a54e7` |
| [iana-tls-extensiontype-values](https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xml) | 50444 | `90df531dd7dc03dbff3490badde4b863f4bd927990dbb6062301e837df7a443d` |
| [iana-tls-parameters](https://www.iana.org/assignments/tls-parameters/tls-parameters.xml) | 310949 | `e2e80a91e7d3f3657e13bb549abe3ad96be26566f99de6a3c005048d1f04909c` |
| [rfc10024](https://www.rfc-editor.org/rfc/rfc10024.json) | 1050 | `0f630eda182bc7d77248806cd49e2e59cf8f909bbd98d39748e537f965f7c248` |
| [rfc2198](https://www.rfc-editor.org/rfc/rfc2198.json) | 873 | `3e35e9e4eb9f381a6f9eff717a72eb317ca0fe20a8626777863a7ec5bbd457ff` |
| [rfc5116](https://www.rfc-editor.org/rfc/rfc5116.json) | 1095 | `97b0f70b75be2801ae0fdd89b91404b07c5f316a4eaf19e746e972d99573bade` |
| [rfc5869](https://www.rfc-editor.org/rfc/rfc5869.json) | 1073 | `71bb0ebeb164b9f9ec583b21f8d72e3254822db3403eda7253a2e4d0e9e74bc3` |
| [rfc6330](https://www.rfc-editor.org/rfc/rfc6330.json) | 1829 | `b93922266278344edaa9bc1fa728a805c25248a6e304ec1f6556230223105241` |
| [rfc6455](https://www.rfc-editor.org/rfc/rfc6455.json) | 1369 | `4d50383f2d5687b5d462ad263026e7de939b25a816718980ce969d039edcc25a` |
| [rfc7696](https://www.rfc-editor.org/rfc/rfc7696.json) | 1045 | `77861b99206532ed95b91c06aed865cf0aa924877bb4ea9b87f057a4c5aee42f` |
| [rfc7748](https://www.rfc-editor.org/rfc/rfc7748.json) | 990 | `530dd9936682ff91683103476dea6afa9a029f97dd665428b28a2c25f696ab1d` |
| [rfc8032](https://www.rfc-editor.org/rfc/rfc8032.json) | 917 | `9e2673f051b554b22da6339851b56934b0a07a1b137ef14ba6d414a92c60e7bf` |
| [rfc8439](https://www.rfc-editor.org/rfc/rfc8439.json) | 1174 | `b8ded502cb348b16dfbaabad0e9d15c045b82b5ebd6ba92e447ecc3500e96e7f` |
| [rfc8441](https://www.rfc-editor.org/rfc/rfc8441.json) | 738 | `bba60d93fa22d867bcb97efb7882a317a506471b3f7f285a35fbc5849f896484` |
| [rfc8445](https://www.rfc-editor.org/rfc/rfc8445.json) | 1091 | `e65ee259325d3d9c41a7933b35e523a826347d9e86cc0c020eb9af561f12f2e7` |
| [rfc8446](https://www.rfc-editor.org/rfc/rfc8446.json) | 1217 | `e437bff192a0402fbbb392fea3b4c3b5e846cc3717735c91f6b494d1264b0620` |
| [rfc8489](https://www.rfc-editor.org/rfc/rfc8489.json) | 1312 | `6982fbf0d06449651c6254f6ff2981008961f54f9ba9c28312f044b8560b2c5c` |
| [rfc8627](https://www.rfc-editor.org/rfc/rfc8627.json) | 1950 | `e188a714541411921645c2a4ca3ee45468dd2f8c47b3138c85e76aa136e8c738` |
| [rfc8656](https://www.rfc-editor.org/rfc/rfc8656.json) | 1659 | `57550dfdf5ff5a2f100b2a0f29f87b902d30705ef675db49cfdffc9d2ab473f1` |
| [rfc8701](https://www.rfc-editor.org/rfc/rfc8701.json) | 904 | `a982a178d28a79619dba8397c91fbb4ebbba4c4c377a6e760bd94497b773c18e` |
| [rfc8838](https://www.rfc-editor.org/rfc/rfc8838.json) | 1083 | `29b56a8caf8ebbfc9bfafe5afc0ab2fdd427ae4a9807562d43695a22693c6910` |
| [rfc8879](https://www.rfc-editor.org/rfc/rfc8879.json) | 895 | `f30eb5b0a337363e467d711323c2468f360fe234d470474d9c83eb7579c9eea7` |
| [rfc8899](https://www.rfc-editor.org/rfc/rfc8899.json) | 1856 | `176bad10f934b7a2e56e80d15c6cf7e88229d68533de5fdc0773f82aafea6b3f` |
| [rfc8999](https://www.rfc-editor.org/rfc/rfc8999.json) | 791 | `cd4498bd75d3bb315fe52e0e675d785eed539f8b8e9dcdfd1cd4cc4f2d7a1824` |
| [rfc9000](https://www.rfc-editor.org/rfc/rfc9000.json) | 1271 | `b07c5f9659d8584f8732e38b1ed16adb5b8d9b581d68f53c7bcc0013a0e37c66` |
| [rfc9001](https://www.rfc-editor.org/rfc/rfc9001.json) | 741 | `dc401bc634cd40d4bfcbde1274442f7a3d8a6a1cea75e82a3d7912142545605a` |
| [rfc9002](https://www.rfc-editor.org/rfc/rfc9002.json) | 822 | `b7f295cda30478ad64f7c489c569bc2f0dd5e9da0c1ef6071bf33bbfc7535369` |
| [rfc9114](https://www.rfc-editor.org/rfc/rfc9114.json) | 1018 | `55305a8b6c40928d433e9875a7c870903bc6a3c7d36139573947b8578658ad4a` |
| [rfc9204](https://www.rfc-editor.org/rfc/rfc9204.json) | 946 | `37b8321639b41cb928e58392572362e512a5c919e9a4e70c740d1188e89b6c84` |
| [rfc9220](https://www.rfc-editor.org/rfc/rfc9220.json) | 837 | `19a73492042f5c1f5e30ebdf885b946986603f2b79fd26044a014002c4ee8bae` |
| [rfc9221](https://www.rfc-editor.org/rfc/rfc9221.json) | 731 | `ae9321e44bb1b9349b7abb7753b735a9f4b5a493d0c471410c0caee5808ab6f8` |
| [rfc9287](https://www.rfc-editor.org/rfc/rfc9287.json) | 704 | `22b481d794055ed85c5dd88399fca33ec69c75d56b837c02cf31a49d71bc7cad` |
| [rfc9297](https://www.rfc-editor.org/rfc/rfc9297.json) | 1269 | `9770d815185aad9a7908c21635f25cdc3c32ae8f2caf4ed2ef47720c74285492` |
| [rfc9298](https://www.rfc-editor.org/rfc/rfc9298.json) | 1061 | `c8eff8d422b609edbbadcf9ec5f0a103f79a8fa45068fc1254cb096ccfdb3523` |
| [rfc9330](https://www.rfc-editor.org/rfc/rfc9330.json) | 2159 | `22ac9ff17756026c444ba57dea9ea92828da44bdc3a0fea35c903dcbd625de0d` |
| [rfc9331](https://www.rfc-editor.org/rfc/rfc9331.json) | 2582 | `f1cb79b08895417c2ee5abe8e03ec200eb976caeea17baaec456f109eb419c36` |
| [rfc9368](https://www.rfc-editor.org/rfc/rfc9368.json) | 1227 | `799455bffa6d076757b1261769d0483e30de7d14d4413412271e4d99b6eff14c` |
| [rfc9369](https://www.rfc-editor.org/rfc/rfc9369.json) | 1088 | `76f0b629346e9d23bf4fa99e5185c77b4f51157111958f0e0089cdf5c89ede5e` |
| [rfc9420](https://www.rfc-editor.org/rfc/rfc9420.json) | 1329 | `7b6ea69d39144c5dda5158e8847aed3b9ae4c0578c0323f6f0bbe0151fa5ae0f` |
| [rfc9484](https://www.rfc-editor.org/rfc/rfc9484.json) | 1242 | `0ed72a785d8a5e98dbe957a2bc5734987d9e6cb5a3bc3ec2beab341af5162bc1` |
| [rfc9605](https://www.rfc-editor.org/rfc/rfc9605.json) | 1351 | `e3dcaac3a019f64166c803eb6c79d091cc4690b26a1cdb001d9d2e4507e8b205` |
| [rfc9725](https://www.rfc-editor.org/rfc/rfc9725.json) | 889 | `905d9603d7e6665453ac130b4b444cc5d07de9169d3e24660c77cde509e4013b` |
| [rfc9750](https://www.rfc-editor.org/rfc/rfc9750.json) | 1929 | `bbcde598ca84a0e6aa3041c74ee775a9fabbbe88abfae26679f1eb9f0cc6e68d` |
| [rfc9849](https://www.rfc-editor.org/rfc/rfc9849.json) | 720 | `01282d529dfe0e7c699e86dcc413a6e46ef4582ed1de3ecc094549a053eba7e5` |
| [rfc9850](https://www.rfc-editor.org/rfc/rfc9850.json) | 993 | `25ae0f25114c52e87b3036dea967f592d7d5e0e83ea13f9710b7b691fb72cfe0` |
| [rfc9881](https://www.rfc-editor.org/rfc/rfc9881.json) | 1244 | `150b928cacbfeb9c68aaf8001a984227f3acbf998b36f7a05a169a09c364d737` |
