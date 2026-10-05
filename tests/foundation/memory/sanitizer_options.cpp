// DIAG.3a: the allocator boundary sweep asks MallocAllocator for sizes and alignments no system can satisfy and checks
// that try_allocate returns nullptr. Under AddressSanitizer and ThreadSanitizer the runtime's allocator aborts on such
// a request ("allocation-size-too-big") instead of returning null as libc does. allocator_may_return_null=1 restores
// the libc contract for this test executable only. It changes how an impossible request fails, not what the sanitizer
// detects; every memory-error and data-race check stays on.

#if defined(__SANITIZE_ADDRESS__)
#define CRD_MEMORY_TESTS_ASAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_MEMORY_TESTS_ASAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define CRD_MEMORY_TESTS_TSAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRD_MEMORY_TESTS_TSAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif
#endif

#if defined(CRD_MEMORY_TESTS_ASAN)
extern "C" const char* __asan_default_options()
{
    return "allocator_may_return_null=1";
}
#endif

#if defined(CRD_MEMORY_TESTS_TSAN)
extern "C" const char* __tsan_default_options()
{
    return "allocator_may_return_null=1";
}
#endif
