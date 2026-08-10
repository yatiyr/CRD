include(CTest)
include(Catch)

# Header-only test utilities (ULP compare, GPU determinism check) — must
# come BEFORE any test target that links it.
add_subdirectory(test_helpers)

add_subdirectory(core)
add_subdirectory(vm)
add_subdirectory(units)
add_subdirectory(ceir)  # D-007 CEIR-1a: Cerid Execution IR core (arena + intrusive def-use)
add_subdirectory(ceir-host)  # D-007 CEIR-6b: the crd-jobs execution-provider bridge
add_subdirectory(ceir-cook)  # D-007 CEIR-7a: the CEIR asset cook bridge
add_subdirectory(ceir-gpu)  # D-007 CEIR-13d: the GPU lowering bridge
add_subdirectory(render-asset-core)  # RAF-1 (D-007 "RAF band"): identity/namespaces/diagnostics/deps Gate 1
add_subdirectory(gpu-context)        # RAF-2a (D-007 "RAF band"): canonical GPU command model (device-free structural gate)
add_subdirectory(render-program)     # RAF-4 (D-007 "RAF band"): shader + program contract Gate 4
add_subdirectory(render-material)    # RAF-5 (D-007 "RAF band"): material + technique contract Gate 5
add_subdirectory(render-pass)        # RAF-6 (D-007 "RAF band"): pass-executor registry Gate 6
add_subdirectory(render-graph)       # RAF-7 (D-007 "RAF band"): unified frame-graph runtime Gate 7
add_subdirectory(time)
add_subdirectory(perf)
add_subdirectory(perf-ui)
add_subdirectory(log)
add_subdirectory(memory)
add_subdirectory(math)
add_subdirectory(geometry-primitives)
add_subdirectory(geometry-bvh)
add_subdirectory(geometry-convex)
add_subdirectory(geometry-mesh)
add_subdirectory(geometry-spatial)
add_subdirectory(geometry-polygon)
add_subdirectory(geometry-mesh-processing)
add_subdirectory(lod) # REN-40-C1: the mesh LOD chain
add_subdirectory(geometry-delaunay)
add_subdirectory(geometry-decomposition)
add_subdirectory(geometry-bvh-gpu)
add_subdirectory(asset-io)
add_subdirectory(geometry-shader-helpers)
add_subdirectory(geometry-curves)
add_subdirectory(hesap)
add_subdirectory(hesap-dense)
add_subdirectory(hesap-sparse)
add_subdirectory(hesap-ordering)
add_subdirectory(hesap-resources)
add_subdirectory(hesap-iterative)
add_subdirectory(hesap-amg)
add_subdirectory(hesap-sched)
add_subdirectory(hesap-direct)
add_subdirectory(hesap-eigen)
add_subdirectory(hesap-special)
add_subdirectory(hesap-quadrature)
add_subdirectory(hesap-autodiff)
add_subdirectory(kir)
add_subdirectory(frame-cook)
add_subdirectory(technique-cook)  # REN-37 (D-007 row 140): the LIGHTING TECHNIQUE as an authored `.crdt` asset
add_subdirectory(material-cook)   # REN-38-C (D-007 row 141): the MATERIAL as an authored `.crdm` asset
add_subdirectory(vertex-cook)     # REN-38-D (D-007 row 141): the VERTEX PROGRAM as an authored `.crdv` asset
add_subdirectory(light-cook)      # REN-38-E (D-007 row 141): the LIGHTING VOCABULARY as an authored asset
add_subdirectory(gpu-context-vulkan)  # ADR-0099 v17-i-a
add_subdirectory(gpu-context-dx12)    # ADR-0100 v17-i: D3D12 IComputeContext backend
add_subdirectory(gpu-context-cuda)    # ADR-0100 (2026-08-07): CUDA IComputeContext backend; self-skips without CUDAToolkit
add_subdirectory(kir-vulkan)
add_subdirectory(ceir-gpu-vulkan)  # D-007 CEIR-13z-1b: a CEIR compute asset executed on a real Vulkan device (execute_lowered, ADR-0126)
add_subdirectory(kir-cuda)
add_subdirectory(kir-dx12)
add_subdirectory(ceir-gpu-dx12)  # D-007 CEIR-13z-1b: the DX12 mirror — a CEIR compute asset on a real D3D12 device (Windows-only)
add_subdirectory(kir-webgpu)
add_subdirectory(kir-hip)
add_subdirectory(kir-metal)
add_subdirectory(hesap-opt)
add_subdirectory(hesap-stats)
add_subdirectory(hesap-ode)
add_subdirectory(hesap-fft)
add_subdirectory(hesap-dsp)
add_subdirectory(hesap-wavelet)
add_subdirectory(hesap-comms)
add_subdirectory(hesap-interp)
add_subdirectory(hesap-diff)
add_subdirectory(hesap-motion)
add_subdirectory(hesap-tensor)
add_subdirectory(containers)
add_subdirectory(platform)
add_subdirectory(app)
add_subdirectory(config)
add_subdirectory(imgui)
# RET-8 (ADR-0105): tests/{renderer,shader,rhi,rhi_vulkan} DELETED with their modules — the parity audit
# (D-007 rows 89-96) records every assertion's green gpu-context home.
add_subdirectory(jobs)
add_subdirectory(stress)
add_subdirectory(resources)
add_subdirectory(meshgen)
add_subdirectory(scene)
add_subdirectory(scene-render)  # GEO-7 (D-007 row 72): chunk-grain extract/cull/submit gates
add_subdirectory(anim)          # GEO-8 (D-007 row 73): SKEL/ANIM resources + THE FOX bit-stable resample gate
add_subdirectory(timeline)      # GEO-9 (D-007 row 74): TIML resource + deterministic eval + THE EXR RENDER GATE
add_subdirectory(audio)         # GEO-10 (D-007 row 75): codecs bit-exact + graph render + MIDI + realtime
add_subdirectory(ceridc)        # GEO-11 (D-007 row 76): THE AGENT GATE — the full scenario through MCP alone
add_subdirectory(scene_cooker)
add_subdirectory(cooker)
add_subdirectory(preset)
add_subdirectory(profile)
add_subdirectory(eylem)
add_subdirectory(eylem-rigid3d)
add_subdirectory(draw)
add_subdirectory(eylem-viz)
add_subdirectory(geometry-viz)
add_subdirectory(sandbox)

if(CRD_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
