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

## How to add an entry

1. Run `/research <topic>` in OpenCode — the researcher agent writes the file
   for you and updates this index.
2. Or copy `RESEARCH_TEMPLATE.md` manually if you're filling one in by hand.

Keep entries short and opinionated. The point is decision support, not a
literature review.
