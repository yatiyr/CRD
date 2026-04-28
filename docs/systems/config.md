# crd-config

Small configuration substrate over TOML. `crd-config` exists so downstream
systems can consume human-edited settings through a stable, typed, non-fatal
API instead of hand-parsing ad hoc blobs.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| 1.6a | TOML wrapper, typed get/set, schema-with-defaults behavior | ✅ |
| 1.6b | explicit reload hook | ⏳ |
| 1.6c | first consumers (ImGui, log, input) | ⏳ |

## Core decisions

- Backend is TOML (`toml++`), but the public API is Cerid-owned.
- Missing keys and type mismatches are **non-fatal** and return the caller's
  fallback value with a warning through `g_log_config`.
- Access uses dot-separated key paths (`imgui.theme.preset`), even when the
  underlying TOML uses nested tables.
- Runtime config remains text. This module is intentionally separate from the
  cooked runtime-binary asset pipeline.

## What ships today

- `Config`
  - `load_from_string()`
  - `load_from_file()`
  - `reload()`
  - `contains()`
  - `get<T>(key, fallback)`
  - `set<T>(key, value)`
- Supported types in v1a:
  - `int`
  - `i64`
  - `f32`
  - `f64`
  - `bool`
  - `containers::String`
  - `containers::Array<i64>`
  - `containers::Array<f32>`
  - `containers::Array<containers::String>`
  - `math::Vec4f`
- `g_log_config` channel
- `engine/config/sample.toml`
- `smoke_config` runtime example

## How to use it

```cpp
crd::config::Config cfg;
if (!cfg.load_from_file(crd::platform::fs::Path("runtime/configs/imgui_layer.toml")))
{
    // handle fatal file-open/parse failure for your app if needed
}

const auto preset = cfg.get<crd::containers::String>("imgui.theme.preset", crd::containers::String("dark"));
const auto clear  = cfg.get<crd::math::Vec4f>("renderer.clear_color", {});
const auto fps    = cfg.get<int>("app.target_fps", 60);
```

## Long-term direction

- explicit reload hook in 1.6b
- first real consumers in 1.6c
- later shader/asset hot-reload work will influence how far config reload
  should go, but file watching itself is intentionally deferred
