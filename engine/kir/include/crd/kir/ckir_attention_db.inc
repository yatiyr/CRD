// ckir_attention_db.inc — CHECKED-IN flash-attention (BR,BC) tuning DB (AS-2-for-attention). Rows are {device, S, D, BR, BC},
// each the on-device measured winner from the time_attention sweep (enumerate → GPU-event-time all valid tiles → oracle-validate →
// pick fastest), replayed by lookup_attention_tuned so run() emits the tuned flash kernel with NO runtime search. Regenerate by
// re-running the [attention] autotune sweep (tests/kir-cuda/test_autotune_cuda.cpp) on the target device and transcribing winners.
// Device: RTX 4070 Ti SUPER (Ada, sm_89). BR=128 (wide query block) beats the fixed BR=64 default across the S sweep, D∈{32,64}.
// ⚠ Other GPUs need their own rows (measure on the target hardware — only sm_89 available this session). D=128 spills the per-thread
// q[D]+acc[D] accumulator (>255 regs) ⇒ it runs but needs the warp-collaborative kernel for a competitive tile (future).
{"sm_89", 512, 32, 128, 32},
{"sm_89", 1024, 32, 128, 32},
{"sm_89", 2048, 32, 128, 32},
{"sm_89", 4096, 32, 128, 32},
{"sm_89", 512, 64, 128, 32},
{"sm_89", 1024, 64, 128, 32},
{"sm_89", 2048, 64, 128, 32},
{"sm_89", 4096, 64, 128, 32},
