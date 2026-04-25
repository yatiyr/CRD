# crd-log — A Walkthrough In Plain Words

> This document is for **you** (and future-you), not a manual for outsiders.
> It explains, in plain language, *why* every piece of the logging system exists and *how*
> it actually works on the inside. No marketing, no diagrams pretending things are simpler
> than they are. If something is ugly, this document says so.

---

## 1. The 30-second mental model

When you write:

```cpp
CRD_LOG_INFO(g_log_renderer, "loaded {} meshes in {} ms", count, ms);
```

…six things happen, in order:

1. **A macro decides whether the line even compiles into the binary.**
   If your build was configured with `CRD_LOG_MIN_LEVEL=Warn`, the whole line above
   becomes `((void)0)` — *literally nothing* — at preprocessing. No code, no string,
   no symbol. This is "compile-time stripping".

2. **A one-byte runtime check** asks: "is this channel's runtime level
   `<= Info` right now?" If not, we return immediately. Cost: one load + one
   compare. The arguments `count`, `ms` are *not yet evaluated*.

3. **`std::format` runs** on the format string and args, producing a `std::string`.
   This is where the cost actually lives — but only when we're going to log.

4. **`detail::dispatch()` is called.** It packages the formatted message + metadata
   (channel pointer, source location, timestamp, thread id) into a `LogRecord`.

5. **In sync mode**, `dispatch` walks every sink and calls `sink->write(rec)`.
   **In async mode**, it copies the record into a queue and a background thread
   does the walking. Critical level always goes the sync path so a crash gets out.

6. **Each sink decides what to do.** ConsoleSink prints with ANSI colors,
   FileSink appends to disk and rotates if needed, RingBufferSink keeps the last N
   in memory, DebuggerSink calls `OutputDebugStringA`, NullSink throws it away.

That's the whole thing. Everything below is just detail.

---

## 2. The pieces, top to bottom

### 2.1 `LogLevel` — `engine/log/include/crd/log/log_level.hpp`

A plain `enum class : u8` with seven values:

```
Trace=0  Debug=1  Info=2  Warn=3  Error=4  Critical=5  Off=6
```

Why these exact values? Because they double as a numeric scale — bigger number =
more important — and because `Off` as a sentinel means "this channel accepts nothing".
Setting a channel's level to `Off` is the "shut up" knob.

The helpers `to_string`, `to_short_string`, and `from_string` are deliberately
trivial. They don't allocate, don't throw, don't depend on locale. The short forms
(`TRC`, `DBG`, `INF`, `WRN`, `ERR`, `CRT`) are what shows up in formatted output —
fixed-width so columns line up.

### 2.2 `Channel` — `engine/log/include/crd/log/log_channel.hpp`

Three fields. That's it.

```cpp
struct Channel
{
    const char* name;            // a string literal, e.g. "Renderer"
    LogLevel    runtime_level;   // mutable; one byte
    Channel*    next;            // intrusive registry pointer
};
```

A "channel" is a named subsystem-level filter. Renderer logs go through
`g_log_renderer`, physics through `g_log_physics`, etc. Each has its own current
threshold. You change one channel without affecting the others.

#### How channels register themselves

The macro `CRD_DEFINE_LOG_CHANNEL(VAR, NAME, DEFAULT_LEVEL)` does two things:

```cpp
::crd::log::Channel VAR { NAME, DEFAULT_LEVEL, nullptr };
namespace
{
    struct VAR##_LogChannelRegistrar
    {
        VAR##_LogChannelRegistrar() noexcept
        {
            ::crd::log::detail::register_channel(&VAR);
        }
    };
    const VAR##_LogChannelRegistrar VAR##_log_channel_registrar_instance;
}
```

The first line **defines** the channel as a regular global. The anonymous-namespace
block **defines a tiny static struct** whose constructor — which runs at program
startup, before `main()` — pushes the channel onto a global linked list.

Why a linked list and not a `std::vector`? Three reasons:

- **No heap.** The list head is a `std::atomic<Channel*>` inside a function-local static.
  Each channel carries its own `next` pointer. Zero allocations during registration.
- **No static initialization order fiasco.** Channels are zero-initialized
  before any constructor runs (zero-init phase happens before dynamic-init phase),
  and we never *read* the list during static init — only during program execution,
  which means the list is fully populated by then.
- **Lock-free push.** `register_channel` does a CAS loop on the head pointer.
  Multiple TUs can register concurrently if some weird linker decides to run
  static ctors from threads.

`find_channel`, `set_channel_level`, `set_all_channels_level`, `first_channel`,
`channel_count` are simple read traversals on the list. They acquire-load the head
once and walk; no locking. The list is append-only, so iteration is safe even if a
new channel registers while we walk.

### 2.3 `LogRecord` — `engine/log/include/crd/log/log_record.hpp`

A POD that bundles "everything we know about one log call":

```cpp
struct LogRecord
{
    LogLevel             level;
    const Channel*       channel;
    std::source_location loc;          // file/line/function
    std::chrono::system_clock::time_point time;
    u64                  thread_id;
    std::string_view     message;      // already formatted text
};
```

Important property: **the `message` is a view, not an owning string.** In sync mode it
points into a temporary on the calling thread's stack; in async mode it points into a
`std::string` owned by the queue entry. Sinks that need to remember it (like
`RingBufferSink`) must copy.

### 2.4 `ISink` — `engine/log/include/crd/log/log_sink.hpp`

Three virtuals, one byte of state:

```cpp
class ISink
{
public:
    virtual ~ISink() = default;
    virtual void write(const LogRecord& rec) = 0;
    virtual void flush() = 0;

    LogLevel min_level() const noexcept { return m_min_level; }
    void     set_min_level(LogLevel l) noexcept { m_min_level = l; }
protected:
    LogLevel m_min_level = LogLevel::Trace;
};
```

That `m_min_level` is the second filter, applied per-sink *after* the channel filter.
This is how you get "console shows Info+, file captures Trace+" with the same logger.

### 2.5 The Logger globals — `engine/log/src/logger.cpp`

The "logger" is not a class. It's a set of free functions in `crd::log::` that
manipulate one hidden `LoggerState` struct. Why no class? Because the logger is a
process-wide singleton by nature, and wrapping a singleton in a class only adds
ceremony.

`LoggerState` lives behind a Meyers singleton:

```cpp
LoggerState& state() noexcept
{
    static LoggerState s;
    return s;
}
```

This guarantees thread-safe initialization (C++11+ rules) and lazy construction.
You can call `crd::log::dropped_count()` before `init()` and it works.

`LoggerState` holds:

- `LoggerConfig config` — a copy of what `init()` was called with.
- `std::vector<std::unique_ptr<ISink>> sinks` + `std::mutex sinks_mutex` — the sink
  list. The mutex protects the *list* itself; sinks are responsible for their own
  internal thread-safety on their public APIs (e.g. `RingBufferSink::snapshot()`).
- `std::deque<QueuedRecord> queue` + `queue_mutex` + `queue_cv` + `drain_cv` — the
  async pipeline. `drain_cv` is what `flush()` waits on.
- `std::thread worker` + `std::atomic<bool> running` — the background thread (only
  when `cfg.async == true`).
- `std::atomic<u64> dropped` — counter incremented when the queue overflows.
- `std::atomic<bool> initialized` — guards `init()`/`shutdown()`.

#### `init()` and `shutdown()`

`init` copies the config. If async, it spawns a worker that runs `worker_main()`.
Both are idempotent and noexcept; calling `init` twice is a no-op, and so is
calling `shutdown` before `init`.

`shutdown` flips `running` to false, signals the worker, joins it, then flushes
all sinks and clears the sink list. After shutdown, you can call `init` again with
a different config. That's why the tests use a `LoggerScope` RAII helper.

#### `dispatch()` — the hot path

```cpp
void dispatch(LogLevel level, const Channel& ch,
              std::source_location loc, std::string_view message) noexcept;
```

- Reads `state().config` and `state().initialized` (acquire-load).
- If we're not async, OR if this is `Critical` and `flush_on_critical` is set:
  build a `LogRecord` on the stack, lock `sinks_mutex`, call `write()` on every
  sink whose `min_level` accepts this level. If level is Critical, also call
  `flush()` on each sink.
- Otherwise: copy `message` into a `QueuedRecord::message`, lock the queue mutex,
  push, release, and `notify_one()` the condition variable. If the queue is full
  and `drop_on_overflow` is true, increment `dropped` and return immediately.
  Otherwise wait on `drain_cv` until the worker drains some space.

Crucially: the per-thread cost of a *queued* log is one `std::string` copy + one
mutex acquire + one CV signal. That's it. No I/O, no formatting (formatting
already happened in the caller via `std::format`).

#### The worker thread

`worker_main()` is a textbook consumer:

```text
while (true):
    wait on queue_cv until queue not empty OR running is false
    if queue is empty and running is false: drain_cv.notify_all(); return
    pop one entry
    deliver to all sinks (lock sinks_mutex briefly)
    if queue is now empty: drain_cv.notify_all()
```

The two condition variables serve two different audiences: `queue_cv` is for the
worker to wake up when work arrives, `drain_cv` is for `flush()` callers (and the
`drop_on_overflow=false` path) to wake up when there's space again.

### 2.6 The macros — `engine/log/include/crd/log/log_macros.hpp`

This file is doing more work than it looks like. Strip away the per-level wrappers
and the core is:

```cpp
#define CRD_LOG_IMPL(LEVEL, CH, FMT, ...)                                                \
    do                                                                                   \
    {                                                                                    \
        if (::crd::log::detail::should_log((CH), (LEVEL)))                               \
        {                                                                                \
            ::crd::log::detail::dispatch(                                                \
                (LEVEL), (CH),                                                           \
                ::std::source_location::current(),                                       \
                ::std::string_view(::std::format(FMT __VA_OPT__(,) __VA_ARGS__)));       \
        }                                                                                \
    } CRD_WHILE_FALSE
```

A few things to notice:

- **`do { ... } while (false)`** so `if (cond) CRD_LOG_INFO(...); else foo();` is
  legal. This is the standard macro idiom; we wrap it in `CRD_WHILE_FALSE` (defined
  in `crd/core/assert.hpp`) which silences MSVC's `C4127: constant in conditional`.
- **`__VA_OPT__(,)`** is the C++20 way of saying "emit the comma only if there are
  variadic arguments." `CRD_LOG_INFO(g_log, "literal")` and
  `CRD_LOG_INFO(g_log, "x={}", x)` both expand cleanly. **MSVC requires
  `/Zc:preprocessor`** for `__VA_OPT__` to work — that flag is in our root
  CMakeLists. The traditional MSVC preprocessor doesn't support it.
- **`should_log` is checked first.** That short-circuits before evaluating the
  format args. If you write `CRD_LOG_DEBUG(g_log_x, "{}", expensive())`, and the
  channel level is Info, `expensive()` is *never called*.
- **`std::source_location::current()`** captures the call site automatically. It's
  a default arg "as if" — calling it inside the macro sees the macro's expansion
  site, which is the user's source line. Perfect.

The per-level wrappers compile-time-strip:

```cpp
#if CRD_LOG_LEVEL_TRACE >= CRD_LOG_MIN_LEVEL_NUM
    #define CRD_LOG_TRACE(CH, FMT, ...) CRD_LOG_IMPL(LogLevel::Trace, ...)
#else
    #define CRD_LOG_TRACE(CH, FMT, ...) ((void)0)
#endif
```

`CRD_LOG_MIN_LEVEL_NUM` is set by CMake in `build_config.hpp`. The default is `0`
(Trace) in Debug builds and `2` (Info) in Release builds.

`CRD_LOG_CRITICAL` is **never** stripped, because if your engine is dying you want
the death rattle in the binary regardless of build flavor.

Two helpers are also provided:

- `CRD_LOG_IF(cond, LEVEL_MACRO, ch, fmt, ...)` — log only when `cond` is true.
- `CRD_LOG_ONCE(LEVEL_MACRO, ch, fmt, ...)` — fire at most once per call site, ever.
  Implemented with a `static bool` local. Yes, this means it's per-translation-unit
  inline + per-call-site, which is what you want.

### 2.7 The sinks

#### `NullSink` — `sinks/null_sink.hpp`

Two empty virtuals. One job: not crash. Useful in benchmarks (you can measure the
true cost of `dispatch` without I/O confounding it) and in tests that need a
default sink.

#### `ConsoleSink` — `sinks/console_sink.{hpp,cpp}`

On construction it asks each of `stdout` and `stderr`: "are you a TTY?" via
`_isatty(_fileno(...))` on Windows, `isatty(fileno(...))` on POSIX. If yes, it sets
`m_color = true` and tries to enable
`ENABLE_VIRTUAL_TERMINAL_PROCESSING` on the Windows console handle so ANSI escapes
actually render.

`write(rec)`:
1. Call `format_record(rec, with_color, short_path)`.
2. Pick `stderr` if level is Error/Critical, else `stdout`.
3. `fwrite` + `fputc('\n')`.

That's it. No mutex — the global Logger already serialises calls (sync mode mutex,
async single worker). The only contention with another process writing to stderr
is the OS's job.

#### `FileSink` — `sinks/file_sink.{hpp,cpp}`

Opens the file in `"ab"` mode (append, binary). Why binary? To avoid Windows
silently translating `\n` into `\r\n` and breaking our byte-count math.

Rotation logic:
- Track `m_bytes_written`. After a successful append, add `n`.
- Before every write, ask `rotate_if_needed(line.size())`. If
  `bytes_written + line.size() > max_bytes`, rotate.
- Rotation: close current file. Delete the oldest (`name.<max_files>.ext`).
  Rename `name.<max_files-1>.ext -> name.<max_files>.ext`, etc. Finally rename the
  current `name.ext -> name.1.ext` and reopen `name.ext`.

Has its own `std::mutex`. In async mode the worker is the only writer, so the mutex
is uncontended; but `flush()` may be called from any thread, and we want the
contract to be safe for callers that share a sink across multiple loggers later.

`max_bytes = 0` disables rotation entirely.

#### `DebuggerSink` — `sinks/debugger_sink.{hpp,cpp}`

Windows: `OutputDebugStringA(line.c_str())`. The line is the same formatted text
the file/console sinks produce, plus a trailing newline. Visual Studio's "Output"
window renders it.

Other OSes: no-op. Sink still constructs, still gets called, just throws the bytes
away. That keeps user code portable — no `#ifdef` at log call sites.

#### `RingBufferSink` — `sinks/ring_buffer_sink.{hpp,cpp}`

Fixed-capacity circular buffer of `StoredLogRecord`. Each `StoredLogRecord` owns
copies of the channel name, message, and source file path so it survives long
after `write()` returned.

`write(rec)` takes the mutex, overwrites slot `m_head`, advances `m_head`, bumps
`m_count` up to capacity. Once full, oldest entries are silently overwritten —
that's the whole point of a ring buffer.

`snapshot()` returns a vector copy in chronological order. The math:
`start = (m_head + m_capacity - m_count) % m_capacity`, then walk `m_count` slots.

This sink will be the source-of-truth for an in-game console overlay later
(ImGui's debug UI calls `snapshot()` once per frame, renders the records).

### 2.8 The formatter — `engine/log/src/log_formatter.{hpp,cpp}`

`format_record` is a private helper used by Console / File / Debugger sinks. It
produces:

```
YYYY-MM-DD HH:MM:SS.mmm [LVL] [Channel] tid=NNNN file:line - message
```

`localtime_s`/`localtime_r` for the wall clock; subsecond is computed by taking
`time.time_since_epoch()` modulo 1 second. Thread id is hashed once per thread
and cached in `thread_local` storage so we don't pay the hash cost every call.

Color path: an ANSI escape sequence is wrapped around just the `[LVL]` token, with
`\x1b[0m` to reset. We don't color the whole line because timestamps and channels
are easier to read in default terminal color.

`short_path` flag passes through `basename_of`, which scans for the last `/` or
`\\`. Cheap, stable, no `std::filesystem`.

---

## 3. The path of one log call (worked example)

Say in `renderer.cpp`:

```cpp
CRD_LOG_INFO(g_log_renderer, "compiled {} pipelines", n);
```

With `CRD_LOG_MIN_LEVEL=Info` (or lower), the macro is **not** stripped. Expansion is
roughly:

```cpp
do
{
    if (crd::log::detail::should_log(g_log_renderer, LogLevel::Info))
    {
        crd::log::detail::dispatch(
            LogLevel::Info, g_log_renderer,
            std::source_location::current(),
            std::string_view(std::format("compiled {} pipelines", n)));
    }
} while (false);
```

1. **`should_log`**: `(u8)Info >= (u8)g_log_renderer.runtime_level`. If
   `runtime_level == Info`, true; if `Warn`, false. One load + one branch.
2. We pass: `std::format` runs. We get a temporary `std::string`. The
   `std::string_view` ctor stores a pointer into that temporary (which is alive
   until the full expression completes — the `dispatch` call returns).
3. `dispatch` enters. `state().config.async == true`, level is not Critical.
   We build a `QueuedRecord`, *copy* the string contents into `q.message`, push
   onto the deque, signal the CV.
4. Caller returns. The temporary `std::string` is destroyed; we don't care, we
   already copied.
5. **Worker thread** wakes up (already blocked on the CV), pops, builds a
   `LogRecord` whose `message` is a `string_view` into `q.message`, locks
   `sinks_mutex`, calls each sink's `write()`.
6. **`ConsoleSink::write`**: format → fwrite to stdout.
   **`FileSink::write`**: lock its own mutex → maybe rotate → fwrite to disk.
7. Worker queue empty → `drain_cv.notify_all()` (wakes any `flush()` caller).
8. `q` (the `QueuedRecord`) goes out of scope, its `std::string` is freed.

**Lifetime risk to internalise**: the message `string_view` you read inside a sink
is valid only for the duration of `write()`. Don't store it. Copy if you need to.

---

## 4. Why the design choices

### Why `enum class` and not `int`?
Type safety. `LogLevel l = 5` is a compile error; you have to write
`LogLevel::Critical`. `to_string` and the macros become exhaustive switches that
the compiler can warn on if a level is missed.

### Why a free-function API instead of a `Logger` class?
The logger is process-global. There's never a reason to have two of them — sinks
already give you per-destination filtering. A class would just hide a singleton
behind a `Logger::instance()` and add no value.

### Why `std::source_location` and not `__FILE__`/`__LINE__`?
Cleaner. The macro doesn't have to forward `__FILE__`/`__LINE__` explicitly.
`std::source_location::current()` evaluates at the call site through default-arg
semantics. (We still emit the call inside the macro because we need it on the
expansion line, not the dispatch-function line.)

### Why a deque + mutex + condvar instead of a fancy lock-free queue?
Honesty. A single-mutex queue is correct, simple, and *fast enough* for log
throughput on game-thread budgets. If profiling later shows it's a bottleneck,
swap in an MPSC ring. Don't ship complexity you can't measure the value of.

### Why drop on overflow by default?
A blocked game thread is far worse than a missing log line. `dropped_count()` lets
you see, after the fact, that you flooded the queue and need to either raise
`async_queue_capacity`, suppress that channel, or move it back to sync mode.

### Why does Critical bypass async?
If your engine is about to die, the very next thing might be `CRD_FATAL(...)` →
debugbreak → process exit. Anything still in the async queue is gone. So
`Critical` writes synchronously and flushes every sink. `flush_on_critical = true`
is the default for that reason.

### Why does `log_macros.hpp` include `assert.hpp`?
For `CRD_WHILE_FALSE`. Yes, that's a small backward dependency from log onto
core's assert macros — but they're macros, not types, and `crd-log` already
depends on `crd-core`. No cycle.

---

## 5. The build switches

| CMake option / variable | Default | Effect |
|---|---|---|
| `CRD_LOG_MIN_LEVEL` | empty (auto) | `Trace` in Debug, `Info` otherwise |
| `CRD_LOG_MIN_LEVEL_RESOLVED` | derived | What the build actually picks |
| `CRD_LOG_MIN_LEVEL_NUM` | derived | The numeric value baked into `build_config.hpp` |

You can override per build:

```bash
cmake --preset win-release -DCRD_LOG_MIN_LEVEL=Warn
```

…and Trace/Debug/Info call sites disappear from the binary entirely.

---

## 6. The runtime config knobs

```cpp
struct LoggerConfig
{
    bool  async                = false;   // true to spawn a worker thread
    usize async_queue_capacity = 8192;    // soft cap for the deque
    bool  drop_on_overflow     = true;    // false = block producer (rarely wanted)
    bool  flush_on_critical    = true;    // sync-deliver Critical even in async mode
};
```

Per-channel:

```cpp
g_log_renderer.runtime_level = LogLevel::Debug;        // direct
crd::log::set_channel_level("Renderer", LogLevel::Debug);  // by name
crd::log::set_all_channels_level(LogLevel::Warn);          // bulk
```

Per-sink:

```cpp
my_file_sink->set_min_level(LogLevel::Trace);  // capture everything
my_console_sink->set_min_level(LogLevel::Info); // but show only Info+ on terminal
```

---

## 7. Anatomy of a log line

```
2026-04-26 01:09:37.377 [INF] [Renderer] tid=4857446244092308000 main.cpp:56 - frame 0 submitted
└─────────┬─────────┘└──┬──┘└────┬─────┘└──────────┬───────────┘└────┬────┘   └────────┬────────┘
   wall-clock          short    channel       hashed thread id   call site         message
   ms-precision        level                                    (basename only)
```

- **Wall-clock** comes from `std::chrono::system_clock`, formatted via `localtime_s`.
- **Short level** is fixed-width 3 chars so columns line up.
- **Channel** is the registered subsystem name. Useful for `grep`.
- **Thread id** is the hash of `std::this_thread::get_id()`. The numeric value
  is large and ugly but stable per-thread for the lifetime of the process. We
  hash so we don't depend on the OS-specific representation.
- **Call site** is `basename(__FILE__):line`. The full path lives only in your
  source code on disk — keeping logs short keeps them readable.
- **Message** is whatever `std::format` produced.

---

## 8. What I would write differently next time

Things that work today but I'd revisit when the engine is bigger:

- **Allocations on the producer.** Every async log copies the formatted string into
  a `std::string` inside a `QueuedRecord`. With a real `crd-memory` module we should
  use a slab allocator backed by a per-thread arena. The interface is
  `std::string_view` already, so the swap is invisible to callers.
- **The queue is `std::deque` + `std::mutex`.** Replace with a power-of-two MPSC
  ring (Vyukov's, or a simple ticket-locked one) when profiling demands it.
- **`source_location` strings are in the binary even at Info+.** That's fine for
  development, mildly annoying for shipping size. Could strip with a build flag.
- **`localtime_s` per-call.** A monotonic-clock + once-per-second wall conversion
  caching trick would save microseconds per log; not yet worth the complexity.
- **Async overflow blocks on `drain_cv` only when `drop_on_overflow=false`.**
  Should also have a "spin-then-block" variant for low-latency cases.
- **No structured (key/value) records.** Today the message is one flat string.
  When we add an in-game console with filterable columns, we'll want a
  `std::span<KeyValue>` field on `LogRecord`.

None of these are urgent. The current implementation is *boring*, which is exactly
what infrastructure should be.

---

## 9. The example output (from the smoke runtime)

`runtime/src/main.cpp` brings up four channels (Engine / Renderer / Physics /
Audio), spawns two worker threads, and exercises every level. After running it,
`build/win-debug/runtime/engine.log` looks like this (timestamps and tids will
differ on your machine):

```
2026-04-26 01:09:37.377 [INF] [Engine] tid=17877431896815509209 main.cpp:36 - CRD Engine v0.1.0 starting up
2026-04-26 01:09:37.377 [INF] [Engine] tid=17877431896815509209 main.cpp:37 - platform=WINDOWS compiler=MSVC arch=X64
2026-04-26 01:09:37.377 [TRC] [Engine] tid=17877431896815509209 main.cpp:40 - trace: this is the chattiest level
2026-04-26 01:09:37.377 [DBG] [Engine] tid=17877431896815509209 main.cpp:41 - debug: useful while building things
2026-04-26 01:09:37.377 [INF] [Renderer] tid=17877431896815509209 main.cpp:43 - Renderer initialised, backend=Vulkan
2026-04-26 01:09:37.377 [DBG] [Physics] tid=17877431896815509209 main.cpp:44 - physics tick budget = 4 ms
2026-04-26 01:09:37.377 [WRN] [Audio] tid=17877431896815509209 main.cpp:45 - no audio device found, falling back to silence
2026-04-26 01:09:37.377 [INF] [Renderer] tid=4857446244092308000 main.cpp:56 - frame 0 submitted
2026-04-26 01:09:37.377 [DBG] [Physics] tid=295193356635212754 main.cpp:65 - stepped 32 bodies
2026-04-26 01:09:37.380 [DBG] [Physics] tid=295193356635212754 main.cpp:65 - stepped 33 bodies
2026-04-26 01:09:37.380 [INF] [Renderer] tid=4857446244092308000 main.cpp:56 - frame 1 submitted
2026-04-26 01:09:37.383 [DBG] [Physics] tid=295193356635212754 main.cpp:65 - stepped 34 bodies
2026-04-26 01:09:37.383 [INF] [Renderer] tid=4857446244092308000 main.cpp:56 - frame 2 submitted
2026-04-26 01:09:37.386 [CRT] [Engine] tid=17877431896815509209 main.cpp:73 - fatal subsystem failure simulated -- shutting down
2026-04-26 01:09:37.386 [ERR] [Renderer] tid=17877431896815509209 main.cpp:72 - shader compile failed: 'tonemap.frag'
```

Things to notice:

- **Three thread ids** show up: the main thread plus two workers. The workers
  interleave with the main timeline, demonstrating that the logger is genuinely
  thread-safe.
- **The Audio Info-level call did NOT appear.** Look at `main.cpp:48`:
  `CRD_LOG_INFO(g_log_audio, "(this should NOT appear)")`. Audio's runtime
  level is `Warn`, so `should_log` returns false at the call site, and `std::format`
  never even runs. Money well spent.
- **`[CRT]` appears BEFORE `[ERR]`** even though source-order is the opposite.
  This is the Critical bypass in action: it took the synchronous path in
  `dispatch()` and was delivered to disk before the queued Error record reached
  the worker. Exactly what you want when you're crashing.

---

## 10. Tests reference

`tests/log/test_log.cpp` covers:

| Test | Verifies |
|---|---|
| `LogLevel string round-trip` | `to_string` / `from_string` agree |
| `Channel is registered and discoverable` | The registrar runs on startup |
| `Sync logging delivers to sinks` | Basic happy path |
| `Channel runtime level filters at producer` | The per-channel cutoff works |
| `Per-sink min_level filters at consumer` | The per-sink cutoff works |
| `Async logging delivers all records` | 4 threads × 100 messages = 400 received |
| `RingBufferSink overwrites oldest` | Capacity contract |
| `FileSink writes to disk and rotates` | Disk + rotation policy |
| `Critical bypasses async queue` | Critical synchronously delivered |
| `NullSink accepts records without crashing` | Lifetime / ctor |
| `set_all_channels_level affects every channel` | Bulk update |

All 13 (including the 2 inherited core tests) pass with `ctest --preset win-debug`.

---

## 11. What this gives me, in one sentence

A way to scatter `CRD_LOG_INFO(...)` calls anywhere in the engine, with the
confidence that disabled levels cost effectively nothing, that enabled levels
never block the game thread, and that I can re-route the entire stream to disk,
console, or an in-game overlay without touching call sites.
