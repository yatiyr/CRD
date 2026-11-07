#pragma once

// crd-net -- coded bodies (NET.3; threat T09; contract: docs/design/network-foundation.md#body-coding). A channel may
// declare models, and its bodies then travel coded against a model both ends hold: a zstd dictionary for what a
// reliable channel carries, an entropy model for the small bit-packed bodies of a snapshot. The codecs are
// crd-compression's; this is what binds them to channels. Every message is coded alone. Nothing here keeps anything
// between messages, so a refused or lost message never costs the next one, and nothing of one message can show in the
// size of another.
//
// A BodyModel is compiled once and shared read-only by every session and thread. A BodyCoder is one I/O loop's. Both
// take their memory at construction and allocate nothing after it.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/wire.hpp>

namespace crd::compression
{
class EntropyModel;
class ZstdDictionary;
class ZstdMessageCoder;
} // namespace crd::compression

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

// A model's source compiled for both directions. `source` outlives it. `max_message_bytes` bounds what it codes, and is
// the same for every model and coder of a loop.
class BodyModel
{
public:
    BodyModel(memory::IAllocator& allocator, wire::CodecKind kind, cont::ConstSpan<u8> source,
              u32 max_message_bytes) noexcept;
    ~BodyModel();
    BodyModel(const BodyModel&) = delete;
    BodyModel& operator=(const BodyModel&) = delete;

    // False when the allocator refused or `source` is no model of its kind.
    [[nodiscard]] bool ready() const noexcept { return m_entropy != nullptr || m_dictionary != nullptr; }
    [[nodiscard]] wire::CodecKind kind() const noexcept { return m_kind; }
    // XXH3-64 of the source: what a schema's CodecDesc::model pins.
    [[nodiscard]] u64 hash() const noexcept { return m_hash; }

private:
    friend class BodyCoder;

    memory::IAllocator& m_allocator;
    wire::CodecKind m_kind;
    u64 m_hash = 0U;
    compression::EntropyModel* m_entropy = nullptr;
    compression::ZstdDictionary* m_dictionary = nullptr;
};

// One I/O loop's coding state: zstd's contexts, and a message's room while a frame is written.
class BodyCoder
{
public:
    BodyCoder(memory::IAllocator& allocator, u32 max_message_bytes) noexcept;
    ~BodyCoder();
    BodyCoder(const BodyCoder&) = delete;
    BodyCoder& operator=(const BodyCoder&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_coder != nullptr && m_scratch != nullptr; }

    // Codes `plain` at the start of `out` and returns the size: zero when the body should travel as it is, because
    // coding did not make it smaller or would pass `max_ratio`, what a reader refuses.
    [[nodiscard]] usize encode(const BodyModel& model, u16 max_ratio, cont::ConstSpan<u8> plain,
                               cont::Span<u8> out) noexcept;

    // Whether `coded` decodes to exactly `out`. Hostile input.
    [[nodiscard]] bool decode(const BodyModel& model, cont::ConstSpan<u8> coded, cont::Span<u8> out) noexcept;

    // Room for one message, the coder's to lend while a frame is written.
    [[nodiscard]] cont::Span<u8> scratch() noexcept { return cont::Span<u8>(m_scratch, m_scratch_bytes); }

private:
    memory::IAllocator& m_allocator;
    compression::ZstdMessageCoder* m_coder = nullptr;
    u8* m_scratch = nullptr;
    u32 m_scratch_bytes = 0U;
};

// What a channel's writer or reader codes with at the session's revisions: empty codes nothing.
struct BodyCoding
{
    const BodyModel* model = nullptr;
    BodyCoder* coder = nullptr;
    u16 max_ratio = 0U;
    u32 max_bytes = 0U; // the channel's most: what a body may decode to, whatever revision wrote it
};

// Whether a coded body of `coded` bytes may claim to expand to `expanded`: it is smaller than what it expands to, as a
// writer ensures, and within the channel's ratio. Checked before anything is reserved for it.
[[nodiscard]] constexpr bool expansion_holds(u64 coded, u64 expanded, u16 max_ratio) noexcept
{
    return coded < expanded && expanded <= coded * max_ratio;
}

} // namespace crd::net
