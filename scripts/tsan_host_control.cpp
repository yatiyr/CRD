// The ThreadSanitizer host-separation control (JOBS-HARD.3; docs/design/test-instruments.md#host-separation-diag10a).
// No Cerid code: the shape of the jobs streaming case ("fiber keep: while tiny jobs stream ...") at -O0. Three
// workers and the main thread run frames of tiny jobs while an outside reader walks the pool's owned arrays through
// unique_ptr and vector getters in a tight loop, and every getter result is checked against the value it must hold;
// a wrong value aborts with "=== WRONG". Mode "fiber" runs each job on a fiber announced through the TSan fiber API,
// as the jobs pool does; mode "plain" calls it directly, so no fiber code runs at all. A correct machine never fails
// either mode: on the development host both fault, the plain mode without any fiber, so a fault there is the host's.
// scripts/tsan-host-check.py builds and runs it:
//     clang++ -std=c++20 -O0 -g -fsanitize=thread -fno-omit-frame-pointer -pthread tsan_host_control.cpp
// Without -fsanitize=thread only the plain mode exists (the unsanitized arm).
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CTL_FIBERS 1
#endif
#endif
#ifndef CTL_FIBERS
#define CTL_FIBERS 0
#endif
#if CTL_FIBERS
#include <sanitizer/tsan_interface.h>
#include <sys/mman.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if CTL_FIBERS
extern "C" void ctl_switch(void** from_rsp, void* to_rsp);
extern "C" void __tsan_func_exit(); // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
asm(R"(
.text
.globl ctl_switch
.type ctl_switch, @function
ctl_switch:
    push %r15
    push %r14
    push %r13
    push %r12
    push %rbp
    push %rbx
    mov  %rsp, (%rdi)
    mov  %rsi, %rsp
    pop  %rbx
    pop  %rbp
    pop  %r12
    pop  %r13
    pop  %r14
    pop  %r15
    ret
)");
#endif

namespace
{
constexpr std::size_t kStack   = 64 * 1024;
constexpr int         kThreads = 4; // main and three workers

struct Progress
{
    alignas(64) std::atomic<std::uint32_t> ack{0};
    std::atomic<std::uint64_t> task{0};
    std::atomic<std::uint32_t> executing{0};
};

struct Lane
{
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> popped{0};
    std::uint64_t              capacity = 1024;
    [[nodiscard]] std::uint64_t size() const { return pushed.load(std::memory_order_acquire) - popped.load(std::memory_order_acquire); }
};

struct Fiber
{
    void*  stack    = nullptr;
    void*  rsp      = nullptr;
    void*  tsan     = nullptr;
    void*  tsan_ret = nullptr;
    void** ret_rsp  = nullptr;
};

struct Pool
{
    std::unique_ptr<Progress[]>        progress;
    std::vector<std::unique_ptr<Lane>> lanes;
    std::atomic<std::uint64_t>         done{0};
    std::atomic<std::uint64_t>         remaining{0};
    std::atomic<std::uint32_t>         gen{0};
    std::atomic<bool>                  stop{false};
    std::atomic<std::uint32_t>         in_use{0};
    std::atomic<std::uint32_t>         peak{0};
};

Pool                g_pool;
Progress*           g_progress_expect = nullptr;
std::vector<Lane*>  g_lanes_expect;
bool                g_fibers = true;
std::mutex          g_free_mutex;
std::vector<Fiber*> g_free;
thread_local void*  t_sched_rsp = nullptr;
thread_local Fiber* t_current   = nullptr;

[[noreturn]] void wrong(const char* what, const void* got, const void* want)
{
    std::fprintf(stderr, "=== WRONG %s: got %p, want %p\n", what, got, want);
    std::fflush(stderr);
    std::abort();
}

Progress& progress_of(int idx)
{
    Progress& p = g_pool.progress[static_cast<std::size_t>(idx)];
    if (&p != g_progress_expect + idx)
    {
        wrong("progress", &p, g_progress_expect + idx);
    }
    return p;
}

void job_body() noexcept
{
    volatile std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < 64; ++i)
    {
        sum = sum + i * i;
    }
    g_pool.done.fetch_add(1, std::memory_order_acq_rel);
}

#if CTL_FIBERS
[[noreturn]] void fiber_entry() noexcept
{
    for (;;)
    {
        job_body();
        Fiber* const f = t_current;
        __tsan_func_exit();
        __tsan_switch_to_fiber(f->tsan_ret, 0);
        ctl_switch(&f->rsp, *f->ret_rsp);
    }
}

void init_stack(Fiber* f)
{
    auto* top = reinterpret_cast<std::uintptr_t*>(static_cast<char*>(f->stack) + kStack);
    top       = reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(top) & ~std::uintptr_t{15});
    *--top    = 0;
    *--top    = reinterpret_cast<std::uintptr_t>(&fiber_entry);
    for (int i = 0; i < 6; ++i)
    {
        *--top = 0;
    }
    f->rsp = top;
}
#endif

bool run_one(int idx) noexcept
{
    std::uint64_t left = g_pool.remaining.load(std::memory_order_relaxed);
    while (left != 0 && !g_pool.remaining.compare_exchange_weak(left, left - 1, std::memory_order_acq_rel))
    {
    }
    if (left == 0)
    {
        return false;
    }
    Lane& lane = *g_pool.lanes[static_cast<std::size_t>(idx)];
    lane.popped.fetch_add(1, std::memory_order_release);
    progress_of(idx).task.store(left, std::memory_order_relaxed);
    progress_of(idx).executing.store(1, std::memory_order_relaxed);
    const std::uint32_t now  = g_pool.in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint32_t       peak = g_pool.peak.load(std::memory_order_relaxed);
    while (now > peak && !g_pool.peak.compare_exchange_weak(peak, now, std::memory_order_relaxed))
    {
    }
#if CTL_FIBERS
    if (g_fibers)
    {
        Fiber* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_free_mutex);
            f = g_free.back();
            g_free.pop_back();
        }
        init_stack(f);
        f->ret_rsp  = &t_sched_rsp;
        f->tsan_ret = __tsan_get_current_fiber();
        t_current   = f;
        __tsan_switch_to_fiber(f->tsan, 0);
        ctl_switch(&t_sched_rsp, f->rsp);
        t_current = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_free_mutex);
            g_free.push_back(f);
        }
    }
    else
#endif
    {
        job_body();
    }
    g_pool.in_use.fetch_sub(1, std::memory_order_relaxed);
    progress_of(idx).executing.store(0, std::memory_order_relaxed);
    progress_of(idx).task.store(0, std::memory_order_relaxed);
    lane.pushed.fetch_add(1, std::memory_order_release);
    return true;
}

void worker(int idx) noexcept
{
    while (!g_pool.stop.load(std::memory_order_acquire))
    {
        progress_of(idx).ack.store(g_pool.gen.load(std::memory_order_acquire), std::memory_order_release);
        if (!run_one(idx))
        {
            std::this_thread::yield();
        }
    }
}

std::uint64_t read_pool() noexcept
{
    std::uint64_t v = g_pool.done.load(std::memory_order_relaxed) + g_pool.in_use.load(std::memory_order_relaxed) +
                      g_pool.peak.load(std::memory_order_relaxed);
    const Lane* const firsts[3] = {g_pool.lanes[0].get(), g_pool.lanes[1].get(), g_pool.lanes[2].get()};
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (firsts[i] != g_lanes_expect[i])
        {
            wrong("lane array", firsts[i], g_lanes_expect[i]);
        }
        v += firsts[i]->capacity + firsts[i]->size();
    }
    std::size_t i = 0;
    for (const std::unique_ptr<Lane>& lane : g_pool.lanes)
    {
        if (lane.get() != g_lanes_expect[i])
        {
            wrong("lane walk", lane.get(), g_lanes_expect[i]);
        }
        v += lane->size();
        ++i;
    }
    for (int t = 0; t < kThreads; ++t)
    {
        v += progress_of(t).ack.load(std::memory_order_relaxed) + progress_of(t).executing.load(std::memory_order_relaxed);
    }
    return v;
}
} // namespace

int main(int argc, char** argv)
{
    g_fibers           = CTL_FIBERS && (argc > 1) && (std::strcmp(argv[1], "fiber") == 0);
    const int frames   = (argc > 2) ? std::atoi(argv[2]) : 200;
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        g_pool.progress   = std::make_unique<Progress[]>(kThreads);
        g_progress_expect = g_pool.progress.get();
        g_pool.lanes.clear();
        g_lanes_expect.clear();
        for (int t = 0; t < kThreads; ++t)
        {
            g_pool.lanes.push_back(std::make_unique<Lane>());
            g_lanes_expect.push_back(g_pool.lanes.back().get());
        }
        std::vector<Fiber*> all;
#if CTL_FIBERS
        if (g_fibers)
        {
            for (int i = 0; i < 208; ++i)
            {
                auto* f  = new Fiber;
                f->stack = ::mmap(nullptr, kStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                f->tsan  = __tsan_create_fiber(0);
                all.push_back(f);
            }
        }
#endif
        g_free = all;
        g_pool.stop.store(false);
        std::vector<std::thread> workers;
        for (int i = 1; i < kThreads; ++i)
        {
            workers.emplace_back(&worker, i);
        }
        std::atomic<bool>          reading{true};
        std::atomic<std::uint64_t> sampled{0};
        std::thread                reader(
            [&]
            {
                while (reading.load(std::memory_order_acquire))
                {
                    const std::uint64_t v = read_pool();
                    sampled.fetch_add(1U + (v & 0U), std::memory_order_seq_cst); // a seq_cst RMW per sample, as the pin
                }
            });
        for (int frame = 0; frame < frames; ++frame)
        {
            const std::uint64_t target = g_pool.done.load() + 512;
            g_pool.gen.fetch_add(1, std::memory_order_acq_rel);
            g_pool.remaining.store(512, std::memory_order_release);
            while (g_pool.done.load(std::memory_order_acquire) < target)
            {
                if (!run_one(0))
                {
                    std::this_thread::yield();
                }
            }
        }
        reading.store(false);
        reader.join();
        g_pool.stop.store(true, std::memory_order_release);
        for (std::thread& w : workers)
        {
            w.join();
        }
#if CTL_FIBERS
        for (Fiber* f : all)
        {
            __tsan_destroy_fiber(f->tsan);
            ::munmap(f->stack, kStack);
            delete f;
        }
#endif
        g_free.clear();
        g_pool.progress.reset();
        std::printf("cycle %d done, %llu jobs, %llu samples\n", cycle, static_cast<unsigned long long>(g_pool.done.load()),
                    static_cast<unsigned long long>(sampled.load()));
    }
    return 0;
}
