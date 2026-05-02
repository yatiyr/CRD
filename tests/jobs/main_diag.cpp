#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>

static LONG CALLBACK diag_exception_handler(EXCEPTION_POINTERS* ep) noexcept
{
    auto* rec = ep->ExceptionRecord;
    std::fprintf(stderr,
                 "[fiber-diag] ExceptionCode=0x%08lX ExceptionAddress=%p\n",
                 static_cast<unsigned long>(rec->ExceptionCode),
                 rec->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, &diag_exception_handler);
#endif
    return Catch::Session().run(argc, argv);
}
