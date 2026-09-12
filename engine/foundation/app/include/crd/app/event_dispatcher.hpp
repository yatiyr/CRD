#pragma once

#include <crd/app/event.hpp>

#include <type_traits>
#include <utility>

namespace crd::app
{
class EventDispatcher
{
public:
    explicit EventDispatcher(Event& event) noexcept : m_event(event) {}

    template <typename EventType, typename Fn> bool dispatch(Fn&& fn)
    {
        static_assert(std::is_base_of_v<Event, EventType>);
        if (m_event.type_id() != EventType::static_type_id())
        {
            return false;
        }

        m_event.handled = m_event.handled || static_cast<bool>(std::forward<Fn>(fn)(static_cast<EventType&>(m_event)));
        return true;
    }

private:
    Event& m_event;
};
} // namespace crd::app
