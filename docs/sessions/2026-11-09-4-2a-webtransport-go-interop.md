# 2026-11-09 — 4.2a: WebTransport interop with webtransport-go

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S59](../design/network-foundation.md#webtransport).

CI first: run 42617196650 (879fc9a3) was read for its failed jobs. `linux-gcc-shipping`, `-release` and `-relwithdebinfo` failed at link time on a null dereference in `fuzz_http3.cpp`, fixed in 33c99ba8; the rest fail in hesap-special (`airy.hpp`, `bessel.hpp`), the other owner's code. Tidy ran here on the owed files and found one issue in `test_quic_connection.cpp`, fixed in 184e54be.

## What landed

- `Http3Connection` sends SETTINGS_WT_ENABLED under -17's 0x20b8933b and -16's 0x2c7cf000 and reads either (draft section 7.1). The selection 7.1 describes changes nothing here: S59 records the differences.
- `crd-webtransport-go-peer` (`tests/foundation/net/interop/webtransport_go_peer`): webtransport-go 0.13.0 over quic-go 0.62.0, the exchange of `interop_webtransport.hpp` in Go. `go.sum` is the pin (`tools/webtransport-go-modules`), `check-pins.py` validates it, and `crd_add_go_peer` runs `go vet` and `go build` with `-mod=readonly`, `GOSUMDB=off` and `GOTOOLCHAIN=local`.
- `scripts/install-go.py` and `tools/go-linux-x64`: Go 1.27.3 from go.dev, the SHA-256 go.dev publishes, retrieved 2026-11-09. Wired into `linux-gcc-public-checks` (`ci.yml` and `ci/pipeline.py`).
- `crd-net-webtransport-interop-go` (label `webtransport-interop`, `CRD_BUILD_WEBTRANSPORT_VS_GO`). `webtransport_interop.py` takes `--go`.

## Evidence

- `crd-net-webtransport-interop-go`: 30 of 30 cases on linux-gcc-public-checks in 60 s, three rounds, each both ways round over real UDP on `webtransport-h3`: no body, 16x64 KiB, 4x3 MiB, 16 unidirectional, datagrams, 8 resets over RESET_STREAM_AT, 8 stops, a mix closed by the server, negotiation, Origin read and refused, 403 and 404, the lossy path, and **flow control against a second implementation** under a 64 KiB and a 128 KiB window.
- `crd-net-tests` `[http3],[qpack],[webtransport]`: 170 cases, 44,605 assertions, pass. `crd-net-webtransport-interop-quinn`, `crd-net-http3-interop-nghttp3`, `-quiche` and `crd-net-qmux-interop-crate` pass after the change. `crd-fuzz-net-http3` and `crd-fuzz-net-webtransport` link on linux-gcc-release (the lane that failed).
- Teeth, each failing and passing once restored: -16 not sent (the unit case, and a Go client refuses the server); -16 not read (the unit case, and Cerid's client never opens a session on the Go server).

## Not asked of webtransport-go, and why

The legacy spelling (its client never sends it, and its server advertises legacy sessions under 0x14e9cd29, not the 0xc671706a Cerid reads; browsers' settings are confirmed under WEB.5). The exporter (none offered). 0-RTT (no session opens in it). 64 datagrams into a Go client: quic-go's HTTP/3 layer keeps 32 for a session nobody reads yet and drops the rest, so the shape is capped at 32 against Go; a Cerid client still receives all 64 of its burst.

## Downloads

Go 1.27.3 and the module set of `go.sum`, from go.dev and proxy.golang.org: testing tools, pre-approved and pinned.

## Next

The session's channel surface (S59 "Channels"), then WebSocket over HTTP/1.1, HTTP/2 with HPACK and RFC 8441.
