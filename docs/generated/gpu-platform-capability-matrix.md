# GPU-platform capability matrix (GENERATED -- do not edit)

> Emitted by `tools/ceir_capability_matrix/gen_matrix.py` from `docs/capabilities/gpu-platform-capabilities.toml` (CEIR-0g §4 step-4). The manifest is the source of truth; this file is regenerated. Run `python tools/ceir_capability_matrix/gen_matrix.py` to refresh.

- schema: `2`  ·  audit_date: `2026-08-06`  ·  features: **50**
- ceir_level distribution: L0×17 · L2×4 · L3×3 · L5×14 · L6×10 · n/a×2
- documentary field coverage (present-iff-meaningful): subcategory 50/50 · definition 50/50 · dependencies 27/50 · ckir_capabilities 31/50 · command_families 34/50 · executors 37/50 · runtime_systems 42/50 · backend_capabilities 24/50 · editor_support 50/50 · quality_perf_gates 11/50 · fallback 18/50 · references 26/50 · limitations 47/50

## Health checks

- HARD errors: **0**  ·  review flags: **0**

## Matrix

| id | category | class | raf | ceir | providers | determinism | backends | assets | status |
|---|---|---|---|---|---|---|---|---|---|
| `app_custom_renderer` | runtime | A+E | 5 | 5 | gpu | - | vk+dx12 | 1 | shipped |
| `canvas_compositor` | ui_2d | A+R | 0 | 0 | - | - | - | 0 | target |
| `ceir_audio_dsp` | audio | A+R | n/a | 3 | host | BitExact | - | 3 | gate-only |
| `ceir_autodiff` | compiler | B | n/a | 5 | gpu | - | vk+dx12 | 1 | gate-only |
| `ceir_autotune` | compiler | A+R | n/a | 5 | gpu | - | vk+dx12 | 1 | gate-only |
| `ceir_dist_sharding` | compute | A+R | n/a | 5 | host, gpu | BitExact | - | 2 | gate-only |
| `ceir_ml` | compute | A+R | n/a | 5 | gpu | - | vk+dx12 | 1 | gate-only |
| `ceir_optimizer` | compiler | B | n/a | 2 | - | - | - | 0 | gate-only |
| `ceir_sparse` | compute | A+R | n/a | 5 | gpu | - | vk+dx12 | 1 | gate-only |
| `ceir_transform_rewrite` | compiler | A+R | n/a | 5 | gpu | BitExact | vk+dx12 | 4 | gate-only |
| `chir_language` | language | B | n/a | 2 | - | - | - | 2 | gate-only |
| `cluster_mesh_shader` | geometry | A+R | 4 | 5 | gpu | - | vk+dx12 | 0 | mechanic-proven |
| `crd_d007_shell` | ui_2d | T | 0 | n/a | - | - | - | 1 | target |
| `crd_font_own_stack` | ui_2d | A+R | 0 | 0 | - | - | - | 1 | target |
| `cuda_graphs_provider` | provider | B | n/a | 3 | gpu | BitExact | - | 2 | gate-only |
| `deferred_classic` | render_pipeline | A | 2 | 0 | - | - | - | 0 | target |
| `draw_debug_viz` | ui_foundation | A+R | 5 | 0 | - | - | vk+dx12 | 0 | shipped |
| `forward_basic` | render_pipeline | A | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |
| `forward_csm` | render_pipeline | A | 5 | 6 | gpu | - | vk+dx12 | 2 | shipped |
| `forward_csm_gpu` | render_pipeline | A+R | 5 | 6 | gpu | - | vk+dx12 | 2 | shipped |
| `forward_csm_moment` | render_pipeline | A | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |
| `forward_plus` | render_pipeline | A | 1 | 0 | - | - | - | 0 | target |
| `full_rt_lit` | render_pipeline | A+R | 2 | 3 | gpu | - | vk+dx12 | 1 | gate-only |
| `hot_reload_frame` | runtime | A+R | 5 | 5 | gpu | - | vk+dx12 | 0 | shipped |
| `hot_reload_program` | runtime | A+R | 5 | 5 | gpu | - | vk+dx12 | 0 | shipped-coarse |
| `hybrid_rt` | render_pipeline | A+R | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |
| `imgui_debug_ui` | ui_foundation | T | 5 | n/a | - | - | vk+dx12 | 0 | shipped |
| `lighting_2d` | scene_2d | A+R | 0 | 0 | - | - | - | 1 | target |
| `mesh_shader_frame` | geometry | A+R | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |
| `mrt_gbuffer_mechanic` | mechanic | A | 2 | 2 | gpu | - | vk+dx12 | 0 | mechanic-proven |
| `pass_instancing_foreach` | runtime | A | 5 | 5 | gpu | - | vk+dx12 | 1 | shipped |
| `platform_windowing_input` | ui_foundation | A+R | 3 | 0 | - | - | - | 0 | shipped-basic |
| `rah_resource_table_bindless` | canonical_model | B | 0 | 0 | - | - | - | 0 | target |
| `rah_typed_attachments` | canonical_model | B | 0 | 0 | - | - | - | 0 | target |
| `raytrace_inline_mechanic` | mechanic | A+E | 2 | 2 | gpu | - | vk+dx12 | 0 | mechanic-proven |
| `raytrace_pipeline_mechanic` | mechanic | A+E | 4 | 5 | gpu | - | vk+dx12 | 1 | mechanic-proven |
| `sprite_renderer` | scene_2d | A+R | 0 | 0 | - | - | - | 2 | target |
| `tensor_ml_mlp` | compute | A+R | n/a | 5 | gpu | BitExact | vk+dx12 | 2 | gate-only |
| `tessellation_frame` | geometry | A+R | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |
| `text_layout` | ui_2d | A+R | 0 | 0 | - | - | - | 1 | target |
| `tilemap_2d` | scene_2d | A+R | 0 | 0 | - | - | - | 1 | target |
| `tonemap_agx` | post | A | 5 | 6 | gpu | - | vk+dx12 | 2 | shipped |
| `tonemap_srgb` | post | A | 5 | 6 | gpu | - | vk+dx12 | 2 | shipped |
| `ui_effect_graph` | ui_2d | A+R | 4 | 5 | gpu | - | vk+dx12 | 6 | gate-only |
| `ui_material` | ui_2d | A | 0 | 0 | - | - | - | 1 | target |
| `ui_property_system` | ui_2d | A+R | 0 | 0 | - | - | - | 0 | target |
| `ui_style_system` | ui_2d | A | 0 | 0 | - | - | - | 2 | target |
| `ui_world` | ui_2d | A+R | 0 | 0 | - | - | - | 1 | target |
| `vector_renderer` | ui_2d | A+R | 0 | 0 | - | - | - | 0 | target |
| `visibility_buffer_frame` | render_pipeline | A+R | 5 | 6 | gpu | - | vk+dx12 | 1 | shipped |

