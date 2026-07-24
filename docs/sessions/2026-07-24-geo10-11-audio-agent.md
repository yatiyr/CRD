# 2026-07-24 — GEO-10 + GEO-11: the DAW substrate and the agent surface (D-007 rows 75-76) — **GEO BAND CLOSED**

Two slices closed in one session; the GEO band (rows 66-76) is complete.

## GEO-10 — crd-audio (the DAW substrate)

Our own audio codecs, resources, offline graph, MIDI, and realtime layer — zero third-party in the product path.

- **Codecs → `AudioPcm`** (the MED-5 lossless floor; decode → owned intermediate → encode): **WAV** (RIFF; PCM
  8/16/24/32 + IEEE-float 32/64 + EXTENSIBLE), **AIFF** (IFF big-endian + the 80-bit extended sample rate,
  exact both ways), **FLAC** (full mandatory decoder — every subframe/rice/decorrelation path, CRC-8+CRC-16,
  our own MD5 verified; a compliant encoder — 4096 blocking, per-frame stereo decorrelation, FIXED + LPC(8)
  via hesap-dsp `aryule`, STREAMINFO MD5). All round-trips bit-exact; MD5 = the RFC-1321 vectors.
- **The ffmpeg reference oracle, both directions** (the mikktspace test-oracle precedent — never in the
  product path): ffmpeg 8.0.1 decodes our FLAC to samples identical to our WAV; our decoder reads ffmpeg's
  FLAC encode bit-exact and MD5-verified.
- **Resources**: 'ABUF' (normalized f32 processing domain) + 'AGRF' (the audio-graph-as-resource:
  Source/Gain/Biquad/Mix/Send + edges + automation on the ONE curve engine; one validator both ends — DAG,
  bounds, single output, monotonic automation).
- **THE 4-TRACK GATE**: 2 sources (one RBJ-lowpassed, one automated-gain) → mix. Offline render bit-stable
  (two renders memcmp-identical); the automation verified by superposition (faded lane ≥20 dB down; full mix
  == sum of lanes sample-for-sample); the master encodes to WAV. Automation samples per-sample in exact
  rational time.
- **'MIDI' — MIDI 2.0-native**: 32-bit velocities (bit-replication upscale), notes as intervals, 32-bit
  controllers. SMF format 0+1 parse; the tempo map converts tick → RationalTime exactly across changes.
- **The realtime layer** (⛔ the callback never allocates/locks/touches ResourceManager): an own SPSC command
  ring (cache-line-isolated atomics — the lost-wake scar), a preallocated 32-voice mixer, and `AudioDevice`
  (WASAPI shared-mode, event-driven, float32, `<windows.h>` quarantined). **THE SOAK: 486,720 frames over
  10 s live, xruns == 0** (skips honestly with no endpoint; Linux backend rides the platform sweep).
- Cook edge: .wav/.aiff/.flac → 'ABUF' · .mid → 'MIDI'. Boards: audio 100,744/13 · cooker 313/28.
- **Scar**: the FLAC encoder's stereo-decorrelation layout picker mutated (sub, bps) cumulatively across a
  branch chain — two branches firing left a stale subframe pointer (a 24-bit-only desync). Fixed by choosing
  the winner then setting the complete layout once.

## GEO-11 — the agent surface (`ceridc`) — GEO band capstone

- **The verb library** (`crd-ceridc`): import · cook · query · instantiate · sequence · render · export —
  each emits a machine-readable JSON report. One implementation, **two transports**: a CLI and an **MCP
  JSON-RPC 2.0 stdio server** (`mcp_handle`: initialize · ping · tools/list with per-tool inputSchema ·
  tools/call). The CLI and the stdio loop are thin shells over the library.
- **⛔ Transactional edits**: `instantiate` validates completely before the first side effect — unknown asset
  / non-finite transform rejects atomically (nothing on disk); `dry_run` validates without writing.
- **THE AGENT GATE** (53 asserts): an agent, through the MCP handler alone, imports an STL → cooks a PACK →
  queries the graph → (malformed edit rejects atomically · dry-run writes nothing · real edit lands the SCEN)
  → sequences a 2-shot timeline (the `.otio` twin re-imports with the exact transition) → renders 24 EXR
  frames (decoded through our codec) → exports the interchange twin. Every effect verified through files +
  query verbs, zero GUI. A second smoke drives the real `ceridc mcp` binary over stdio pipes.
- **Scar**: the CLI World needs `register_component<Transform>` before `register_render_components` (the
  sandbox's own order) — a bare register asserted on the nil id.
- Boards: ceridc 53/2. ADR-0081's agent-native charter is real.

## Numbers

audio 100,744/13 · cooker 313/28 · ceridc 53/2 — all green, zero regression. 26 files tidy-clean across the two
slices.

## Deferred with homes

- Lossy audio (MP3/Opus/Vorbis) → MED-6 (the decode surface exists; encoders are month-class, scheduled).
- CLAP hosting → a future phase (the ring/voice model is CLAP-shaped by design).
- The Linux audio backend → the RET-8-style cross-platform sweep (needs hardware to verify).
- MediaKind::Resource binding of real scene renders through the timeline render seam → the renderer band.

## Lessons

- The reference-codec oracle is decisive and cheap: ffmpeg both-directions caught nothing new only because
  the hermetic MD5 round-trip already pinned correctness — but it is the proof that "our FLAC" IS FLAC, not a
  private dialect. Every owned codec should earn a both-directions oracle where a reference tool exists.
- "Own the primitive" held again: the SPSC ring is ~30 lines of hand-rolled atomics and it made the realtime
  gate a plain FIFO-order assertion instead of a flaky timing test.
