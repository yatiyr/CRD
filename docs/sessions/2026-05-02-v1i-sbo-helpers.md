# Session: crd-jobs v1i — SBO lambda helpers + parallel_for

**Date:** 2026-05-02  
**Slice:** v1i  
**Branch:** main

---

## What was built

Implemented `make_job<F>()` (41-byte SBO) and `parallel_for()` — the callable-wrapping helpers for the public jobs API.

### SBO design

`JobDecl` is a 64-byte value type copied through Chase-Lev deques and Vyukov MPMC queues. The SBO stores inline callable bytes in the existing fields:

| Field | Bytes | Role |
|---|---|---|
| `data` | 8 | SBO bytes 0–7 |
| `_pad[8]` | 1 | `kSboFlag = 1u` — marks SBO jobs |
| `_pad[9..41]` | 33 | SBO bytes 8–40 |

Total SBO capacity: **41 bytes** (reduced from phase-doc's provisional 48 because v1h already reserved `_pad[0..7]` for `Counter*`).

### Queue-copy safety

The core problem: `JobDecl::data` in an SBO job would point into `_pad` of the *original* JobDecl. After the queue copies the value, that pointer dangles.

Fix: `run_job_in_fiber` checks `_pad[8] == kSboFlag` and copies the 41 SBO bytes (`data` + `_pad[9..41]`) into the acquiring fiber's `sbo_buf` field before the first context switch. The `Fiber` struct persists across suspension + resume on any thread — it is the stable storage.

New `Fiber` field:

```cpp
alignas(8) crd::u8 sbo_buf[41] = {};
```

`sbo_trampoline<F>` (per-type static fn stored in `JobDecl::fn`) receives `sbo_buf` as its `data` argument and calls `operator()` on the buffered `F`.

### Constraints

```cpp
requires (sizeof(std::decay_t<F>)  <= 41u &&
          alignof(std::decay_t<F>) <= 8u  &&
          std::is_trivially_copyable_v<std::decay_t<F>> &&
          std::is_trivially_destructible_v<std::decay_t<F>>)
```

`is_trivially_copyable_v` is required in addition to `is_trivially_destructible_v` because the callable is moved through queues via `memcpy`.

### parallel_for

Creates `min(num_jobs, count)` SBO jobs via `make_job`. Each job wraps a `Task{begin, end, FD fn}` struct. Uses `std::vector<JobDecl>` for the job array; v1j replaces this with the frame arena.

---

## Files changed

- `engine/jobs/include/crd/jobs/job_decl.hpp` — `_pad` layout comment + `detail::kSboFlag`
- `engine/jobs/src/fiber.hpp` — added `sbo_buf[41]` field
- `engine/jobs/include/crd/jobs/jobs.hpp` — `detail::sbo_trampoline<F>`, `make_job<F>`, `parallel_for<F>`
- `engine/jobs/src/worker_pool.cpp` — SBO detection + `sbo_buf` copy in `run_job_in_fiber`
- `tests/jobs/test_jobs.cpp` — 5 new tests (tests 16–20)
- `docs/phases/phase-2.5-jobs.md` — slice table + SBO design-section updated
- `context.md` — test counts, shipped milestone

---

## Tests added (tests 16–20)

| # | Name | What it proves |
|---|---|---|
| 16 | make_job basic SBO lambda | 8-byte capture; simple dispatch |
| 17 | make_job SBO survives fiber suspension | SBO bytes persist across `counter_wait` + resume on a different thread |
| 18 | make_job captures struct by value | Struct `Payload{int addend, atomic<int>* out}` round-trips through SBO |
| 19 | parallel_for splits range and executes all items | 100-item sum across 4 jobs = 4950 |
| 20 | parallel_for clamps num_jobs to count | 3 items, 10 jobs clamped to 3 — no zero-range job dispatched |

Test 17 is the key regression guard: it exercises the `data`-pointer-dangle scenario that the `sbo_buf` design prevents. Under ASan it would trap immediately if the old "point into JobDecl" approach were used.

---

## Six-configuration results

| Config | Tests |
|---|---|
| win-debug | 351/351 ✅ |
| win-release | 348/348 ✅ |
| win-relwithdebinfo | green ✅ |
| win-asan | green ✅ |
| win-clang-cl | green ✅ |
| win-tidy | green ✅ |

---

## Decisions

- SBO capacity is 41 bytes, not 48 (phase-doc provisional). `_pad[0..7]` is owned by v1h (Counter*); `_pad[8]` by v1i (kSboFlag). Documented in job_decl.hpp.
- `is_trivially_copyable_v` added as a constraint — the phase doc only listed `is_trivially_destructible_v` but memcpy-through-queue transfer requires both.
- `parallel_for` uses `std::vector` for the job array temporarily; v1j frame arena replaces this.

---

## Next

**v1j** — per-thread linear frame allocator: `frame_alloc()` / `frame_reset()`. Defined in `docs/phases/phase-2.5-jobs.md`.
