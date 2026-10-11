#include <crd/app/event_bus.hpp>

namespace crd::app
{
EventBus::~EventBus()
{
    release_all_subscriptions();
}

EventBus::EventBus(EventBus&& other) noexcept
    : m_buckets(std::move(other.m_buckets))
    , m_next_subscription_id(other.m_next_subscription_id)
{
}

EventBus& EventBus::operator=(EventBus&& other) noexcept
{
    if (this != &other)
    {
        release_all_subscriptions(); // the subscriptions this bus held end here
        m_buckets              = std::move(other.m_buckets);
        m_next_subscription_id = other.m_next_subscription_id;
    }
    return *this;
}

void EventBus::release_all_subscriptions() noexcept
{
    for (const Bucket& bucket : m_buckets)
    {
        for (crd::usize i = 0; i < bucket.subscribers.size(); ++i)
        {
            crd::lifetime_ledger().released(crd::LifetimeClass::CallbackRegistration);
        }
    }
    m_buckets.clear();
}

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
            crd::lifetime_ledger().released(crd::LifetimeClass::CallbackRegistration);
            return;
        }
    }
}
} // namespace crd::app
