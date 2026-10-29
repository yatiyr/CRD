#pragma once

// The export spelling of the board peers (crd_add_bench_peer, runtime/CMakeLists.txt): each peer is a shared library
// of its own, built with CRD_BENCH_PEER_BUILD, whose symbols are hidden but for the operations its header declares.

#if defined(_WIN32)
#if defined(CRD_BENCH_PEER_BUILD)
#define CRD_BENCH_PEER_API __declspec(dllexport)
#else
#define CRD_BENCH_PEER_API __declspec(dllimport)
#endif
#else
#define CRD_BENCH_PEER_API __attribute__((visibility("default")))
#endif
