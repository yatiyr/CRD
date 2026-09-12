#pragma once

#include <crd/app/event.hpp>
#include <crd/containers/array.hpp>

#include <functional>
#include <type_traits>
#include <utility>

namespace crd::app
{
struct EventSubscription
{
    EventTypeId type_id = nullptr;
    crd::u64 subscription_id = 0;

    [[nodiscard]] bool is_valid() const noexcept { return type_id != nullptr && subscription_id != 0; }
};

class EventBus
{
public:
    EventBus() = default;

    template <typename EventType, typename Fn> EventSubscription subscribe(Fn&& fn)
    {
        static_assert(std::is_base_of_v<Event, EventType>);

        Bucket& bucket = find_or_create_bucket(EventType::static_type_id());
        Subscriber subscriber;
        subscriber.id = m_next_subscription_id++;
        subscriber.callback = [handler = std::forward<Fn>(fn)](Event& event) mutable
        {
            handler(static_cast<EventType&>(event));
        };
        bucket.subscribers.push_back(std::move(subscriber));
        return EventSubscription{EventType::static_type_id(), bucket.subscribers.back().id};
    }

    void unsubscribe(EventSubscription subscription) noexcept;

    template <typename EventType> void publish(EventType& event)
    {
        static_assert(std::is_base_of_v<Event, EventType>);

        Bucket* bucket = find_bucket(EventType::static_type_id());
        if (bucket == nullptr)
        {
            return;
        }
        for (auto& subscriber : bucket->subscribers)
        {
            subscriber.callback(event);
        }
    }

private:
    struct Subscriber
    {
        crd::u64 id = 0;
        std::function<void(Event&)> callback{};
    };

    struct Bucket
    {
        EventTypeId type_id = nullptr;
        crd::containers::Array<Subscriber> subscribers{};
    };

    [[nodiscard]] Bucket* find_bucket(EventTypeId type_id) noexcept;
    [[nodiscard]] const Bucket* find_bucket(EventTypeId type_id) const noexcept;
    Bucket& find_or_create_bucket(EventTypeId type_id);

    crd::containers::Array<Bucket> m_buckets{};
    crd::u64 m_next_subscription_id = 1;
};
} // namespace crd::app
