# catch2-posix-signal-refault — applied by crd_patched_copy() to
# Catch2/src/catch2/internal/catch_fatal_condition_handler.cpp (3.7.1).
#
# Catch2's POSIX fatal-condition handler reports the case, restores the previous handlers and re-raises every signal.
# A re-raised signal is a sent one: the next handler (a sanitizer's, or the default action) gets SI_TKILL's siginfo,
# whose si_addr slot holds the sender's pid and uid. So every fault inside a test case on a sanitizer lane was reported
# at an address like 0x03e800000a89 (uid 1000, the process's own pid) instead of the faulting one: the C2 flight
# recorder batch's SEGV and the 2026-10-25 TSan host faults (docs/sessions/2026-10-26-catch2-fault-address.md). The
# handler now takes SA_SIGINFO, and a SIGSEGV, SIGILL or SIGFPE the kernel raised (si_code > 0) returns after the
# report: the instruction executes again under the restored handler, which receives the real siginfo and context, as
# crd's own crash handler does (engine/foundation/core/src/crash.cpp). A sent signal is still re-raised. Windows (SEH)
# compiles the copy to the upstream text. ThreadSanitizer trees do not compile the POSIX handler at all
# (CATCH_CONFIG_NO_POSIX_SIGNALS, set in the root CMakeLists.txt).
crd_patch_replace(
    [[    static void handleSignal( int sig ) {]]
    [[    // crd-patch catch2-posix-signal-refault: a fault the kernel raised re-executes under the restored handler
    // (cmake/patches/catch2-posix-signal-refault.cmake)
    static void handleSignal( int sig, siginfo_t* info, void* ) {]])
crd_patch_replace(
    [[        reportFatal( name );
        raise( sig );]]
    [[        reportFatal( name );
        const bool fault = sig == SIGSEGV || sig == SIGILL || sig == SIGFPE;
        if ( fault && info != nullptr && info->si_code > 0 ) {
            return;
        }
        raise( sig );]])
crd_patch_replace(
    [[        sa.sa_handler = handleSignal;
        sa.sa_flags = SA_ONSTACK;]]
    [[        sa.sa_sigaction = handleSignal;
        sa.sa_flags = SA_ONSTACK | SA_SIGINFO;]])
