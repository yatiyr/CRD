# 2026-11-08: NET.3, training a channel's models

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3) (stays Partial). Track C batch 118. Design: [S32](../design/network-foundation.md#body-coding).
The user's order this run ("do not regress anything") is why the clang lanes' break, from the rewrite agent's
commit in this track's file, was fixed first; the rewrite itself stays that agent's.

## CI first

Dispatch 42413677756 (d7b506c8): win-clang-cl, linux-clang-tsan and both clang ARM64 lanes failed to compile
`fuzz_link.cpp` (`-Wunused-lambda-capture` on `far`, a capture 3dda87d6 kept from 3f34263b). Fixed and pushed first
(2e5c900a): `far` is `constexpr` and the lambda captures nothing, as the same file's `later` already did on every lane.
Rebuilt and replayed on linux-clang-fuzz. Those lanes ran none of their tests, so the coded bodies wait for the next run.

## What landed

- `ceridc model train` and `ceridc model measure` (`tools/ceridc/src/model.cpp`): samples are a directory of messages,
  one per file in name order; entropy through `EntropyTrainer`, dictionaries through `zstd_train_dictionary`; every
  fifth file held out by default and measured apart; the model loaded through `BodyModel` and every sample coded and
  decoded by `BodyCoder` as a writer would; the report gives the hash, sent against plain bytes per set, the widest
  ratio, the schema's `models` entry (`max_ratio` the least power of two above that ratio) and `small-corpus` when a
  dictionary had under ten sample bytes per byte of room. Written last, whole or not at all; typed refusals.
- `read_decimal` moved from `policy.cpp` to `tools/ceridc/src/decimal.hpp`, shared by both verbs.
- `coding_fixture.hpp`: `state_delta_samples()`, which the pinned state model and the tool's test both train on.

## Evidence

- `test_ceridc_model.cpp`, 1,630 assertions in 5 cases, on linux-gcc-debug and win-clang-cl (the real binary on each):
  trained on `state_delta_samples()` with nothing held out, the tool writes the model `coding_test.json` pins byte for
  byte (`6d3632b1a98c5ac0`); measuring the raw note dictionary reports `869106d3867138d3`; sent bytes equal a direct
  `BodyCoder` run; training twice gives the same dictionary; which file is held out; the message bound at its edge;
  noise under an English model stored, and `pays` judged on the held-out share; 25 typed refusals, none writing a model.
- The whole of `crd-ceridc-tests` (49 cases) and `crd-net-tests` (320,876 assertions, unchanged by the fixture split),
  and the `crd-check-*` and `crd-no-*` guards on linux-gcc-debug. Strict clang-tidy 20 clean on every changed file.
- Source mutations of `model.cpp`: a first round of 8, 4 caught, 1 refused by the compiler, 3 survived (the
  small-corpus bound, the message bound loosened, `pays` judged on the training share). Each gained a test; a second
  round of 5 (those three, the message bound by one, the holdout shifted by one file): all caught.

## Next

NET.3's budget board against the best serializers, then its close.
