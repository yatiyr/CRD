# Research

One file per research task. The "we googled this and here's what we found"
archive. Plain English. "What was the question, what's the answer, what
should Cerid do."

Read this folder before re-researching something. Read
[`RESEARCH_TEMPLATE.md`](RESEARCH_TEMPLATE.md) to write a new entry.

Filename convention: `YYYY-MM-DD-<kebab-case-topic>.md` (2-4 word slug).

| Date | Topic | Used by |
| ---- | ----- | ------- |
| 2026-05-27 | [Streaming allocators (virtual-memory substrate)](cerid-streaming-allocators.md) | Phase 2.2 S2+ (ADR-0085) |
| 2026-06-14 | [Beating MKL on 1D complex FFT — every number + dead end](fft-mkl-crush.md) | v10 `crd-hesap-fft` (FRONTIER, open) |
| 2026-07-06 | [v15 forward-mode AD: frontier crush levers + reconstruct-verify tables](2026-07-06-v15-forward-ad-crush.md) | v15 `crd-hesap-autodiff` (a–z impl reference) |
| 2026-07-23 | [The offline renderer frontier — the OFF band master survey](2026-07-23-offline-renderer-frontier.md) | D-007 OFF band (rows 46-54); offline render mode |
| 2026-07-23 | [The geometry-resource pipeline — import → decompose → author → cook](2026-07-23-geometry-resource-pipeline.md) | D-007 GEO band (rows 66-70); mesh/scene I/O before the offline renderer |
| 2026-07-23 | [The world-class resource + scene system — GEO band no-gap expansion](2026-07-23-resource-scene-world-class.md) | D-007 GEO band (rows 66-76): ECS integration · asset processor · animation · timeline (film) · audio (DAW) · MCP/CLI agent surface |
| 2026-07-23 | [The owned media-codec platform — images · audio · video · transcode](2026-07-23-media-codec-platform.md) | D-007 MED band (rows 77-88): full codec roster by patent status (H.265/AAC excluded by name, H.264 time-gated 2027-11), decode→intermediate→encode transcode engine |

## How to add an entry

1. Run `/research <topic>` in OpenCode — the researcher agent writes the file
   for you and updates this index.
2. Or copy `RESEARCH_TEMPLATE.md` manually if you're filling one in by hand.

Keep entries short and opinionated. The point is decision support, not a
literature review.
