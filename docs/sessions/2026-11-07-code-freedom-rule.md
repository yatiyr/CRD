# 2026-11-07: Cerid's freedom decides what stays

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Decision on the permissive sources found by the [copied-code audit](2026-11-07-copied-code-audit.md). The user, in full:

> IF IT AFFECTS MY LICENSE, REWRITE SO IF IT IS A PROBLEM AND RISK TO USE ANYTHING AND THE LICENSE SAYS USE IT BUT IT MUST BE OPEN
> SOURCE OR STUFF (SO IF IT DIMINISHES CERID'S FREEDOM, REWRITE, NO REGRESSIONS, FULL AGENT RULE! IF SOMETHING IS WRITTEN IT MUST BE
> FULLY CERID OR A CODE THAT CAN BE MODIFIED AND USED FREELY)

The rule now in [AGENTS](../../AGENTS.md) and [pinned inputs](../design/pinned-inputs.md#copyleft): the test for a source is whether
its licence lets Cerid stay a closed, sold product and change the code freely.

| Answer | Sources | Outcome |
|---|---|---|
| Yes | MIT, BSD, Apache-2.0, BSL-1.0, public domain | Stays; header line and kept notice |
| No | GPL, LGPL, AGPL, MPL, proprietary text | Rewritten with no regression |
| Unclear | No licence statement found | Treated as no |

Licences settled for this decision, read at their source:

| Source | Evidence | Outcome |
|---|---|---|
| Cephes | [xsf's licence list](https://github.com/scipy/xsf/blob/main/LICENSES_bundled.txt): BSD-3 with the author's permission | Stays |
| QUADPACK | [SLATEC guide](https://www.netlib.org/slatec/guide): the library holding it is in the public domain | Stays |
| ReproBLAS | `LICENSE` in the pinned copy: BSD-style terms, University of California | Stays |
| MINPACK-2 | No statement found; only later projects' licences cover copies of it | `more_thuente_line_search.hpp` is rewritten |

Validators: `python scripts/check-master-plan.py`, `python scripts/check-repository.py`.
