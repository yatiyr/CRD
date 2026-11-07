// crd-net -- stages S6 and S7 of bounded admission (NET.3; docs/design/network-foundation.md#frames).

#include <crd/memory/allocator.hpp>
#include <crd/net/admission.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
constexpr u8 kTransportKind = 0U;

[[nodiscard]] AdmitVerdict read_refusal(FrameRead read, HeaderForm form) noexcept
{
    // Only a stream has more bytes coming.
    return (read == FrameRead::Short && form == HeaderForm::Sized) ? AdmitVerdict::NeedMore : AdmitVerdict::Malformed;
}

[[nodiscard]] bool same_identifier(const u8 (&wire_id)[16], const crypto::CapabilityId& authorized) noexcept
{
    return std::memcmp(wire_id, authorized.bytes, sizeof(wire_id)) == 0;
}
} // namespace

cont::StringView admit_verdict_name(AdmitVerdict verdict) noexcept
{
    switch (verdict)
    {
        case AdmitVerdict::Admitted:
            return "admitted";
        case AdmitVerdict::NeedMore:
            return "need-more";
        case AdmitVerdict::Malformed:
            return "malformed";
        case AdmitVerdict::BadChannel:
            return "bad-channel";
        case AdmitVerdict::WrongCarrier:
            return "wrong-carrier";
        case AdmitVerdict::Unannounced:
            return "unannounced";
        case AdmitVerdict::UnknownMessage:
            return "unknown-message";
        case AdmitVerdict::NewerMessage:
            return "newer-message";
        case AdmitVerdict::NotInRevision:
            return "not-in-revision";
        case AdmitVerdict::OtherChannel:
            return "other-channel";
        case AdmitVerdict::Undersized:
            return "undersized";
        case AdmitVerdict::Oversized:
            return "oversized";
        case AdmitVerdict::BadFragment:
            return "bad-fragment";
        case AdmitVerdict::ChannelMessages:
            return "channel-messages";
        case AdmitVerdict::ChannelBytes:
            return "channel-bytes";
        case AdmitVerdict::PrincipalDatagrams:
            return "principal-datagrams";
        case AdmitVerdict::PrincipalMessages:
            return "principal-messages";
        case AdmitVerdict::PrincipalBytes:
            return "principal-bytes";
        case AdmitVerdict::BudgetOperations:
            return "budget-operations";
        case AdmitVerdict::BudgetBytes:
            return "budget-bytes";
        case AdmitVerdict::PendingFull:
            return "pending-full";
        case AdmitVerdict::ReassemblyFull:
            return "reassembly-full";
        case AdmitVerdict::NoMemory:
            return "no-memory";
        case AdmitVerdict::StreamLimit:
            return "stream-limit";
        case AdmitVerdict::NoCapability:
            return "no-capability";
        case AdmitVerdict::OtherSchema:
            return "other-schema";
        case AdmitVerdict::Undeclared:
            return "undeclared";
        case AdmitVerdict::Replayable:
            return "replayable";
        case AdmitVerdict::NotGranted:
            return "not-granted";
        case AdmitVerdict::Buffered:
            return "buffered";
        case AdmitVerdict::StaleFragment:
            return "stale-fragment";
        case AdmitVerdict::DuplicateFragment:
            return "duplicate-fragment";
        case AdmitVerdict::FragmentConflict:
            return "fragment-conflict";
        case AdmitVerdict::IntegrityFailed:
            return "integrity-failed";
        case AdmitVerdict::PaddingFrame:
            return "padding-frame";
        case AdmitVerdict::BadPadding:
            return "bad-padding";
    }
    return "unknown";
}

AdmitAction admit_action(AdmitVerdict verdict, bool reliable) noexcept
{
    switch (verdict)
    {
        case AdmitVerdict::Admitted:
            return AdmitAction::Deliver;
        case AdmitVerdict::NeedMore:
        case AdmitVerdict::Buffered:
        case AdmitVerdict::PaddingFrame:
            return AdmitAction::Continue;
        case AdmitVerdict::ChannelMessages:
        case AdmitVerdict::ChannelBytes:
        case AdmitVerdict::PrincipalDatagrams:
        case AdmitVerdict::PrincipalMessages:
        case AdmitVerdict::PrincipalBytes:
        case AdmitVerdict::BudgetOperations:
        case AdmitVerdict::BudgetBytes:
        case AdmitVerdict::PendingFull:
        case AdmitVerdict::ReassemblyFull:
        case AdmitVerdict::NoMemory:
            return reliable ? AdmitAction::Wait : AdmitAction::Skip;
        case AdmitVerdict::Malformed:
        case AdmitVerdict::BadChannel:
        case AdmitVerdict::WrongCarrier:
        case AdmitVerdict::Unannounced:
        case AdmitVerdict::UnknownMessage:
        case AdmitVerdict::NotInRevision:
        case AdmitVerdict::OtherChannel:
        case AdmitVerdict::Undersized:
        case AdmitVerdict::Oversized:
        case AdmitVerdict::BadFragment:
        case AdmitVerdict::IntegrityFailed: // a reliable channel cannot lose what its sender believes delivered
        case AdmitVerdict::BadPadding:
            return reliable ? AdmitAction::Close : AdmitAction::Skip;
        case AdmitVerdict::NewerMessage:
        case AdmitVerdict::StreamLimit:
        case AdmitVerdict::NoCapability:
        case AdmitVerdict::OtherSchema:
        case AdmitVerdict::Undeclared:
        case AdmitVerdict::Replayable:
        case AdmitVerdict::NotGranted:
        case AdmitVerdict::StaleFragment:
        case AdmitVerdict::DuplicateFragment:
        case AdmitVerdict::FragmentConflict:
            return AdmitAction::Skip;
    }
    return AdmitAction::Close;
}

Admission::Admission(PrincipalQuota& quota, memory::IAllocator& allocator, cont::Span<ChannelState> channels) noexcept
    : m_quota(quota), m_allocator(allocator), m_channels(channels)
{
}

bool Admission::bind(cont::ConstSpan<SessionSchema> schemas) noexcept
{
    if (schemas.size() > kMaxSessionSchemas)
    {
        return false;
    }
    usize channels = 0U;
    for (const SessionSchema& schema : schemas)
    {
        if (schema.wire == nullptr)
        {
            return false;
        }
        const bool transport = schema.wire->kind == kTransportKind;
        if (transport != (schema.authorized == nullptr))
        {
            return false;
        }
        if (!transport)
        {
            const crypto::MessageSchema& signed_schema = schema.authorized->schema;
            if (!same_identifier(schema.wire->id, signed_schema.schema) ||
                static_cast<u8>(signed_schema.kind) != schema.wire->kind || signed_schema.major != schema.wire->major)
            {
                return false;
            }
        }
        channels += schema.wire->channels.size();
    }
    if (channels > m_channels.size())
    {
        return false;
    }

    m_schema_count = 0U;
    m_channel_count = 0U;
    for (const SessionSchema& schema : schemas)
    {
        SchemaState& state = m_schemas[m_schema_count];
        state = SchemaState{};
        state.wire = schema.wire;
        state.authorized = schema.authorized;
        state.first_channel = m_channel_count;
        for (usize index = 0U; index < schema.wire->channels.size(); ++index)
        {
            ChannelState& channel = m_channels[m_channel_count];
            channel = ChannelState{};
            channel.desc = &schema.wire->channels[index];
            channel.slot = m_schema_count;
            channel.index = static_cast<u32>(index);
            ++m_channel_count;
        }
        ++m_schema_count;
    }
    return true;
}

bool Admission::announce(cont::ConstSpan<u8> schema_id, u16 major, u16 writer_minor) noexcept
{
    if (schema_id.size() != sizeof(wire::WireSchema::id))
    {
        return false;
    }
    for (u32 slot = 0U; slot < m_schema_count; ++slot)
    {
        SchemaState& schema = m_schemas[slot];
        if (std::memcmp(schema.wire->id, schema_id.data(), schema_id.size()) != 0)
        {
            continue;
        }
        if (schema.wire->major != major || (schema.announced && schema.writer_minor != writer_minor))
        {
            return false;
        }
        schema.writer_minor = writer_minor;
        schema.announced = true;
        return true;
    }
    return false;
}

void Admission::set_capability(const crypto::Capability* capability) noexcept
{
    m_capability = capability;
}

u32 Admission::channel_number(u32 schema_slot, u32 channel_index) const noexcept
{
    return m_schemas[schema_slot].first_channel + channel_index;
}

AdmitVerdict Admission::open_stream(const Route& route) noexcept
{
    if (route.form != RouteForm::Whole)
    {
        return AdmitVerdict::Malformed;
    }
    if (route.channel >= m_channel_count)
    {
        return AdmitVerdict::BadChannel;
    }
    if (!m_channels[route.channel].desc->reliable)
    {
        return AdmitVerdict::WrongCarrier;
    }
    return m_quota.streams.try_add(1U, m_quota.limits.streams) ? AdmitVerdict::Admitted : AdmitVerdict::StreamLimit;
}

void Admission::close_stream() noexcept
{
    m_quota.streams.release(1U);
}

AdmitVerdict Admission::open_datagram(cont::ConstSpan<u8> bytes, u64 now_ns, DatagramCursor& cursor) noexcept
{
    const auto refuse = [this](AdmitVerdict verdict) noexcept
    {
        count_verdict(verdict);
        return verdict;
    };
    usize consumed = 0U;
    if (read_route(bytes, cursor.route, consumed) != FrameRead::Ok)
    {
        return refuse(AdmitVerdict::Malformed);
    }
    if (cursor.route.channel >= m_channel_count)
    {
        return refuse(AdmitVerdict::BadChannel);
    }
    const wire::ChannelDesc& channel = *m_channels[cursor.route.channel].desc;
    if (channel.reliable)
    {
        return refuse(AdmitVerdict::WrongCarrier);
    }
    const GateRule rule = gate_rule(m_quota.limits.datagrams_per_s);
    if (channel.priority != 0U && m_quota.datagrams.open_at(rule) > now_ns)
    {
        return refuse(AdmitVerdict::PrincipalDatagrams);
    }
    m_quota.datagrams.charge(1U, rule, now_ns);
    cursor.at = consumed;
    cursor.more = true;
    cursor.first = true;
    if (cursor.route.form == RouteForm::Padded)
    {
        // One read of at most two blocks, charged. The gates close the next frame, not this one.
        const AdmitVerdict padding = take_padding(cursor.route.channel, bytes, now_ns, cursor);
        if (padding != AdmitVerdict::Admitted)
        {
            return refuse(padding);
        }
    }
    cursor.bytes = bytes;
    return AdmitVerdict::Admitted;
}

AdmitVerdict Admission::take_padding(u32 channel, cont::ConstSpan<u8> bytes, u64 now_ns,
                                     DatagramCursor& cursor) noexcept
{
    ChannelState& state = m_channels[channel];
    u32 count = 0U;
    usize used = 0U;
    const cont::ConstSpan<u8> rest = bytes.subspan(cursor.at);
    if (read_frame_number(rest, count, used) != FrameRead::Ok ||
        !padding_count_holds(count, state.desc->padding_block) || count > rest.size() - used)
    {
        return AdmitVerdict::BadPadding;
    }
    for (const u8 byte : rest.subspan(used, count))
    {
        if (byte != 0U)
        {
            return AdmitVerdict::BadPadding;
        }
    }
    const usize at = cursor.at + used + count;
    Route inner;
    usize inner_bytes = 0U;
    if (read_route(bytes.subspan(at), inner, inner_bytes) != FrameRead::Ok || inner.channel != channel ||
        inner.form == RouteForm::Padded)
    {
        return AdmitVerdict::BadPadding;
    }
    state.bytes.charge(static_cast<u32>(at), gate_rule(state.desc->bytes_per_s), now_ns);
    m_quota.bytes.charge(static_cast<u32>(at), gate_rule(m_quota.limits.bytes_per_s), now_ns);
    cursor.route = inner;
    cursor.at = at + inner_bytes;
    return AdmitVerdict::Admitted;
}

AdmitVerdict Admission::read_frame(u32 channel, HeaderForm form, cont::ConstSpan<u8> bytes,
                                   ParsedFrame& parsed) const noexcept
{
    const ChannelState& state = m_channels[channel];
    const SchemaState& schema = m_schemas[state.slot];
    const wire::ChannelDesc& desc = *state.desc;

    parsed = ParsedFrame{};
    usize at = 0U;
    usize used = 0U;
    FrameRead read = read_frame_number(bytes, parsed.header.message, used);
    if (read != FrameRead::Ok)
    {
        return read_refusal(read, form);
    }
    at = used;

    if (form != HeaderForm::ToEnd)
    {
        read = read_frame_number(bytes.subspan(at), parsed.header.length, used);
        if (read != FrameRead::Ok)
        {
            return read_refusal(read, form);
        }
        at += used;
    }
    if (schema.wire->kind != kTransportKind && !schema.announced)
    {
        return AdmitVerdict::Unannounced;
    }
    if (parsed.header.message == kPaddingMessage && form == HeaderForm::Sized && desc.reliable &&
        desc.padding_block != 0U)
    {
        parsed.header_bytes = at;
        return padding_count_holds(parsed.header.length, desc.padding_block) ? AdmitVerdict::PaddingFrame
                                                                             : AdmitVerdict::BadPadding;
    }

    const wire::MessageDesc* message = wire::find_message(*schema.wire, parsed.header.message);
    const bool newer = schema.writer_minor > schema.wire->minor;
    if (message == nullptr)
    {
        if (!newer)
        {
            return AdmitVerdict::UnknownMessage;
        }
        parsed.header_bytes = at;
        return (parsed.header.length > u64{desc.max_message_bytes} + kMaxFrameTailBytes) ? AdmitVerdict::Oversized
                                                                                         : AdmitVerdict::NewerMessage;
    }
    if (message->since > schema.writer_minor)
    {
        return AdmitVerdict::NotInRevision;
    }
    if (message->channel != state.index)
    {
        return AdmitVerdict::OtherChannel;
    }

    const FrameShape shape = frame_shape(desc, *message);
    read = read_frame_tail(bytes.subspan(at), form, shape, parsed.header, used);
    if (read != FrameRead::Ok)
    {
        return read_refusal(read, form);
    }
    parsed.header_bytes = at + used;
    parsed.message = message;
    parsed.hashed = shape.hash;

    if (form == HeaderForm::ToEnd)
    {
        parsed.header.length = static_cast<u32>(bytes.size() - parsed.header_bytes); // a datagram is far below 4 GiB
    }
    else if (form == HeaderForm::Sized)
    {
        if (used > parsed.header.length)
        {
            return AdmitVerdict::Malformed; // the length counts what it was just read past
        }
        parsed.header.length -= static_cast<u32>(used);
    }
    if (parsed.header.length < message->min_bytes)
    {
        return AdmitVerdict::Undersized;
    }
    // A newer writer may have appended fields past this reader's most; the channel's most still holds for every
    // revision.
    const bool over_message = !newer && parsed.header.length > message->max_bytes;
    return (over_message || parsed.header.length > desc.max_message_bytes) ? AdmitVerdict::Oversized
                                                                           : AdmitVerdict::Admitted;
}

GateRule Admission::gate_rule(u64 per_second) const noexcept
{
    return GateRule{per_second, m_quota.limits.tolerance_ns};
}

AdmitVerdict Admission::authorize(const SchemaState& schema, const wire::ChannelDesc& channel,
                                  const wire::MessageDesc& message, u64 object,
                                  crypto::Delivery delivery) const noexcept
{
    if (schema.wire->kind == kTransportKind)
    {
        // A transport schema runs below S18, and only on control channels: nothing it carries can execute an operation.
        const bool below = message.operation == 0U && channel.channel_class == wire::ChannelClass::Control;
        return below ? AdmitVerdict::Admitted : AdmitVerdict::Undeclared;
    }
    if (schema.authorized == nullptr || m_capability == nullptr)
    {
        return AdmitVerdict::NoCapability;
    }
    switch (crypto::authorize_operation(*schema.authorized, *m_capability, message.operation, object, delivery))
    {
        case crypto::OperationVerdict::Admitted:
            return AdmitVerdict::Admitted;
        case crypto::OperationVerdict::OtherSchema:
            return AdmitVerdict::OtherSchema;
        case crypto::OperationVerdict::Undeclared:
            return AdmitVerdict::Undeclared;
        case crypto::OperationVerdict::Replayable:
            return AdmitVerdict::Replayable;
        case crypto::OperationVerdict::NotGranted:
            return AdmitVerdict::NotGranted;
    }
    return AdmitVerdict::Undeclared;
}

AdmitVerdict Admission::admit(u32 channel, const ParsedFrame& parsed, crypto::Delivery delivery, u64 now_ns,
                              bool reassembled, FrameStep& step) noexcept
{
    ChannelState& state = m_channels[channel];
    const SchemaState& schema = m_schemas[state.slot];
    const wire::ChannelDesc& desc = *state.desc;
    const wire::MessageDesc& message = *parsed.message;
    const u32 bytes = parsed.header.length;
    const QuotaLimits& limits = m_quota.limits;

    // S6, in order, spending nothing until all of it holds. A priority 0 channel keeps its own gates and is charged to
    // the principal, but the principal's other traffic cannot close it: a session at its budget can still renew.
    const GateRule channel_messages = gate_rule(desc.messages_per_s);
    const GateRule channel_bytes = gate_rule(desc.bytes_per_s);
    const GateRule principal_messages = gate_rule(limits.messages_per_s);
    const GateRule principal_bytes = gate_rule(limits.bytes_per_s);
    const bool budgeted = message.operation != 0U && m_capability != nullptr;
    const GateRule budget_operations =
        budgeted ? gate_rule(m_capability->grant.budget.operations_per_second) : GateRule{};
    const GateRule budget_bytes = budgeted ? gate_rule(m_capability->grant.budget.bytes_per_second) : GateRule{};

    struct Check
    {
        u64 open_at;
        AdmitVerdict refusal;
    };
    const bool exempt = desc.priority == 0U;
    const Check checks[] = {
        {state.messages.open_at(channel_messages), AdmitVerdict::ChannelMessages},
        {state.bytes.open_at(channel_bytes), AdmitVerdict::ChannelBytes},
        {exempt ? 0U : m_quota.messages.open_at(principal_messages), AdmitVerdict::PrincipalMessages},
        {exempt ? 0U : m_quota.bytes.open_at(principal_bytes), AdmitVerdict::PrincipalBytes},
        {budgeted ? m_quota.budget_operations.open_at(budget_operations) : 0U, AdmitVerdict::BudgetOperations},
        {budgeted ? m_quota.budget_bytes.open_at(budget_bytes) : 0U, AdmitVerdict::BudgetBytes},
    };
    for (const Check& check : checks)
    {
        if (check.open_at > now_ns)
        {
            step.retry_at_ns = check.open_at;
            return check.refusal;
        }
    }
    step.retry_at_ns = kNeverNs;
    if (!m_quota.pending.try_add(1U, priority_limit(limits.pending_messages, desc.priority)))
    {
        return AdmitVerdict::PendingFull;
    }
    if (reassembled && !m_quota.reassembly.try_add(bytes, priority_limit(limits.reassembly_bytes, desc.priority)))
    {
        m_quota.pending.release(1U);
        return AdmitVerdict::ReassemblyFull;
    }
    step.retry_at_ns = 0U;

    state.messages.charge(1U, channel_messages, now_ns);
    state.bytes.charge(bytes, channel_bytes, now_ns);
    m_quota.messages.charge(1U, principal_messages, now_ns);
    m_quota.bytes.charge(bytes, principal_bytes, now_ns);
    if (budgeted)
    {
        m_quota.budget_operations.charge(1U, budget_operations, now_ns);
        m_quota.budget_bytes.charge(bytes, budget_bytes, now_ns);
    }

    // S7: nothing has read a field of the body yet, and on a refusal nothing will.
    const AdmitVerdict verdict = authorize(schema, desc, message, parsed.header.object, delivery);
    if (verdict != AdmitVerdict::Admitted)
    {
        m_quota.pending.release(1U);
        if (reassembled)
        {
            m_quota.reassembly.release(bytes);
        }
        return verdict;
    }

    step.frame = AdmittedFrame{};
    step.frame.schema = schema.wire;
    step.frame.message = &message;
    step.frame.object = parsed.header.object;
    step.frame.channel = channel;
    step.frame.writer_minor = schema.writer_minor;
    step.frame.length = bytes;
    step.frame.hash = parsed.header.hash;
    step.frame.hashed = parsed.hashed;
    step.frame.reassembled = reassembled;
    return AdmitVerdict::Admitted;
}

bool Admission::charge_stepped_over(const ParsedFrame& parsed, u64 now_ns, u64& retry_at_ns) noexcept
{
    const GateRule rule = gate_rule(m_quota.limits.bytes_per_s);
    const u64 open = m_quota.bytes.open_at(rule);
    if (open > now_ns)
    {
        retry_at_ns = open;
        return false;
    }
    m_quota.bytes.charge(parsed.header.length, rule, now_ns);
    return true;
}

AdmitVerdict Admission::charge_padding(u32 channel, u32 bytes, u64 now_ns, u64& retry_at_ns) noexcept
{
    ChannelState& state = m_channels[channel];
    const GateRule channel_bytes = gate_rule(state.desc->bytes_per_s);
    const GateRule principal_bytes = gate_rule(m_quota.limits.bytes_per_s);
    u64 open = state.bytes.open_at(channel_bytes);
    if (open > now_ns)
    {
        retry_at_ns = open;
        return AdmitVerdict::ChannelBytes;
    }
    open = (state.desc->priority == 0U) ? 0U : m_quota.bytes.open_at(principal_bytes);
    if (open > now_ns)
    {
        retry_at_ns = open;
        return AdmitVerdict::PrincipalBytes;
    }
    state.bytes.charge(bytes, channel_bytes, now_ns);
    m_quota.bytes.charge(bytes, principal_bytes, now_ns);
    return AdmitVerdict::Admitted;
}

void Admission::open_body(FrameStep& step, cont::ConstSpan<u8> body) noexcept
{
    AdmittedFrame& frame = step.frame;
    if (frame.hashed && frame_hash(frame.message->id, frame.object, body) != frame.hash)
    {
        finish(frame);
        frame = AdmittedFrame{};
        step.verdict = AdmitVerdict::IntegrityFailed;
        return;
    }
    frame.body = body;
}

void Admission::finish(const AdmittedFrame& frame) noexcept
{
    m_quota.pending.release(1U);
    if (frame.reassembled)
    {
        m_quota.reassembly.release(frame.length);
    }
    if (frame.buffer != nullptr)
    {
        m_allocator.deallocate(frame.buffer);
    }
}

u8* Admission::allocate_buffer(u32 bytes) noexcept
{
    return static_cast<u8*>(m_allocator.try_allocate(bytes, 1U));
}

} // namespace crd::net
