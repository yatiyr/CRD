# zstd-lazy-is-aligned-unused — applied by crd_patched_copy() to zstd/lib/compress/zstd_lazy.c (1.5.7).
#
# ZSTD_isAligned is a MEM_STATIC helper used only inside the two assert(ZSTD_isAligned(...)) checks of
# ZSTD_row_prefetch, so a build without asserts never references it. zstd's compiler.h gives MEM_STATIC
# __attribute__((unused)) only under __GNUC__, and clang-cl (clang in MSVC mode) does not define __GNUC__: the helper
# drew -Wunused-function on every win-clang-cl-shipping build. The one replacement gives it the attribute exactly where
# upstream's own MEM_STATIC leaves it out (clang without __GNUC__); MSVC and GCC preprocess the copy to the upstream
# text. The anchor is upstream's definition line; one that stops matching fails the configure (see
# docs/design/pinned-inputs.md).
crd_patch_replace(
    [[MEM_STATIC int ZSTD_isAligned(void const* ptr, size_t align) {]]
    [[#if defined(__clang__) && !defined(__GNUC__) /* crd-patch: clang-cl's MEM_STATIC lacks the unused attribute */
__attribute__((unused))
#endif
MEM_STATIC int ZSTD_isAligned(void const* ptr, size_t align) {]])
