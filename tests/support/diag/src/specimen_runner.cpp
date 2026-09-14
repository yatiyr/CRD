// crd-diag-harness -- specimen runner implementation (DIAG.0).
//
// Portable bounded child-process execution. Windows uses CreateProcess + a stdout
// pipe + WaitForSingleObject(timeout); POSIX uses fork/exec + a stdout pipe +
// waitpid(WNOHANG) polling with SIGKILL on timeout. Timeout is the mandatory bound;
// an address-space rlimit is deliberately NOT set, because ASan reserves terabytes
// of shadow space and RLIMIT_AS would make the sanitizer positive fail mysteriously
// (Outcome::memory_bound_enforced records this honestly).
//
// Engine containers only (docs/CODING.md): crd::containers::String / Array. The
// only std facilities used are the permitted non-owning std::string_view (for
// scanning the child's stdout) and C stdio/OS syscalls at the process boundary.

#include <crd/diag/specimen_runner.hpp>

#include <chrono>
#include <cstdio>
#include <string_view>
#include <sys/stat.h>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crd::diag
{

namespace cont = crd::containers;

const char* verdict_name(Verdict v)
{
    switch (v)
    {
    case Verdict::Clean: return "Clean";
    case Verdict::Crashed: return "Crashed";
    case Verdict::SanitizerCaught: return "SanitizerCaught";
    case Verdict::InstrumentAbsent: return "InstrumentAbsent";
    case Verdict::MissingSymbolizer: return "MissingSymbolizer";
    case Verdict::ZeroSelection: return "ZeroSelection";
    case Verdict::DeniedOutput: return "DeniedOutput";
    case Verdict::MismatchedBinary: return "MismatchedBinary";
    case Verdict::MissingExecutable: return "MissingExecutable";
    case Verdict::Timeout: return "Timeout";
    case Verdict::Unexpected: return "Unexpected";
    }
    return "Unexpected";
}

namespace
{

void append_int(cont::String& s, long long value)
{
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%lld", value);
    if (n > 0)
    {
        s.append(buf, static_cast<crd::usize>(n));
    }
}

bool path_exists(const cont::String& p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

bool is_directory(const cont::String& p)
{
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0)
    {
        return false;
    }
#if defined(_WIN32)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

// Try to create + write + remove a probe file inside `dir`. Returns false when the
// directory is missing, is actually a file, or cannot be written -- the DeniedOutput
// signal. Using a file-as-directory (the negative control) fails on every platform,
// unlike a read-only directory which an elevated user can still write.
bool output_writable(const cont::String& dir)
{
    if (!path_exists(dir) || !is_directory(dir))
    {
        return false;
    }
    cont::String probe = dir;
    probe.append("/.crd-diag-write-probe");
    std::FILE* f = std::fopen(probe.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool wrote = std::fputc('x', f) != EOF;
    std::fclose(f);
    std::remove(probe.c_str());
    return wrote;
}

// Scan the child's stdout (a permitted non-owning view) for the two echoed tags.
cont::String grab_tag(std::string_view out, std::string_view key)
{
    std::string_view::size_type pos = out.find(key);
    while (pos != std::string_view::npos)
    {
        const bool at_line_start = (pos == 0) || (out[pos - 1] == '\n') || (out[pos - 1] == '\r');
        if (at_line_start)
        {
            const std::string_view::size_type start = pos + key.size();
            std::string_view::size_type end = out.find_first_of("\r\n", start);
            if (end == std::string_view::npos)
            {
                end = out.size();
            }
            const std::string_view value = out.substr(start, end - start);
            return cont::String{value};
        }
        pos = out.find(key, pos + 1);
    }
    return cont::String{};
}

struct ChildResult
{
    bool            spawned = false;
    bool            timed_out = false;
    bool            crashed = false;
    int             exit_code = 0;
    cont::String    stdout_text;
};

#if defined(_WIN32)

cont::String quote_arg(const cont::String& a)
{
    const std::string_view sv = a;
    if (!sv.empty() && sv.find_first_of(" \t\"") == std::string_view::npos)
    {
        return a;
    }
    cont::String q;
    q.append("\"");
    for (char c : sv)
    {
        if (c == '"')
        {
            q.append("\\\"");
        }
        else
        {
            q.push_back(c);
        }
    }
    q.append("\"");
    return q;
}

ChildResult spawn_bounded(const cont::String& exe, const cont::Array<cont::String>& args, crd::u32 timeout_ms)
{
    ChildResult r;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (CreatePipe(&rd, &wr, &sa, 0) == 0)
    {
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    cont::String cmd = quote_arg(exe);
    for (const cont::String& a : args)
    {
        cmd.append(" ");
        cmd.append(quote_arg(a));
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    cont::Array<char> mutable_cmd;
    for (char c : std::string_view{cmd})
    {
        mutable_cmd.push_back(c);
    }
    mutable_cmd.push_back('\0');

    const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (ok == 0)
    {
        CloseHandle(rd);
        return r;
    }
    r.spawned = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) != 0 && avail > 0)
        {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) != 0 && got > 0)
            {
                r.stdout_text.append(buf, static_cast<crd::usize>(got));
                continue;
            }
        }
        const DWORD w = WaitForSingleObject(pi.hProcess, 20);
        if (w == WAIT_OBJECT_0)
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(pi.hProcess, 1U);
            WaitForSingleObject(pi.hProcess, 2000);
            r.timed_out = true;
            break;
        }
    }

    for (;;)
    {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) == 0 || avail == 0)
        {
            break;
        }
        char buf[4096];
        DWORD got = 0;
        if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) == 0 || got == 0)
        {
            break;
        }
        r.stdout_text.append(buf, static_cast<crd::usize>(got));
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    r.crashed = !r.timed_out && (code >= 0xC0000000U || code == 0xC0000409U);

    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

#else

ChildResult spawn_bounded(const cont::String& exe, const cont::Array<cont::String>& args, crd::u32 timeout_ms)
{
    ChildResult r;

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
    {
        return r;
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }

    if (pid == 0)
    {
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        cont::Array<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const cont::String& a : args)
        {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(fds[1]);
    const int rd = fds[0];
    ::fcntl(rd, F_SETFL, O_NONBLOCK);
    r.spawned = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    bool reaped = false;
    for (;;)
    {
        char buf[4096];
        const ssize_t got = ::read(rd, buf, sizeof(buf));
        if (got > 0)
        {
            r.stdout_text.append(buf, static_cast<crd::usize>(got));
            continue;
        }
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid)
        {
            reaped = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            r.timed_out = true;
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (;;)
    {
        char buf[4096];
        const ssize_t got = ::read(rd, buf, sizeof(buf));
        if (got <= 0)
        {
            break;
        }
        r.stdout_text.append(buf, static_cast<crd::usize>(got));
    }
    ::close(rd);

    if (reaped && !r.timed_out)
    {
        if (WIFSIGNALED(status))
        {
            r.crashed = true;
            r.exit_code = 128 + WTERMSIG(status);
        }
        else if (WIFEXITED(status))
        {
            r.exit_code = WEXITSTATUS(status);
        }
    }
    return r;
}

#endif

bool sv_is_none(std::string_view s)
{
    return s.empty() || s == "none";
}

} // namespace

crd::containers::String host_tuple()
{
    cont::String t;
#if defined(_WIN32)
    t.append("windows");
#elif defined(__linux__)
    t.append("linux");
#elif defined(__APPLE__)
    t.append("macos");
#else
    t.append("unknown-os");
#endif
    t.append("/");
#if defined(__x86_64__) || defined(_M_X64)
    t.append("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    t.append("arm64");
#elif defined(__i386__) || defined(_M_IX86)
    t.append("x86");
#else
    t.append("unknown-isa");
#endif
    t.append("/");
#if defined(__clang__)
    t.append("clang-");
    append_int(t, __clang_major__);
    t.append(".");
    append_int(t, __clang_minor__);
#elif defined(_MSC_VER)
    t.append("msvc-");
    append_int(t, _MSC_VER);
#elif defined(__GNUC__)
    t.append("gcc-");
    append_int(t, __GNUC__);
    t.append(".");
    append_int(t, __GNUC_MINOR__);
#else
    t.append("unknown-cc");
#endif
    return t;
}

Outcome run_specimen(const crd::containers::String& exe_path,
                     const crd::containers::Array<crd::containers::String>& args,
                     const Expectation& expect)
{
    Outcome o;
    o.host_tuple = host_tuple();
    o.memory_bound_enforced = false; // timeout-only; RLIMIT_AS would break ASan children

    if (!expect.output_dir.empty() && !output_writable(expect.output_dir))
    {
        o.verdict = Verdict::DeniedOutput;
        o.reason.append("output path not writable: ");
        o.reason.append(expect.output_dir);
        return o;
    }
    if (exe_path.empty() || !path_exists(exe_path))
    {
        o.verdict = Verdict::MissingExecutable;
        o.reason.append("specimen not found: ");
        o.reason.append(exe_path);
        return o;
    }

    const ChildResult c = spawn_bounded(exe_path, args, expect.timeout_ms);
    if (!c.spawned)
    {
        o.verdict = Verdict::MissingExecutable;
        o.reason.append("spawn failed: ");
        o.reason.append(exe_path);
        return o;
    }

    o.exit_code = c.exit_code;
    o.crashed = c.crashed;
    o.timed_out = c.timed_out;
    const std::string_view out_view = c.stdout_text;
    o.identity = grab_tag(out_view, "CRD_DIAG_IDENTITY=");
    o.sanitizer = grab_tag(out_view, "CRD_DIAG_SANITIZER=");

    if (c.timed_out)
    {
        o.verdict = Verdict::Timeout;
        o.reason.append("exceeded ");
        append_int(o.reason, static_cast<long long>(expect.timeout_ms));
        o.reason.append(" ms");
        return o;
    }

    if (!expect.expected_identity.empty() && !(o.identity == expect.expected_identity))
    {
        o.verdict = Verdict::MismatchedBinary;
        o.reason.append("identity '");
        o.reason.append(o.identity);
        o.reason.append("' != expected '");
        o.reason.append(expect.expected_identity);
        o.reason.append("'");
        return o;
    }

    switch (expect.want)
    {
    case Expectation::Want::CleanExit:
        o.verdict = (!c.crashed && c.exit_code == 0) ? Verdict::Clean : Verdict::Unexpected;
        break;
    case Expectation::Want::Crash:
        o.verdict = c.crashed ? Verdict::Crashed : Verdict::Unexpected;
        break;
    case Expectation::Want::SanitizerCatch:
        if (sv_is_none(std::string_view{o.sanitizer}))
        {
            o.verdict = Verdict::InstrumentAbsent;
        }
        else if (c.crashed || c.exit_code != 0)
        {
            o.verdict = Verdict::SanitizerCaught;
        }
        else
        {
            o.verdict = Verdict::Unexpected;
        }
        break;
    }
    o.reason.append(verdict_name(o.verdict));
    return o;
}

Outcome select_and_run(const crd::containers::Array<crd::containers::String>& candidates,
                       const crd::containers::String& filter,
                       const Expectation& expect)
{
    const std::string_view filt = filter;
    for (const cont::String& cand : candidates)
    {
        const std::string_view cv = cand;
        if (filt.empty() || cv.find(filt) != std::string_view::npos)
        {
            return run_specimen(cand, cont::Array<cont::String>{}, expect);
        }
    }
    Outcome o;
    o.host_tuple = host_tuple();
    o.verdict = Verdict::ZeroSelection;
    o.reason.append("no specimen matched filter '");
    o.reason.append(filter);
    o.reason.append("'");
    return o;
}

} // namespace crd::diag
