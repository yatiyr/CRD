#pragma once

// events.hpp — Phase 3.1.6 v9-c: event detection with scipy `solve_ivp` semantics. An event is a scalar
// function g(t, y); after every ACCEPTED step the driver checks for a sign change of g between the step
// endpoints (direction-filtered), refines the crossing with brentq over the step's interpolant (Hermite —
// the v9-a fallback; native interpolants slot in later), records the hit, and for a TERMINAL event stops
// the integration exactly at the event time. Events are integration OPTIONS, deliberately not OdeFunction
// virtuals (ADR-0091). ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <utility>

namespace crd::hesap::ode
{

// The event contract (driver-layer, like OdeFunction). value() must be continuous in t and y.
template <typename T> class OdeEvent
{
public:
    virtual ~OdeEvent() = default;

    // g(t, y) — the event fires where g crosses zero.
    [[nodiscard]] virtual T value(T t, crd::containers::ConstSpan<T> y) const = 0;

    // scipy semantics: 0 ⇒ any crossing fires; > 0 ⇒ only g going − → +; < 0 ⇒ only g going + → −.
    [[nodiscard]] virtual T direction() const noexcept { return static_cast<T>(0); }

    // Terminal ⇒ integration stops at the event time (status EventTerminal, OdeResult::event_index set).
    [[nodiscard]] virtual bool terminal() const noexcept { return false; }

    // Optional user-owned recording of every hit time (nullptr ⇒ not recorded).
    [[nodiscard]] virtual crd::containers::Array<T>* hits() const noexcept { return nullptr; }
};

// Adapter for a callable g(T t, ConstSpan<const T> y) -> T.
template <typename T, typename G> class FunctorOdeEvent final : public OdeEvent<T>
{
public:
    FunctorOdeEvent(G g, T direction = static_cast<T>(0), bool terminal = false,
                    crd::containers::Array<T>* hits = nullptr) noexcept
        : m_g(std::move(g)), m_direction(direction), m_terminal(terminal), m_hits(hits)
    {
    }

    [[nodiscard]] T value(T t, crd::containers::ConstSpan<T> y) const override { return m_g(t, y); }
    [[nodiscard]] T direction() const noexcept override { return m_direction; }
    [[nodiscard]] bool terminal() const noexcept override { return m_terminal; }
    [[nodiscard]] crd::containers::Array<T>* hits() const noexcept override { return m_hits; }

private:
    G m_g;
    T m_direction;
    bool m_terminal;
    crd::containers::Array<T>* m_hits;
};

} // namespace crd::hesap::ode
