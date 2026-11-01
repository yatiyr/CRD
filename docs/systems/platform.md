# crd-platform

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Reference pointer; no live status here.** Track slices/findings in [ROADMAP](../ROADMAP.md).

[Module manifest](../../engine/foundation/platform/CMakeLists.txt); [Current module/source map](../systems/README.md).
The window backend (the GLFW `PlatformContext`, `Window` and the `platform.hpp` umbrella) is
[crd-platform-window](../../engine/foundation/platform-window/CMakeLists.txt), split from this module on 2026-11-01
([ownership decision 9](../design/capability-ownership.md#decisions)); headers and namespace are unchanged.
Full technical requirements and dated evidence: [preserved reference](../archive/systems/platform.md).
Read the master row and current public source before relying on a historical example.
