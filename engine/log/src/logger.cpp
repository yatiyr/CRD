#include "log_formatter.hpp"

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/log/log_channel.hpp>
#include <crd/log/log_record.hpp>
#include <crd/log/logger.hpp>
#include <crd/log/sinks/console_sink.hpp>
#include <crd/log/sinks/debugger_sink.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crd::log
{
// The default channel, defined here. Users CAN log to it (e.g. for examples
// and quick scratch use), but each subsystem should declare its own.
Channel g_log_default{"Default", LogLevel::Trace, nullptr};

namespace
{
// ----------------------------------------------------------------
// Async queue entry. Each entry owns its formatted message string
// because the producer's per-thread scratch buffer is not safe
// to reference from the worker thread.
// ----------------------------------------------------------------
struct QueuedRecord
{
    LogLevel level;
    const Channel* channel;
    std::source_location loc;
    std::chrono::system_clock::time_point time;
    u64 thread_id;
    std::string message;
};

// ----------------------------------------------------------------
// Logger global state.
// Held inside a function-local static so initialisation is well-defined
// (Meyers singleton; thread-safe in C++11+).
// ----------------------------------------------------------------
struct LoggerState
{
    LoggerConfig config;
    std::vector<std::unique_ptr<ISink>> sinks;
    std::mutex sinks_mutex;

    // ---- async machinery --------------------------------------
    std::deque<QueuedRecord> queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::condition_variable drain_cv; // for flush()
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<u64> dropped{0};

    std::atomic<bool> initialized{false};
};

LoggerState& state() noexcept
{
    static LoggerState s;
    return s;
}

// Fan out a single record to all sinks. Caller holds sinks_mutex.
void deliver_to_sinks_locked(LoggerState& st, const LogRecord& rec) noexcept
{
    for (auto& sink : st.sinks)
    {
        if (!sink)
        {
            continue;
        }
        if (static_cast<u8>(rec.level) < static_cast<u8>(sink->min_level()))
        {
            continue;
        }
        sink->write(rec);
    }
}

// Build a transient LogRecord pointing into 'msg' and deliver it.
void deliver_queued(LoggerState& st, const QueuedRecord& q) noexcept
{
    LogRecord rec;
    rec.level = q.level;
    rec.channel = q.channel;
    rec.loc = q.loc;
    rec.time = q.time;
    rec.thread_id = q.thread_id;
    rec.message = q.message;

    std::lock_guard<std::mutex> lock(st.sinks_mutex);
    deliver_to_sinks_locked(st, rec);
}

// Worker thread main. Pops records and delivers them. Exits when
// running == false AND the queue is empty.
void worker_main(LoggerState* st_ptr) noexcept
{
    LoggerState& st = *st_ptr;
    for (;;)
    {
        QueuedRecord q;
        {
            std::unique_lock<std::mutex> lock(st.queue_mutex);
            st.queue_cv.wait(lock, [&] { return !st.queue.empty() || !st.running.load(std::memory_order_acquire); });

            if (st.queue.empty())
            {
                if (!st.running.load(std::memory_order_acquire))
                {
                    // Drain finished. Wake any waiters in flush().
                    st.drain_cv.notify_all();
                    return;
                }
                continue;
            }

            q = std::move(st.queue.front());
            st.queue.pop_front();
        }
        deliver_queued(st, q);

        // If queue is now empty, notify flush() callers.
        {
            std::lock_guard<std::mutex> lock(st.queue_mutex);
            if (st.queue.empty())
            {
                st.drain_cv.notify_all();
            }
        }
    }
}
} // namespace

// ------------------------------------------------------------------
// Bridge: crd-core assert handler -> Critical log + flush
// ------------------------------------------------------------------
//
// Installed by init() so any CRD_ASSERT failure that runs WHILE the logger
// is up gets a Critical record (and a flush) before the platform UI fires.
// We use the default channel here on purpose: the assert is a global event,
// not tied to a particular subsystem. Re-entrancy is already prevented by
// the thread-local guard in crd-core's fire_assert_handler().
namespace
{
void crd_log_default_assert_handler(const char* expression, const char* file, int line, const char* message) noexcept
{
    // Best-effort: if message is null, std::format prints "" cleanly.
    CRD_LOG_CRITICAL(g_log_default, "ASSERT: {} | {}:{} | msg='{}'", expression ? expression : "?", file ? file : "?",
                     line, message ? message : "");
    flush();
}
} // namespace

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

void init(const LoggerConfig& cfg) noexcept
{
    LoggerState& st = state();
    if (st.initialized.load(std::memory_order_acquire))
    {
        return; // double-init is a no-op
    }
    st.config = cfg;

    if (st.config.async)
    {
        st.running.store(true, std::memory_order_release);
        st.worker = std::thread(&worker_main, &st);
    }
    st.initialized.store(true, std::memory_order_release);

    // Install the assert -> log bridge. We don't overwrite a handler the user
    // may have installed manually before init().
    if (::crd::get_assert_handler() == nullptr)
    {
        ::crd::set_assert_handler(&crd_log_default_assert_handler);
    }
}

void shutdown() noexcept
{
    LoggerState& st = state();
    if (!st.initialized.load(std::memory_order_acquire))
    {
        return;
    }

    // Pull our handler back out of crd-core BEFORE we tear sinks down, so a
    // race-y assert during shutdown can't try to log into a dead state.
    if (::crd::get_assert_handler() == &crd_log_default_assert_handler)
    {
        ::crd::set_assert_handler(nullptr);
    }

    if (st.config.async)
    {
        st.running.store(false, std::memory_order_release);
        st.queue_cv.notify_all();
        if (st.worker.joinable())
        {
            st.worker.join();
        }
    }

    flush();

    {
        std::lock_guard<std::mutex> lock(st.sinks_mutex);
        st.sinks.clear();
    }
    st.initialized.store(false, std::memory_order_release);
}

bool is_initialized() noexcept
{
    return state().initialized.load(std::memory_order_acquire);
}

void add_sink(std::unique_ptr<ISink> sink) noexcept
{
    if (!sink)
    {
        return;
    }
    LoggerState& st = state();
    std::lock_guard<std::mutex> lock(st.sinks_mutex);
    st.sinks.push_back(std::move(sink));
}

void clear_sinks() noexcept
{
    LoggerState& st = state();
    std::lock_guard<std::mutex> lock(st.sinks_mutex);
    st.sinks.clear();
}

void flush() noexcept
{
    LoggerState& st = state();

    // Drain async queue (if any).
    if (st.config.async)
    {
        std::unique_lock<std::mutex> lock(st.queue_mutex);
        st.drain_cv.wait(lock, [&] { return st.queue.empty(); });
    }

    std::lock_guard<std::mutex> lock(st.sinks_mutex);
    for (auto& sink : st.sinks)
    {
        if (sink)
        {
            sink->flush();
        }
    }
}

u64 dropped_count() noexcept
{
    return state().dropped.load(std::memory_order_relaxed);
}

namespace detail
{
void dispatch(LogLevel level, const Channel& ch, std::source_location loc, std::string_view message) noexcept
{
    LoggerState& st = state();

    // If init() was never called, we still want logs to "work" for
    // tests / early bootstrap: behave like sync mode with whatever
    // sinks were added.
    const bool async = st.initialized.load(std::memory_order_acquire) && st.config.async;
    const bool flush_critical = st.config.flush_on_critical;

    const auto now = std::chrono::system_clock::now();
    const u64 tid = current_thread_id();

    if (!async || (level == LogLevel::Critical && flush_critical))
    {
        LogRecord rec;
        rec.level = level;
        rec.channel = &ch;
        rec.loc = loc;
        rec.time = now;
        rec.thread_id = tid;
        rec.message = message;

        std::lock_guard<std::mutex> lock(st.sinks_mutex);
        deliver_to_sinks_locked(st, rec);

        if (level == LogLevel::Critical)
        {
            for (auto& sink : st.sinks)
            {
                if (sink)
                {
                    sink->flush();
                }
            }
        }
        return;
    }

    // Async path.
    QueuedRecord q;
    q.level = level;
    q.channel = &ch;
    q.loc = loc;
    q.time = now;
    q.thread_id = tid;
    q.message.assign(message.data(), message.size());

    {
        std::unique_lock<std::mutex> lock(st.queue_mutex);
        if (st.queue.size() >= st.config.async_queue_capacity)
        {
            if (st.config.drop_on_overflow)
            {
                st.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            st.drain_cv.wait(lock, [&] { return st.queue.size() < st.config.async_queue_capacity; });
        }
        st.queue.push_back(std::move(q));
    }
    st.queue_cv.notify_one();
}
} // namespace detail
} // namespace crd::log
