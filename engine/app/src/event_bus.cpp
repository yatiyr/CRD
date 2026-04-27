#include <crd/app/event_bus.hpp>

namespace crd::app
{
EventBus::Bucket* EventBus::find_bucket(EventTypeId type_id) noexcept
{
    for (auto& bucket : m_buckets)
    {
        if (bucket.type_id == type_id)
        {
            return &bucket;
        }
    }
    return nullptr;
}

const EventBus::Bucket* EventBus::find_bucket(EventTypeId type_id) const noexcept
{
    for (const auto& bucket : m_buckets)
    {
        if (bucket.type_id == type_id)
        {
            return &bucket;
        }
    }
    return nullptr;
}

EventBus::Bucket& EventBus::find_or_create_bucket(EventTypeId type_id)
{
    if (Bucket* existing = find_bucket(type_id); existing != nullptr)
    {
        return *existing;
    }

    Bucket bucket;
    bucket.type_id = type_id;
    m_buckets.push_back(std::move(bucket));
    return m_buckets.back();
}

void EventBus::unsubscribe(EventSubscription subscription) noexcept
{
    Bucket* bucket = find_bucket(subscription.type_id);
    if (bucket == nullptr)
    {
        return;
    }

    for (crd::usize i = 0; i < bucket->subscribers.size(); ++i)
    {
        if (bucket->subscribers[i].id == subscription.subscription_id)
        {
            bucket->subscribers.swap_remove(i);
            return;
        }
    }
}
} // namespace crd::app
