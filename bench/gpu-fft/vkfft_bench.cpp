// vkfft_bench.cpp — GPU FFT crush campaign: the VkFFT peer (the specialized-Vulkan gold standard, the real head-to-head for
// our Vulkan FFT). Batched C2C forward, f32, min-of-N, num_iter appends per submit (amortizes submit overhead → steady-state
// GPU throughput = VkFFT's strongest number = the most conservative bar for us). Same sizes + 5*N*log2(N)*batch FLOP model as
// docs/bench/2026-07-13-gpu-fft-cufft-gold.md. Reuses VkFFT's utils_VkFFT (Vulkan device/fence/commandPool + allocateBuffer).

#define VKFFT_BACKEND 0
#include "vkFFT.h"
#include "utils_VkFFT.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

// timed: record `num_iter` FFTs into one command buffer, submit, wait — return the CPU-measured ms (submit overhead
// amortized over num_iter → per-FFT time approaches pure GPU throughput).
static double timed_appends(VkGPU* vkGPU, VkFFTApplication* app, VkFFTLaunchParams* lp, uint64_t num_iter)
{
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool                 = vkGPU->commandPool;
    cbai.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount          = 1;
    VkCommandBuffer cb               = {};
    vkAllocateCommandBuffers(vkGPU->device, &cbai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    lp->commandBuffer = &cb;
    for (uint64_t i = 0; i < num_iter; ++i) { VkFFTAppend(app, -1, lp); }
    vkEndCommandBuffer(cb);
    VkSubmitInfo si         = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cb;
    auto t0                 = std::chrono::steady_clock::now();
    vkQueueSubmit(vkGPU->queue, 1, &si, vkGPU->fence);
    vkWaitForFences(vkGPU->device, 1, &vkGPU->fence, VK_TRUE, 100000000000ULL);
    auto t1 = std::chrono::steady_clock::now();
    vkResetFences(vkGPU->device, 1, &vkGPU->fence);
    vkFreeCommandBuffers(vkGPU->device, vkGPU->commandPool, 1, &cb);
    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() * 0.001;
}

int main()
{
    VkGPU vkGPU     = {};
    vkGPU.device_id = 0;
    if (createInstance(&vkGPU, 0) != VK_SUCCESS) { std::printf("createInstance failed\n"); return 1; }
    if (findPhysicalDevice(&vkGPU) != VK_SUCCESS) { std::printf("findPhysicalDevice failed\n"); return 1; }
    if (createDevice(&vkGPU, 0) != VK_SUCCESS) { std::printf("createDevice failed\n"); return 1; }
    if (createFence(&vkGPU) != VK_SUCCESS) { std::printf("createFence failed\n"); return 1; }
    if (createCommandPool(&vkGPU) != VK_SUCCESS) { std::printf("createCommandPool failed\n"); return 1; }
    vkGetPhysicalDeviceProperties(vkGPU.physicalDevice, &vkGPU.physicalDeviceProperties);
    glslang_initialize_process();

    std::printf("# VkFFT C2C forward (f32) — %s\n", vkGPU.physicalDeviceProperties.deviceName);
    std::printf("# %-8s %-10s %-10s %-10s\n", "N", "batch", "min_ms", "GFLOP/s");

    const uint64_t sizes[] = {256, 512, 1024, 2048, 4096};
    for (int si = 0; si < 5; ++si)
    {
        const uint64_t n     = sizes[si];
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
        if (allocateBuffer(&vkGPU, &buffer, &bufferMem,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, bufferSize) != VKFFT_SUCCESS)
        {
            std::printf("# alloc fail N=%llu\n", (unsigned long long)n);
            continue;
        }
        cfg.buffer     = &buffer;
        cfg.bufferSize = &bufferSize;

        VkFFTApplication app = {};
        if (initializeVkFFT(&app, cfg) != VKFFT_SUCCESS) { std::printf("# initializeVkFFT fail N=%llu\n", (unsigned long long)n); continue; }
        VkFFTLaunchParams lp = {};

        const uint64_t num_iter = 20;
        timed_appends(&vkGPU, &app, &lp, num_iter); // warmup
        double best = 1e30;
        for (int r = 0; r < 20; ++r)
        {
            const double per = timed_appends(&vkGPU, &app, &lp, num_iter) / static_cast<double>(num_iter);
            if (per < best) { best = per; }
        }
        const double gflops = 5.0 * (double)n * (std::log((double)n) / std::log(2.0)) * (double)batch / (best * 1e-3) / 1e9;
        std::printf("%-8llu %-10llu %-10.4f %-10.1f\n", (unsigned long long)n, (unsigned long long)batch, best, gflops);

        deleteVkFFT(&app);
        vkDestroyBuffer(vkGPU.device, buffer, nullptr);
        vkFreeMemory(vkGPU.device, bufferMem, nullptr);
    }
    glslang_finalize_process();
    return 0;
}
