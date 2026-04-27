# Session — 2026-04-27 — crd-platform v1d (filesystem + dynlib + threading)

## Goal

Close `crd-platform` Phase 1 with the remaining OS-service slice:
filesystem, dynamic library loading, and a deliberately small threading
helper surface. Keep the public API backend-agnostic and narrow; ship
tests and green builds across Debug / Release / ASan.

## What we built / changed

- **`engine/platform/include/crd/platform/filesystem.hpp`** — new
  `crd::platform::fs` namespace:
  - `Path` UTF-8 `String`-backed wrapper with forward-slash canonical
    internal form
  - roots: `current_working_dir()`, `executable_dir()`,
    `user_config_dir(app_name)`
  - metadata: `exists`, `is_file`, `is_directory`, `file_size`,
    `last_modified_unix_seconds`
  - sync I/O: `read_file_text`, `read_file_binary`, `write_file_text`,
    `write_file_binary`
  - directory ops: `create_directories`, `remove_file`, `remove_all`,
    `list_directory`
- **`engine/platform/src/filesystem.cpp`** — implementation on top of
  `<filesystem>` plus UTF-8↔wide conversion on Windows so filesystem
  calls don't depend on the host ANSI codepage.
- **`engine/platform/include/crd/platform/dynamic_library.hpp`** — new
  move-only RAII wrapper: `open(Path)`, `resolve`, `resolve_as<Fn>`,
  `is_valid`.
- **`engine/platform/src/dynamic_library.cpp`** — `LoadLibraryW` /
  `GetProcAddress` on Windows, `dlopen` / `dlsym` on POSIX.
- **`engine/platform/include/crd/platform/threading.hpp`** — small helper
  surface: naming, thread id, concurrency counts, affinity, pause.
- **`engine/platform/src/threading.cpp`** — portable baseline
  implementation. `current_thread_id` uses a hashed `std::thread::id`,
  counts come from `std::thread::hardware_concurrency`, naming is a
  best-effort no-op, affinity returns false, pause yields. The point of
  this slice is to lock the API shape before `crd-jobs` arrives, not to
  prematurely build a scheduler.
- **`runtime/examples/smoke_filesystem.cpp`** — cwd / executable-dir /
  config-dir sanity print.
- **`tests/platform/test_filesystem.cpp`** — path normalization,
  cwd/exe-dir roots, text roundtrip, binary roundtrip, directory listing,
  config-dir app-name append.
- **`tests/platform/test_dynamic_library.cpp`** — invalid-instance
  behaviour, system-library load + symbol resolve, move semantics.
- **`tests/platform/test_threading.cpp`** — basic sanity for counts,
  naming, pause, and affinity's stable boolean contract.
- **Docs**: platform system overview and ROADMAP status / decision log /
  re-entry notes updated.

## Plain-English explanation

`crd-platform` now covers the boring but essential OS services that the
rest of the engine will lean on. It can tell you where the program is
running from, read and write files, inspect directories, and load a DLL /
shared object by name. It also has a tiny threading facade so the rest of
the engine can ask "what thread am I on?", "how many cores do I have?",
and use a future-proof naming / affinity API before the job system exists.

The important architectural point is that none of this leaks raw
platform-specific types into headers. Files are addressed through
`fs::Path`, dynamic libraries through a move-only wrapper, and threading
through a narrow namespace-level API. That keeps the next phases free to
swap implementations without dragging Win32 details into every module.

## Decisions made

- **`<filesystem>` internal, custom `Path` external.** Battle-tested
  implementation underneath; Cerid-owned UTF-8 type at the public edge.
- **Path canonical form is forward-slash UTF-8.** OS-native conversions
  happen only inside the platform module.
- **Threading surface is API-first, implementation-second.** The job
  system is the first real consumer with hard requirements; until then,
  baseline behaviour is better than brittle SDK-specific code.
- **Dynamic-library tests use a known system library**, not a bespoke test
  DLL target. Keeps the test graph smaller.
- **ASan is the final arbiter for string/path edge cases.** It caught the
  `Path::operator/` overflow that Debug and Release both missed.

## Files touched

- `engine/platform/include/crd/platform/filesystem.hpp` — new
- `engine/platform/include/crd/platform/dynamic_library.hpp` — new
- `engine/platform/include/crd/platform/threading.hpp` — new
- `engine/platform/include/crd/platform/platform.hpp` — umbrella updated
- `engine/platform/include/crd/platform/window.hpp` — `poll_input()`
  comment corrected to match actual frame order
- `engine/platform/src/filesystem.cpp` — new
- `engine/platform/src/dynamic_library.cpp` — new
- `engine/platform/src/threading.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_filesystem`
- `runtime/examples/smoke_filesystem.cpp` — new
- `tests/platform/test_filesystem.cpp` — new
- `tests/platform/test_dynamic_library.cpp` — new
- `tests/platform/test_threading.cpp` — new
- `docs/systems/platform.md` — v1d shipped state
- `docs/ROADMAP.md` — status, decision log, Where I left off

## Tests / verification

- `win-debug`: 191/191
- `win-release`: 190/190 (Debug-only stats test correctly skipped)
- `win-asan`: 191/191, no leaks, no UAF, no OOB
- ASan specifically caught and verified the fix for `Path::operator/`
  overflow

## Next session starts with

Phase 1 closeout: retrospective, CONTEXT sweep, bench refresh, and mark
Phase 1 complete. Then move into `crd-app` v1a (Hazel-style Event base
+ `EventDispatcher`).
