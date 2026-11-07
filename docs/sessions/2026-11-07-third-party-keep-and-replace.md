# 2026-11-07: third-party libraries kept and replaced

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [WINDOW-OWN](../ROADMAP.md#slice-window-own), [ZSTD-OWN](../ROADMAP.md#slice-zstd-own).

The user decided, for the third-party code that shipped engine modules declare:

| Library | Decision |
|---|---|
| AWS-LC | Stays for good as crd-crypto's provider. Cerid writes no cryptographic primitive. |
| GLFW | Stays for now. Cerid's own backend replaces it later: WINDOW-OWN. |
| zstd | Stays for now. Cerid's own codec replaces it later: ZSTD-OWN. |
| Dear ImGui | Stays for debugging. It is not rewritten; Cerid's own UI system covers products, and ImGui may be deleted later. No row until the user asks. |
| GPU interfaces | Vulkan and DirectX 12 headers stay. |

Validators: `python scripts/check-master-plan.py`, `python scripts/check-repository.py`.
