# Windows verification — crd-jobs fiber-runtime hardening (2026-05-12)

The four fiber-job-system fixes from `docs/sessions/2026-05-12-jobs-fiber-tls-hoist-fix.md`
(TLS-hoist `CRD_JOBS_TLS_OPAQUE`, stale-`tl_fiber` clear, `counter_wait` switch-then-publish,
`counter_decrement` drain-only-at-zero) were developed and verified on a Linux dev VM. They are
plain C++ + one portable attribute macro — no platform-specific codegen — but the **Windows half
of the Definition-of-Done sweep was not run there**. Run the steps below on your Windows box and
record the results in the session log.

## Why a Windows check matters here

- `engine/jobs/CMakeLists.txt` still pins `worker_pool.cpp` + `fiber_init.cpp` to `/Od` on MSVC
  (was the original masking of bug #1). The source fix makes that unnecessary; once Windows is
  green you can delete that `if(MSVC) set_source_files_properties(... /Od /Y- ...)` block — but
  **only after** running the sweep below with it removed too.
- `CRD_JOBS_TLS_OPAQUE` resolves to `CRD_NOINLINE` (`__declspec(noinline)`) on MSVC/clang-cl, not
  `noipa` (which doesn't exist there). MSVC LTCG / clang-cl thin-LTO could in principle still CSE
  the accessor — `crd-jobs` already has `INTERPROCEDURAL_OPTIMIZATION OFF` and the trampoline
  TU's `/Od` covers it today, so this is belt-and-suspenders on top of belt-and-suspenders, but
  worth a sanity disasm.
- The `Waiter` struct grew two members (`claim`, `park_finalized`); nothing pins its size, but
  re-run `win-tidy` for the naming/lint check on the changed files.

## Steps

All from `D:\Dev\cerid` in PowerShell 7 (`$env:VULKAN_SDK` set as usual):

1. **Full nine-config sweep** (`scripts/full-sweep.ps1` covers Win × 9 + Linux × 6):
   ```powershell
   .\scripts\full-sweep.ps1
   ```
   Must come back all-PASS. The configs that matter most for this change:
   `win-release`, `win-relwithdebinfo`, `win-clang-cl`, `win-asan` (build + ctest), `win-tidy`
   (build), and the three `*-shipping` configs (full LTO — exercises bug #1's optimizer surface).
   If you only have time for a subset: `win-release`, `win-relwithdebinfo`, `win-clang-cl`,
   `win-asan`, `win-tidy`.

2. **Hammer the regression test** under each optimised config (the bug was a tight race; one run
   isn't enough):
   ```powershell
   $exe = "D:\Dev\cerid\build\win-release\tests\jobs\crd-jobs-tests.exe"   # repeat for win-relwithdebinfo, win-clang-cl, win-asan
   1..500 | % { & $exe "jobs: cross-thread fiber resume stress" *> $null; if ($LASTEXITCODE -ne 0) { Write-Host "FAIL on iter $_ (exit $LASTEXITCODE)"; break } }
   ```
   (On Linux this exact test passed 300× and a much heavier standalone variant ~9,000×.)
   Optionally, an even heavier soak with more threads/rounds: build a small driver like the one
   used on Linux (`crd::jobs::init({.num_threads=8}); for r in 0..200: run_and_wait(span of N
   roots, each run_and_wait-ing on M children); shutdown;`) and loop it a few hundred times under
   `win-release`.

3. **Sanity disasm** of `job_fiber_trampoline` in the optimised build (optional but cheap):
   ```powershell
   dumpbin /disasm:nobytes "D:\Dev\cerid\build\win-release\engine\jobs\crd-jobs.dir\worker_pool.cpp.obj" | Select-String "job_fiber_trampoline" -Context 0,80
   ```
   Confirm the scheduler-context address fed to the `fiber_switch` at the bottom of the loop comes
   from a fresh `call`/`__declspec(noinline)`-call (not a value hoisted into a callee-saved reg
   before the job call). Note: with `worker_pool.cpp` still at `/Od` this is trivially true; the
   interesting check is *after* you remove the `/Od` (step 4).

4. **(Optional follow-up) Remove the MSVC `/Od` and re-verify.** Delete the `if(MSVC) ... /Od ...`
   block in `engine/jobs/CMakeLists.txt`, then redo steps 1–3. If everything stays green, that
   block is gone for good; if anything regresses, restore it and note it in `docs/debt.md`.

## Record the result

Append to `docs/sessions/2026-05-12-jobs-fiber-tls-hoist-fix.md` (the "Verification" section has a
`<fill in exact counts>` placeholder for the Linux sweep too) and, if the `/Od` removal in step 4
sticks, update the comment in `engine/jobs/CMakeLists.txt` and the note in `docs/debt.md`.
