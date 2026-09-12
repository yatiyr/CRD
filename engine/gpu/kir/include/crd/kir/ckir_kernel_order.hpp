#pragma once

#include <crd/kir/ckir.hpp>

#include <crd/containers/array.hpp>

namespace crd::kir::emit_detail
{

// Shared by compute dialects: hoisting must not move a consumer before its explicit snapshot, statement result,
// or resource producer. This analysis does not freeze live loads; Materialize retains that separate IR meaning.
class KernelEmissionOrder
{
public:
    KernelEmissionOrder(const KGraph& graph, crd::memory::IAllocator* scratch)
        : m_graph(graph), m_materialized(scratch), m_written(scratch), m_deferred(scratch)
    {
        const auto count = static_cast<crd::usize>(graph.size());
        m_materialized.resize(count, 0U);
        m_written.resize(count, 0U);
        m_deferred.resize(count, -1);
        for (int index = 0; index < graph.stmt_count(); ++index)
        {
            const KStmt& statement = graph.stmt(index);
            if (statement.kind == KStmtKind::Materialize && statement.value >= 0)
            {
                m_materialized[static_cast<crd::usize>(statement.value)] = 1U;
            }
            if ((statement.kind == KStmtKind::TraceRayClosest || statement.kind == KStmtKind::TraceRayHit ||
                 statement.kind == KStmtKind::BufferAtomicAddFetch || statement.kind == KStmtKind::BufferAtomicExchange) &&
                statement.result >= 0)
            {
                m_materialized[static_cast<crd::usize>(statement.result)] = 1U;
            }
            if ((statement.kind == KStmtKind::BufferStore || statement.kind == KStmtKind::SharedStore ||
                 statement.kind == KStmtKind::BufferAtomicAdd || statement.kind == KStmtKind::BufferAtomicMin ||
                 statement.kind == KStmtKind::BufferAtomicAddFetch || statement.kind == KStmtKind::BufferAtomicExchange ||
                 statement.kind == KStmtKind::SharedAtomicAdd) && statement.target >= 0)
            {
                m_written[static_cast<crd::usize>(statement.target)] = 1U;
            }
        }
    }

    [[nodiscard]] bool must_defer(int node)
    {
        if (node < 0) { return false; }
        const auto index = static_cast<crd::usize>(node);
        if (m_deferred[index] >= 0) { return m_deferred[index] != 0; }
        const KNode& value = m_graph.node(node);
        bool deferred = m_materialized[index] != 0U;
        if (!deferred && (value.op == KOp::BufferLoad || value.op == KOp::SharedLoad) && value.a >= 0)
        {
            deferred = m_written[static_cast<crd::usize>(value.a)] != 0U;
        }
        if (!deferred) { deferred = must_defer(value.a); }
        if (!deferred) { deferred = must_defer(value.b); }
        if (!deferred) { deferred = must_defer(value.c); }
        if (!deferred) { deferred = must_defer(value.d); }
        for (int operand = 0; !deferred && operand < static_cast<int>(value.n_ext); ++operand)
        {
            deferred = must_defer(m_graph.ext_operand(value, operand));
        }
        m_deferred[index] = deferred ? static_cast<crd::i8>(1) : static_cast<crd::i8>(0);
        return deferred;
    }

private:
    const KGraph& m_graph;
    crd::containers::Array<crd::u8> m_materialized;
    crd::containers::Array<crd::u8> m_written;
    crd::containers::Array<crd::i8> m_deferred;
};

} // namespace crd::kir::emit_detail
