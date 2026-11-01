# Shewchuk's predicates: the differential test's oracle

Jonathan Richard Shewchuk's `shewchuk-predicates.c` (`predicates.c` of May 18, 1996, placed in the public domain; its
attribution stays in the source) is kept verbatim but for one repair. Its four expansion sums (`fast_expansion_sum`,
`linear_expansion_sum` and their `_zeroelim` forms) read the next component after each step, so after the last one
they read one past the expansion's end: 28 reads, which AddressSanitizer reports as a stack-buffer-overflow
(`linux-gcc-arm64-asan` and `win-asan`, run 41295647725; reproduced on `linux-gcc-asan`). The value read there is
never used, since every use is behind an `eindex < elen` or `findex < flen` test, so each read is now
`(++eindex < elen) ? e[eindex] : 0.0` (and `f` alike): the same results, no read out of bounds. It is the oracle of
`test_predicates_vs_shewchuk.cpp`, which holds crd-geometry-primitives' adaptive `orient2d`, `orient3d`, `incircle`
and `insphere` to the signs of Shewchuk's adaptive and exact predicates on every tree that builds the tests.
`CMakeLists.txt` beside this directory compiles it as the `crd-geometry-shewchuk-oracle` object library under the
tree's floating-point contract.

`win32/sys/time.h` stands in for the POSIX header the source includes and never uses, which the Windows SDK lacks.
