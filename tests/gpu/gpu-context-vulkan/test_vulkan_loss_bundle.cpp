// DIAG.7c(g): a recorded Vulkan device loss is written as a readable bundle, the counterpart of DX12's removal bundle
// (DIAG.7b(f)). The loss is injected at the completion seam: a real loss stays the hardware gate (DIAG.7c(h)), so every
// bundle here is labelled Simulated. The device's VK_EXT_device_fault report, when the extension is enabled, is read
// before the bundle is written and travels inside it.
//
// Live dumps are a Windows facility (crash::capture_dump). On Linux the loss is still recorded and kept in process, and
// the write honestly reports Unsupported; the round trip and the reader's refusals are checked on Windows.

#include "vulkan_execution.hpp" // the completion seam under test (src-private)

#include <crd/core/crash.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/memory/allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#if defined(_WIN32)
#include <process.h> // _getpid: a per-process scratch directory
#endif

namespace
{
namespace gpu = crd::gpu;
namespace d = crd::gpu::detail;

// Inject a loss into one compute submission on `vk`; the context latches invalid.
void inject_compute_loss(gpu::VulkanGpuContext& vk)
{
    gpu::VulkanComputeContext compute(vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    d::vk_inject_next_result(VK_ERROR_DEVICE_LOST);
    (void)compute.begin();
    compute.submit_and_wait();
    CHECK_FALSE(compute.valid());
}

#if defined(_WIN32)
[[nodiscard]] crd::u32 count_dumps_with_prefix(const std::filesystem::path& dir, const char* prefix)
{
    crd::u32 n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with(prefix))
        {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] std::filesystem::path first_dump_with_prefix(const std::filesystem::path& dir, const char* prefix)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with(prefix))
        {
            return it->path();
        }
    }
    return {};
}

// Write a hang dump whose evidence is `bytes`, and read it back as a loss bundle.
[[nodiscard]] d::VkBundleRead read_foreign(const std::filesystem::path& dir, const void* bytes, crd::u32 size)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().filename().string().starts_with("hang_"))
        {
            std::filesystem::remove(it->path(), ec);
        }
    }
    crd::crash::DumpNote note{};
    note.kind = crd::crash::DumpKind::Hang;
    note.evidence = bytes;
    note.evidence_bytes = size;
    REQUIRE(crd::crash::WriteResult::Ok == crd::crash::capture_dump(note, nullptr));
    const std::filesystem::path hang = first_dump_with_prefix(dir, "hang_");
    REQUIRE_FALSE(hang.empty());
    d::VkLossRecord read{};
    return d::vk_read_loss_bundle(hang.c_str(), read);
}
#endif
} // namespace

TEST_CASE("DIAG.7c(g): a recorded Vulkan loss is written as a readable bundle",
          "[gpu-context][vulkan][gpu][device-loss]")
{
    using crd::crash::WriteResult;
    gpu::GpuContextConfig cfg;
    cfg.backend = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto first = gpu::create_vulkan_gpu_context(cfg);
    if (first == nullptr || !first->valid())
    {
        SKIP("no Vulkan device");
    }
    auto* const vk_first = static_cast<gpu::VulkanGpuContext*>(first.get());
    std::printf("[DIAG.7c(g)] adapter=%s VK_EXT_device_fault=%s\n", vk_first->adapter_name(),
                vk_first->device_fault() ? "enabled" : "absent");

    // Without crash capture (and on any platform without live dumps): the loss is recorded and kept, nothing written.
    crd::crash::uninstall();
    inject_compute_loss(*vk_first);
    const d::VkLossRecord kept = d::vk_last_loss();
    CHECK(kept.loss_origin == d::VkFailureOrigin::Simulated);
    CHECK(kept.origin == d::VkFailureOrigin::Simulated);
    CHECK(kept.first_result == static_cast<crd::i32>(VK_ERROR_DEVICE_LOST));
    CHECK(std::strcmp(kept.operation, "compute submit") == 0);
    CHECK(std::strcmp(kept.loss_operation, "compute submit") == 0);
    CHECK(std::strcmp(kept.adapter.name, vk_first->adapter_name()) == 0);
    CHECK(kept.adapter.vendor_id != 0U);
    CHECK(kept.adapter.api_version != 0U);
    CHECK(kept.fault.available == vk_first->device_fault());
    CHECK(kept.fault.queried == vk_first->device_fault()); // the fault report is read before the bundle is written
#if defined(_WIN32)
    CHECK(kept.bundle_result == static_cast<crd::u32>(WriteResult::NotInstalled));
#else
    CHECK(kept.bundle_result == static_cast<crd::u32>(WriteResult::Unsupported));
#endif
    first.reset();
    CHECK(d::vk_last_loss().sequence == kept.sequence); // the last-known loss outlives its device

#if defined(_WIN32)
    namespace fs = std::filesystem;
    char leaf[64];
    (void)std::snprintf(leaf, sizeof(leaf), "crd_vk_loss_bundle_%d", _getpid());
    const fs::path dir = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(crd::crash::InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    auto second = gpu::create_vulkan_gpu_context(cfg);
    REQUIRE(second != nullptr);
    REQUIRE(second->valid());
    auto* const vk = static_cast<gpu::VulkanGpuContext*>(second.get());
    const VkDevice device = vk->vk_device();

    // A failure that is not a loss writes nothing: a bounded wait on a fence nobody signals times out.
    {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        REQUIRE(vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS);
        CHECK(d::vk_wait(device, fence, 1'000'000ULL, "test wait") == VK_TIMEOUT);
        vkDestroyFence(device, fence, nullptr);
    }
    CHECK_FALSE(d::vk_device_failure(device).lost());
    CHECK(count_dumps_with_prefix(dir, "gpu_") == 0U);

    // The loss that follows the timeout is still a loss: it is recorded as one and written once.
    inject_compute_loss(*vk);
    const d::VkDeviceFailure failure = d::vk_device_failure(device);
    CHECK(failure.origin == d::VkFailureOrigin::TimedOut); // the first failure is kept
    CHECK(failure.lost());
    const d::VkLossRecord written = d::vk_last_loss();
    CHECK(written.bundle_result == static_cast<crd::u32>(WriteResult::Ok));
    CHECK(written.sequence == failure.sequence);
    CHECK(count_dumps_with_prefix(dir, "gpu_") == 1U);

    // A later failure on the lost device writes no second bundle.
    VkSubmitInfo empty{};
    empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    CHECK(d::vk_submit(device, vk->compute_queue(), empty, VK_NULL_HANDLE, "later submit") == VK_ERROR_DEVICE_LOST);
    CHECK(count_dumps_with_prefix(dir, "gpu_") == 1U);

    const fs::path bundle_path = first_dump_with_prefix(dir, "gpu_");
    REQUIRE_FALSE(bundle_path.empty());
    d::VkLossRecord read{};
    REQUIRE(d::vk_read_loss_bundle(bundle_path.c_str(), read) == d::VkBundleRead::Ok);
    CHECK(read.origin == d::VkFailureOrigin::TimedOut);
    CHECK(read.first_result == static_cast<crd::i32>(VK_TIMEOUT));
    CHECK(std::strcmp(read.operation, "test wait") == 0);
    CHECK(read.loss_origin == d::VkFailureOrigin::Simulated);
    CHECK(std::strcmp(read.loss_operation, "compute submit") == 0);
    CHECK(read.sequence == written.sequence);
    CHECK(read.failures == 2U); // the timeout and the loss; the later submit came after the write
    CHECK(std::strcmp(read.adapter.name, vk->adapter_name()) == 0);
    CHECK(read.adapter.vendor_id == written.adapter.vendor_id);
    CHECK(read.adapter.driver_version == written.adapter.driver_version);
    CHECK(read.fault.available == vk->device_fault());
    CHECK(read.fault.queried == vk->device_fault());
    CHECK(read.fault.query_result == d::vk_device_fault_report(device).query_result);
    CHECK(read.bundle_result == static_cast<crd::u32>(WriteResult::NotInstalled)); // the copy before the write

    // Refusals: no evidence, foreign evidence (DX12's removal magic among it), another version, another size.
    REQUIRE(WriteResult::Ok == crd::crash::capture_dump(nullptr));
    const fs::path plain = first_dump_with_prefix(dir, "live_");
    REQUIRE_FALSE(plain.empty());
    CHECK(d::vk_read_loss_bundle(plain.c_str(), read) == d::VkBundleRead::NoStream);
    const crd::u32 foreign[4] = {0x11111111U, 1U, 2U, 3U};
    CHECK(read_foreign(dir, foreign, sizeof(foreign)) == d::VkBundleRead::BadMagic);
    const crd::u32 dx12_removal[4] = {0x52445243U, 2U, 16U, 0U}; // 'CRDR', the DX12 removal bundle header
    CHECK(read_foreign(dir, dx12_removal, sizeof(dx12_removal)) == d::VkBundleRead::BadMagic);
    const d::VkLossBundleHeader other_version{d::kVkLossBundleMagic, d::kVkLossBundleVersion + 1U,
                                              static_cast<crd::u32>(sizeof(d::VkLossRecord)), 0U};
    CHECK(read_foreign(dir, &other_version, sizeof(other_version)) == d::VkBundleRead::BadVersion);
    const d::VkLossBundleHeader header_only{d::kVkLossBundleMagic, d::kVkLossBundleVersion,
                                            static_cast<crd::u32>(sizeof(d::VkLossRecord)), 0U};
    CHECK(read_foreign(dir, &header_only, sizeof(header_only)) == d::VkBundleRead::BadSize);

    second.reset();
    crd::crash::uninstall();
    fs::remove_all(dir, ec);
#endif
}
