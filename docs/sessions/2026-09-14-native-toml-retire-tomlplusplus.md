# A native TOML reader/writer, and the retirement of tomlplusplus

<!-- doc-role: historical -->
> Dated evidence. Live owners: the [systems map](../systems/README.md) (`toml`), the [pinned-inputs
> contract](../design/pinned-inputs.md) (the removed pin). Rules: [AGENTS](../../AGENTS.md). Follows the
> [std-container ban](2026-09-14-ban-std-containers.md).

## User direction

Implement a native TOML parser and serializer on the engine's own containers and use it everywhere, retiring the
third-party `tomlplusplus` completely.

## The module

A new foundation module `crd-toml` ([engine/foundation/toml](../../engine/foundation/toml/CMakeLists.txt)),
depending only on `containers` and `core`:

- [toml.hpp](../../engine/foundation/toml/include/crd/toml/toml.hpp) — the public surface, deliberately mirroring the
  toml++ subset the engine used so call sites move over with an include swap and `toml::` → `crd::toml::`:
  `node`/`table`/`array`, `node_view` (from `node["key"]`, with `value<T>`, `value_or`, `as_table`/`as_array`,
  `is_*`), `parse_result` (`operator bool`, `error()`, `table()`), typed `value<T>()` for
  `std::string_view`/`i64`/`double`/`bool`, `items()` for ordered table key/value iteration, keyed `operator[]`, and
  `source()` line/column info.
- [toml.cpp](../../engine/foundation/toml/src/toml.cpp) — a recursive-descent parser and a serializer. It supports
  the features the assets use: comments (`#`, inline, Unicode), bare/quoted keys, `[table]` and dotted
  `[table.sub]` headers, `[[array-of-tables]]`, inline arrays (multi-line), inline tables, and string/integer/float/
  boolean scalars, with a `to_toml()` writer for the config round-trip.
- [test_toml.cpp](../../tests/foundation/toml/test_toml.cpp) — scalars/kinds, dotted headers, arrays and
  arrays-of-tables, inline tables, malformed-input rejection, and a build→serialize→reparse round trip.

Two correctness points that a naive parser would miss, both found by the asset-pipeline tests and fixed:

- **Array-of-tables navigation.** `[pass.params]` after `[[pass]]` is the `params` sub-table of the *last* `pass`
  element; the header walker descends into an array-of-tables' most recent element instead of rejecting it.
- **Lossless numeric coercion, both directions (toml++ parity).** toml++'s `value<double>()` returns an integer
  node as a double, and `value<int64_t>()` returns an integer from a float with no fractional part. Cerid's writers
  rely on both: the frame emitter prints `1.0` as `1` (via `%.17g`), and the LOD writer prints `impostor_grid = 8`
  as `8.000000` (fixed `%.6f`). `crd::toml::node::value<double>()` widens an integer and `value<crd::i64>()`
  accepts an integral float, so `scale = 1` and `impostor_grid = 8.000000` both round-trip. A strict reader mis-read
  them as "no size" (`BadResourceSize`) or reverted a field to its default (the field-survival scar).

## The migration and the retirement

Twelve files moved off `#include <toml++/toml.hpp>` to `#include <crd/toml/toml.hpp>` (config; the frame/technique/
material/vertex/light cookers; geometry LOD; and the asset-cooker `cook_db`, `obek`, `preset`, `profile`, `scene`
handlers). Eight module `CMakeLists.txt` now link `crd-toml` instead of `tomlplusplus::tomlplusplus`, and the
`TOML_EXCEPTIONS=0` define is gone.

`tomlplusplus` is fully removed: the [pin registry](../../cmake/pins.json), the `crd_add_pinned_package` call and
`PACKAGES tomlplusplus` rows in the root [CMakeLists](../../CMakeLists.txt), the `CRD_KNOWN_PACKAGES` entry and the
package-name resolver in [CrdModules.cmake](../../cmake/CrdModules.cmake), and the two test fixtures. The
[license manifest](../generated/dependency-licenses.md) was regenerated (9 packages, was 10). No `#include <toml++`,
no `tomlplusplus` token, and no bare `toml::` remain in the tree.

## Verification

- **Build:** `win-debug` (Ninja, MSVC 19.51, vcvars) builds fully, 0 errors, sandbox linked.
- **Tests:** the toml, config, and asset-pipeline suites (frame/technique/material/vertex/light cook, LOD,
  scene-cooker, CEIR frame validate/lower, cook cache) — **429 of 431 pass**. The two failures are Vulkan pixel
  gates in `test_ui_frosted_glass_gpu.cpp` on frames that now parse and round-trip correctly; they are
  software-renderer/environmental, not TOML, and the hosted GPU lanes own them. A full `ctest` run is the definitive
  local check.
- **Validators:** `check-repository.py` (97 modules), `check-master-plan.py`, `check-pins.py` (9 packages),
  `check-generated.py`, and the no-std-container guard all PASS.

## Handoff

One diff: the `crd-toml` module and its test, the 12 migrated files, the 8 module `CMakeLists.txt`, the root
`CMakeLists.txt`/`CrdModules.cmake`/`pins.json` retirement, the regenerated license manifest, the systems-map row and
the two test-fixture updates. Local evidence is on `win-debug`; the hosted matrix (Linux, sanitizers, clang-cl,
shipping, and the GPU lanes) owns the rest, so this is `Needs CI` until the maintainer's published run is green.
No commit was made by the agent.
