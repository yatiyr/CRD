// crd-crypto tests -- the native side of the instrumentation APK (NET.2, decision K15): KeyStoreRunner.run hands in
// Catch2's arguments and the file its output goes to, and gets Catch2's exit code back. The case runs in the app's
// Java VM, so crd-crypto's Keystore protector and key device find it through JNI_GetCreatedJavaVMs.
#include <catch2/catch_session.hpp>

#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>

namespace
{
// More arguments than a test spec, a reporter and its options need.
constexpr jsize kMaxArguments = 32;

// Standard output and error, sent to `path` for the length of one run and restored after.
class Redirect
{
public:
    explicit Redirect(const char* path) noexcept
    {
        const int file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (file < 0)
        {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        m_out = ::dup(STDOUT_FILENO);
        m_err = ::dup(STDERR_FILENO);
        m_ok = m_out >= 0 && m_err >= 0 && ::dup2(file, STDOUT_FILENO) >= 0 && ::dup2(file, STDERR_FILENO) >= 0;
        ::close(file);
    }
    Redirect(const Redirect&)            = delete;
    Redirect& operator=(const Redirect&) = delete;
    ~Redirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        if (m_out >= 0)
        {
            ::dup2(m_out, STDOUT_FILENO);
            ::close(m_out);
        }
        if (m_err >= 0)
        {
            ::dup2(m_err, STDERR_FILENO);
            ::close(m_err);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
    int  m_out = -1;
    int  m_err = -1;
    bool m_ok  = false;
};

// One Java string's modified UTF-8 for the length of the call.
class Utf
{
public:
    Utf() noexcept = default;
    Utf(const Utf&)            = delete;
    Utf& operator=(const Utf&) = delete;
    ~Utf()
    {
        if (m_chars != nullptr)
        {
            m_env->ReleaseStringUTFChars(m_string, m_chars);
            m_env->DeleteLocalRef(m_string);
        }
    }

    bool take(JNIEnv* env, jstring string) noexcept
    {
        m_env    = env;
        m_string = string;
        m_chars  = string != nullptr ? env->GetStringUTFChars(string, nullptr) : nullptr;
        return m_chars != nullptr;
    }

    [[nodiscard]] const char* chars() const noexcept { return m_chars; }

private:
    JNIEnv*     m_env    = nullptr;
    jstring     m_string = nullptr;
    const char* m_chars  = nullptr;
};
} // namespace

// The exit code of one Catch2 session over `arguments` (argv[0] first) with its output in `output`; 64 when the
// arguments or the output cannot be taken.
// The name is JNI's: Java_, the class's binary name and the method.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" JNIEXPORT jint JNICALL Java_cerid_test_keystore_KeyStoreRunner_run(JNIEnv* env, jclass,
                                                                               jobjectArray arguments, jstring output)
{
    constexpr jint usage = 64;
    const jsize    count  = arguments != nullptr ? env->GetArrayLength(arguments) : 0;
    // Each argument and the output path stay referenced until the run ends.
    if (count < 1 || count > kMaxArguments || env->EnsureLocalCapacity(count + 1) != JNI_OK)
    {
        return usage;
    }
    Utf         texts[kMaxArguments];
    const char* argv[kMaxArguments + 1] = {};
    for (jsize i = 0; i < count; ++i)
    {
        if (!texts[i].take(env, static_cast<jstring>(env->GetObjectArrayElement(arguments, i))))
        {
            return usage;
        }
        argv[i] = texts[i].chars();
    }
    Utf path;
    if (!path.take(env, static_cast<jstring>(env->NewLocalRef(output))))
    {
        return usage;
    }
    const Redirect redirect(path.chars());
    if (!redirect.ok())
    {
        return usage;
    }
    return Catch::Session().run(static_cast<int>(count), argv);
}
