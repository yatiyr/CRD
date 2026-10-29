#pragma once

// The export spelling and the object handle of the board peers (crd_add_bench_peer, runtime/CMakeLists.txt): each
// peer is a shared library of its own, built with CRD_BENCH_PEER_BUILD, whose symbols are hidden but for the operations
// its header declares.

#if defined(_WIN32)
#if defined(CRD_BENCH_PEER_BUILD)
#define CRD_BENCH_PEER_API __declspec(dllexport)
#else
#define CRD_BENCH_PEER_API __declspec(dllimport)
#endif
#else
#define CRD_BENCH_PEER_API __attribute__((visibility("default")))
#endif

namespace crd_peer
{
// A peer's handle owned by the caller's scope; destroy() is the peer's own, found by argument-dependent lookup.
template <typename T> class Owned
{
public:
    explicit Owned(T* object) noexcept : m_object(object) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { destroy(m_object); }
    [[nodiscard]] T* get() const noexcept { return m_object; }

private:
    T* m_object;
};
} // namespace crd_peer
