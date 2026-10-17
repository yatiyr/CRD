# zstd-huf-is-sorted-unused — applied by crd_patched_copy() to zstd/lib/compress/huf_compress.c (1.5.5).
#
# HUF_isSorted is a MEM_STATIC helper used only inside assert(HUF_isSorted(...)), so a build without asserts never
# references it. zstd's mem.h gives MEM_STATIC __attribute__((unused)) only under __GNUC__, and clang-cl (clang in MSVC
# mode) does not define __GNUC__: the helper drew -Wunused-function on every win-clang-cl-shipping build. The one
# replacement gives it the attribute exactly where upstream's own MEM_STATIC leaves it out (clang without __GNUC__);
# MSVC and GCC preprocess the copy to the upstream text. The anchor is upstream's definition line; one that stops
# matching fails the configure (see docs/design/pinned-inputs.md).
crd_patch_replace(
    [[MEM_STATIC int HUF_isSorted(nodeElt huffNode[], U32 const maxSymbolValue1) {]]
    [[#if defined(__clang__) && !defined(__GNUC__) /* crd-patch: clang-cl's MEM_STATIC lacks the unused attribute */
__attribute__((unused))
#endif
MEM_STATIC int HUF_isSorted(nodeElt huffNode[], U32 const maxSymbolValue1) {]])
