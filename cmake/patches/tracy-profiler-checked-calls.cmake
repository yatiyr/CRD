# tracy-profiler-checked-calls — applied by crd_patched_copy() to tracy/public/client/TracyProfiler.cpp (0.14.1).
#
# The client's unity unit drew three GCC 13 warnings on linux-gcc-shipping (glibc's warn_unused_result and
# -Wformat-overflow), each a real defect upstream leaves in 0.14.1:
#   - the safe-copy pipe's creation ignored its result: a failed pipe() left m_pipe uninitialised, and every later
#     fcntl, write and read used whatever the two ints held. On failure both ends are now -1, so the size probe stops
#     at EBADF and every safe copy reports failure, which the caller already handles;
#   - the host description wrote the user and host names with sprintf into what was left of a 1,024-byte static
#     buffer (the login and the host name each up to their own buffer's size): now bounded by snprintf and clamped to
#     the space left, as is the user's full name after it.
# Windows compiles the pipe out; the bound on the host description applies on every platform. An anchor that stops
# matching fails the configure (see docs/design/pinned-inputs.md).
crd_patch_replace(
    [[    pipe(m_pipe);
]]
    [[    if( pipe( m_pipe ) != 0 ) // crd-patch: a failed pipe leaves no descriptor to probe, write or read
    {
        m_pipe[0] = -1;
        m_pipe[1] = -1;
    }
]])
crd_patch_replace(
    [[    ptr += sprintf( ptr, "User: %s@%s", user, hostname );

    const char* fullName = GetUserFullName();
    if( fullName ) ptr += sprintf( ptr, " (%s)", fullName );
]]
    [[    // crd-patch: the names are bounded by what is left of buf, and the pointer by its end
    {
        const size_t left = sizeof( buf ) - size_t( ptr - buf );
        const int written = snprintf( ptr, left, "User: %s@%s", user, hostname );
        ptr += written < 0 ? 0 : ( size_t( written ) < left ? size_t( written ) : left - 1 );
    }

    const char* fullName = GetUserFullName();
    if( fullName )
    {
        const size_t left = sizeof( buf ) - size_t( ptr - buf );
        const int written = snprintf( ptr, left, " (%s)", fullName );
        ptr += written < 0 ? 0 : ( size_t( written ) < left ? size_t( written ) : left - 1 );
    }
]])
