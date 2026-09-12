#pragma once

namespace crd::crash
{

// Install the platform crash handler. Creates output_dir if it does not exist.
//
// Windows: SetUnhandledExceptionFilter → MiniDumpWriteDump
//          Writes a timestamped .dmp file openable in VS / WinDbg with the
//          matching PDB.  ExceptionCode + ExceptionAddress are also printed
//          to stderr for immediate visibility in CI logs and terminal runs.
//
// Linux:   sigaction(SIGSEGV | SIGABRT | SIGFPE | SIGILL)
//          Writes a crash log (signal, faulting address, backtrace) then
//          re-raises with the default handler so the OS writes a core dump.
//
// Safe to call multiple times; each call replaces the output directory and
// re-registers the handler.  The previous handler is saved and restored by
// uninstall().
void install(const char* output_dir = "./crashes") noexcept;

// Restore whichever handler was active before the last install() call.
// Called automatically by Application::~Application().
void uninstall() noexcept;

} // namespace crd::crash
