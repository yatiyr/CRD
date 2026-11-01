# Shewchuk's predicates: the differential test's oracle

Jonathan Richard Shewchuk's `shewchuk-predicates.c` (`predicates.c` of May 18, 1996, placed in the public domain; its
attribution stays in the source) is kept verbatim but for two repairs. Its four expansion sums (`fast_expansion_sum`,
`linear_expansion_sum` and their `_zeroelim` forms) read the next component after each step, so after the last one they
read one past the expansion's end: 28 reads, which AddressSanitizer reports as a stack-buffer-overflow
(`linux-gcc-arm64-asan` and `win-asan`, run 41295647725; reproduced on `linux-gcc-asan`). The value read there is never
used, since every use is behind an `eindex < elen` or `findex < flen` test, so each read is now `(++eindex < elen) ?
e[eindex] : 0.0` (and `f` alike): the same results, no read out of bounds. Its 42 compiled definitions were K&R ones (an
identifier list and declarations, or an empty list), which C23 removes and Clang reports on every lane
(`-Wdeprecated-non-prototype`, 33 lines per job on the five Clang jobs of run 41295647725); each is now the prototyped
definition of the same types, and the six tail lengths of `incircleadapt` start at zero, which GCC's
`-Wmaybe-uninitialized` asked for (each is written before it is read). This repair changes no expression;
`floatprint`, the one definition whose K&R parameter was promoted (`float`), is never called. The commented-out
`doubleprint` keeps its original form. It is the oracle of `test_predicates_vs_shewchuk.cpp`, which holds
crd-geometry-primitives' adaptive `orient2d`, `orient3d`, `incircle` and `insphere` to the signs of Shewchuk's adaptive
and exact predicates on every tree that builds the tests. `CMakeLists.txt` beside this directory compiles it as the
`crd-geometry-shewchuk-oracle` object library under the tree's floating-point contract and with its warnings, as errors.

`win32/sys/time.h` stands in for the POSIX header the source includes and never uses, which the Windows SDK lacks.
