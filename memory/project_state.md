---
name: Project state
description: Current phase and last-shipped milestone for the Cerid engine
type: project
---

Phase 2.6 v1d shipped (2026-05-04).

**Why:** Async I/O + async resource loading now complete. Next is v1e — ShaderResourceLoader + MaterialResourceLoader.

**How to apply:** When discussing next steps, v1d is done. Point toward v1e (shader/material loaders end-to-end) as the active work.

Feature inventory as of v1d:
- v1a: ResourceId, CRDR container, ResourceManager shell, manifest_dump CLI
- v1b: Cooker CLI, zstd compression, crd-cooker static lib
- v1c: RefCounted<T>, ResourceHandle<T>, load_sync<T>, cycle detection, smoke_resources
- v1d: AsyncFile (IOCP), load_async<T>, wait_ready() (fiber-cooperative), load coalescing via m_in_flight, smoke_resources_async

Test counts (six-config green):
- win-debug:          429/429
- win-relwithdebinfo: 429/429
- win-release:        426/426
- win-asan:           429/429
- win-clang-cl:       429/429
- win-tidy:           429/429
