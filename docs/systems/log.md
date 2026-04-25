# crd-log

Per-channel, multi-sink, sync-or-async logging. The way every part of the
engine talks to the developer.

> Long-form deep-dive: [`docs/log/LOG_FILE.md`](../log/LOG_FILE.md). Read
> that file when you want to understand the internals; this overview is
> for "I just need to use it."

## What it is

You write:

```cpp
CRD_LOG_INFO(g_log_renderer, "loaded {} meshes in {} ms", count, ms);
```

…and the line ends up on the console with colors, in `engine.log` on disk,
in Visual Studio's Output window, and (later) in an in-game console
overlay. With timestamps, the source file + line, and the thread id.

If the build was configured with `CRD_LOG_MIN_LEVEL=Warn`, the line above
becomes literally nothing — no string in the binary, no code, zero cost.
That's the deal: log generously in development, ship a quiet release.

## Six levels

`Trace` < `Debug` < `Info` < `Warn` < `Error` < `Critical`. There's a
seventh, `Off`, which means "this channel accepts nothing."

`Critical` is special: it always synchronously delivers and flushes, even
in async mode. So a dying engine still gets its last words to disk.

## Channels

Each subsystem owns a channel. Declare in a `.cpp`:

```cpp
CRD_DEFINE_LOG_CHANNEL(g_log_renderer, "Renderer", crd::log::LogLevel::Info)
```

If a header needs to mention it:

```cpp
CRD_DECLARE_LOG_CHANNEL(g_log_renderer);
```

Each channel has its own runtime level. `set_channel_level("Renderer",
LogLevel::Debug)` turns up the volume on one subsystem without touching
the rest.

## Sinks

A sink is a destination. The system ships five:

- **`ConsoleSink`** — stdout/stderr with ANSI colors when the terminal
  supports them. Errors and Critical go to stderr.
- **`FileSink`** — appends to a file, rotates by size (e.g. 10 MB,
  keep 5 backups).
- **`DebuggerSink`** — `OutputDebugStringA` on Windows, no-op elsewhere.
  Lights up the VS Output window.
- **`RingBufferSink`** — keeps the last N records in memory. The
  in-game console overlay (later) reads from this.
- **`NullSink`** — discards. Useful for benchmarks and tests.

Sinks have their own `min_level`, applied after the channel filter. So
you can have "Console shows Info+, File captures Trace+" with the same
channels.

## Setup, in one block

```cpp
#include <crd/log/log.hpp>

CRD_DEFINE_LOG_CHANNEL(g_log_main, "Main", crd::log::LogLevel::Info)

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = true;
    crd::log::init(cfg);

    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::add_sink(std::make_unique<crd::log::FileSink>("engine.log"));

    CRD_LOG_INFO(g_log_main, "engine starting up");
    // ... do stuff ...

    crd::log::shutdown(); // drains and flushes
}
```

## Performance

- Disabled levels (compile-time stripped): zero. The line is gone.
- Disabled levels (runtime, channel filter): one byte load + one branch.
  Format args are NOT evaluated.
- Enabled level, sync mode: `std::format` + sink calls on the calling thread.
- Enabled level, async mode: `std::format` + one string copy + one mutex
  + one condvar signal. Worker thread does the I/O.

## Dependencies

`crd-core` only.

## Tests

`tests/log/test_log.cpp`. 11 cases covering levels, channels, sync, async,
each sink, file rotation, critical bypass, and bulk channel updates.
