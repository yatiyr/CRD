// bench_gpu_fft_vkfft.cpp — GPU FFT crush campaign: the VkFFT peer (the specialized-Vulkan gold standard, the real
// head-to-head for our Vulkan FFT). Batched C2C forward, f32, min-of-N, num_iter appends per submit (amortizes submit
// overhead → steady-state GPU throughput = VkFFT's strongest number = the most conservative bar for us). Same sizes +
// 5*N*log2(N)*batch FLOP model as docs/bench/2026-07-13-gpu-fft-cufft-gold.md. Reuses VkFFT's utils_VkFFT (Vulkan
// device/fence/commandPool + allocateBuffer). Built by CRD_BUILD_GPU_VS_VKFFT (runtime/CMakeLists.txt, which defines
// VKFFT_BACKEND=0, the Vulkan backend) against the pinned VkFFT (cmake/pins.json), run by
// scripts/run_bench_gpu_fft_vkfft.sh.

#include "vkFFT.h"
#include "utils_VkFFT.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

// timed: record `num_iter` FFTs into one command buffer, submit, wait; the CPU-measured ms in `ms` (submit overhead
// amortized over num_iter → per-FFT time approaches pure GPU throughput). False when a Vulkan or VkFFT call fails.
static bool timed_appends(VkGPU* vkGPU, VkFFTApplication* app, VkFFTLaunchParams* lp, uint64_t num_iter, double& ms)
{
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool                 = vkGPU->commandPool;
    cbai.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount          = 1;
    VkCommandBuffer cb               = {};
    if (vkAllocateCommandBuffers(vkGPU->device, &cbai, &cb) != VK_SUCCESS)
    {
        return false;
    }
    VkCommandBufferBeginInfo bi = {};
    bi.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool recorded               = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
    lp->commandBuffer           = &cb;
    for (uint64_t i = 0; recorded && i < num_iter; ++i)
    {
        recorded = VkFFTAppend(app, -1, lp) == VKFFT_SUCCESS;
    }
    recorded = vkEndCommandBuffer(cb) == VK_SUCCESS && recorded;
    bool done = false;
    if (recorded)
    {
        VkSubmitInfo si       = {};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        const auto t0         = std::chrono::steady_clock::now();
        done = vkQueueSubmit(vkGPU->queue, 1, &si, vkGPU->fence) == VK_SUCCESS &&
               vkWaitForFences(vkGPU->device, 1, &vkGPU->fence, VK_TRUE, 100000000000ULL) == VK_SUCCESS;
        const auto t1 = std::chrono::steady_clock::now();
        ms            = std::chrono::duration<double, std::milli>(t1 - t0).count();
        done          = vkResetFences(vkGPU->device, 1, &vkGPU->fence) == VK_SUCCESS && done;
    }
    vkFreeCommandBuffers(vkGPU->device, vkGPU->commandPool, 1, &cb);
    return done;
}

int main()
{
    VkGPU vkGPU     = {};
    vkGPU.device_id = 0;
    if (createInstance(&vkGPU, 0) != VK_SUCCESS)
    {
        std::printf("createInstance failed\n");
        return 1;
    }
    if (findPhysicalDevice(&vkGPU) != VK_SUCCESS)
    {
        std::printf("findPhysicalDevice failed\n");
        return 1;
    }
    if (createDevice(&vkGPU, 0) != VK_SUCCESS)
    {
        std::printf("createDevice failed\n");
        return 1;
    }
    if (createFence(&vkGPU) != VK_SUCCESS)
    {
        std::printf("createFence failed\n");
        return 1;
    }
    if (createCommandPool(&vkGPU) != VK_SUCCESS)
    {
        std::printf("createCommandPool failed\n");
        return 1;
    }
    vkGetPhysicalDeviceProperties(vkGPU.physicalDevice, &vkGPU.physicalDeviceProperties);
    glslang_initialize_process();

    std::printf("# VkFFT C2C forward (f32) — %s\n", vkGPU.physicalDeviceProperties.deviceName);
    std::printf("# %-8s %-10s %-10s %-10s\n", "N", "batch", "min_ms", "GFLOP/s");

    // A size whose buffer, plan or submits fail prints its reason and fails the run, so a board never reads a partial
    // table as a whole one.
    bool                     failed  = false;
    const uint64_t           sizes[] = {256, 512, 1024, 2048, 4096};
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    for (const uint64_t n : sizes)
    {
        const uint64_t batch = (16ULL << 20) / n; // ~16.7M complex elements total

        VkFFTConfiguration cfg = {};
        cfg.FFTdim             = 1;
        cfg.size[0]            = n;
        cfg.numberBatches      = batch;
        cfg.device             = &vkGPU.device;
        cfg.queue              = &vkGPU.queue;
        cfg.fence              = &vkGPU.fence;
        cfg.commandPool        = &vkGPU.commandPool;
        cfg.physicalDevice     = &vkGPU.physicalDevice;

        uint64_t       bufferSize = sizeof(float) * 2 * n * batch;
        VkBuffer       buffer     = {};
        VkDeviceMemory bufferMem  = {};
        if (allocateBuffer(&vkGPU, &buffer, &bufferMem, usage, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, bufferSize) !=
            VKFFT_SUCCESS)
        {
            std::printf("# alloc fail N=%llu\n", static_cast<unsigned long long>(n));
            failed = true;
            continue;
        }
        cfg.buffer     = &buffer;
        cfg.bufferSize = &bufferSize;

        VkFFTApplication app = {};
        if (initializeVkFFT(&app, cfg) != VKFFT_SUCCESS)
        {
            std::printf("# initializeVkFFT fail N=%llu\n", static_cast<unsigned long long>(n));
            failed = true;
        }
        else
        {
            VkFFTLaunchParams lp       = {};
            const uint64_t    num_iter = 20;
            double            ms       = 0.0;
            bool              timed    = timed_appends(&vkGPU, &app, &lp, num_iter, ms); // warmup
            double            best     = 1e30;
            for (int r = 0; timed && r < 20; ++r)
            {
                timed            = timed_appends(&vkGPU, &app, &lp, num_iter, ms);
                const double per = ms / static_cast<double>(num_iter);
                if (per < best)
                {
                    best = per;
                }
            }
            if (timed)
            {
                const double flop   = 5.0 * static_cast<double>(n) * std::log2(static_cast<double>(n)) *
                                    static_cast<double>(batch);
                const double gflops = flop / (best * 1e-3) / 1e9;
                std::printf("%-8llu %-10llu %-10.4f %-10.1f\n", static_cast<unsigned long long>(n),
                            static_cast<unsigned long long>(batch), best, gflops);
            }
            else
            {
                std::printf("# submit fail N=%llu\n", static_cast<unsigned long long>(n));
                failed = true;
            }
            deleteVkFFT(&app);
        }
        vkDestroyBuffer(vkGPU.device, buffer, nullptr);
        vkFreeMemory(vkGPU.device, bufferMem, nullptr);
    }
    glslang_finalize_process();
    return failed ? 1 : 0;
}
