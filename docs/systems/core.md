# crd-core

The bottom of the dependency stack. Everything else uses it; it uses nothing.

## What's in it

- **Fixed-width type aliases** (`u8`, `u16`, `u32`, `u64`, `i8…i64`, `f32`, `f64`,
  `usize`) so types mean the same thing on every compiler.
- **Platform / compiler / arch detection** macros and helper strings:
  `CRD_OS_WINDOWS`, `CRD_COMPILER_MSVC`, `CRD_ARCH_X64`, plus
  `crd::platform_name()`, `crd::compiler_name()`, `crd::arch_name()`.
- **`CRD_ASSERT(expr)` / `CRD_ASSERT_MSG(expr, "...")`** for "this must be
  true or the program is broken" checks. Compiled out when
  `CRD_ENABLE_ASSERTS=OFF`.
- **`CRD_VERIFY(expr)`** aliases `CRD_ASSERT(expr)` in assert-enabled builds
  and becomes plain expression evaluation in release.
- **`CRD_FATAL("...")`** for explicit, unconditional crash with message.
- **Generated `build_config.hpp`** containing `CRD_VERSION_*`,
  `CRD_DEBUG`/`CRD_RELEASE`, and `CRD_LOG_MIN_LEVEL_NUM`.
- **Tiny portability helpers**: `CRD_FORCEINLINE`, `CRD_NOINLINE`,
  `CRD_WHILE_FALSE` (for safe macro `do { ... } while (false)` idiom).

## How to use it

```cpp
#include <crd/core/core.hpp>

void f(crd::u32 n)
{
    CRD_ASSERT(n > 0);
    CRD_VERIFY(n < 1000);
}
```

The umbrella header `crd/core/core.hpp` pulls in `types.hpp`, `platform.hpp`,
`assert.hpp`, and `build_config.hpp`. Most user code only needs that one
include.

## Dependencies

None. This is the floor.

## Tests

`tests/core/test_core.cpp`. Currently 2 cases (type sizes, platform
detection).
