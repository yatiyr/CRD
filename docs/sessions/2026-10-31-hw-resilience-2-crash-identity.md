# 2026-10-31 — HW-RESILIENCE.2: the machine in a native crash

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2), its second part: the hardware identity in the fatal
crash path. Design: [runtime diagnostics](../design/runtime-diagnostics.md#hw-resilience--hardware-fault-resilience).
Before this, flight bundles named the machine but a native crash (a Windows dump, a Linux crash record) did not.

## The seam (crd-core)

crd-core cannot see crd-perf, so `crd::crash` takes bytes, not the identity: `set_machine_stream(bytes, size)` (at
most `kMaxMachineStreamBytes`, 20 KiB) copies them for every later dump and record.

- Windows: each dump, fatal and live, carries them as the user stream `CRDM` (`kMachineStreamType`); a hang dump
  carries its `CRDD` evidence beside it. The fatal path passed no user stream before.
- Linux: the record's text is assembled in an 8 KiB buffer on the alternate stack, too small for a 32 KiB hex line,
  so the setter formats `machine <hex>\n` once and the handler writes it as one more write before `fsync`.
- Replacement (the GPU entries arrive after the install) uses two slots. A writer pins the published slot (bounded,
  lock-free, async-signal-safe); the setter fills the slot no writer holds and publishes it.

## The identity (crd-perf)

- `kHardwareIdentityMaxBytes` is the encoder's largest output (16,438 bytes); a `static_assert` holds it to the core
  cap and a case encodes a full identity to exactly that size.
- `attach_hardware_identity_to_crashes()` runs where the handler is installed (`DiagnosticHost::start`, the bare
  `Application` path), so the OS is read there, never on the fatal path.
- `read_crash_hardware_identity(capture)` decodes either capture on any platform; `find_minidump_stream` now walks a
  dump's directory for it and for both module parsers (their two copies of the walk are deleted).

## Verification

- win-debug: `crd-core-tests [crash],[crash-capture]` and `crd-perf-tests [hw-resilience],[diag-host],[symbols]`
  pass. The crash-capture specimen sets a full 20 KiB stream in every mode; the fatal dump carries it whole, the
  live-dump contract case checks replacement, refusal and removal, and the diagnostic host's application crash
  decodes to this machine (`same_hardware_identity` against the host).
- linux-gcc-debug (WSL): the same cases pass; the record ends with the whole machine line. Strict tidy is clean on
  the touched files there; it found three pre-existing findings in them (a duplicate `<cstring>`, `fenv.h` and
  `signal.h`), all fixed.
- Teeth, each restored and rerun green: a Windows writer without the machine stream failed the fatal-dump, contract
  and host cases; a Linux writer without the machine line failed the record case and the host case; a decoder that
  read `B` as `b` failed the record refusal case (its first version uppercased a digit, so it could not fail; fixed).
- Seed `linux-crash-record-truncated` follows the writer's new `ok` line; DETECTED on linux-gcc-debug.
- The ctest guards pass on win-debug.

Hosted once pushed: the `[crash-capture]`, contract, `[hw-resilience]` and host crash cases on every lane.
