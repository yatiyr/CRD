#pragma once

// Umbrella header for the crd-containers module.
//
// Typical usage:
//
//     #include <crd/containers/containers.hpp>
//
//     using namespace crd::containers;
//
//     Array<crd::u32> ids(default_allocator());
//     ids.push_back(42);
//
//     FixedArray<const char*, 8> tags;
//     tags.push_back("renderer");
//
//     Span<const u32> view = as_const_span(ids);
//
// v1a ships:
//   - Array<T>
//   - FixedArray<T, N>
//   - Span / ConstSpan (aliases to std::span)
//   - hash.hpp (splitmix64, FNV-1a, DefaultHash<T>)
//   - g_log_containers channel
//
// v1b adds:
//   - String (SSO 23-byte)
//   - StringView alias
//   - RingBuffer<T>
//
// v1c adds:
//   - HashMap<K, V>  (open addressing + Robin Hood + backshift, no tombstones)
//   - HashSet<K>     (HashMap<K, EmptySetValue> wrapper)
//
// v1d adds:
//   - SpscQueue<T>   (lock-free single-producer / single-consumer queue)

#include <crd/containers/array.hpp>
#include <crd/containers/atomic_array.hpp>
#include <crd/containers/concurrent_queue.hpp>
#include <crd/containers/fixed_array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/hash_set.hpp>
#include <crd/containers/log_channel.hpp>
#include <crd/containers/ring_buffer.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/spsc_queue.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::containers
{
// Forward decls of the force-link helpers defined in the corresponding
// .cpp files. The anchor variables below reference them so the linker
// is forced to keep the TUs alive even when callers only use header-only
// templates.
int force_link_log_channel() noexcept;
int force_link_string() noexcept;
} // namespace crd::containers

namespace crd::containers::detail
{
// Per-TU anchors that force the linker to keep our .cpp files in the
// archive. Without this, MSVC strips them because the test executable
// references nothing from them directly (every container header is
// template-only).
//
// Note on g_log_channel_anchor: since v1d, the actual
// CRD_DEFINE_LOG_CHANNEL(g_log_containers, ...) lives in crd-log
// (see log_channels_first_party.cpp) to break the dependency cycle.
// We anchor BOTH the (now-empty) containers-side .cpp AND the crd-log
// first-party-channels .cpp, so the channel registrar is guaranteed
// to run regardless of which library the consumer pulls in first.
//
// We use `volatile int` (not `const`) and READ it once via a return
// value, so the optimizer can't fold the call away. Each TU that
// includes this header pays for one indirect call at static-init time
// — negligible.
namespace
{
[[maybe_unused]] inline volatile int g_log_channel_anchor = ::crd::containers::force_link_log_channel();
[[maybe_unused]] inline volatile int g_first_party_channels_anchor = ::crd::log::force_link_first_party_channels();
[[maybe_unused]] inline volatile int g_string_anchor = ::crd::containers::force_link_string();
} // anonymous namespace
} // namespace crd::containers::detail
