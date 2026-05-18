#include <crd/rhi/rhi.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
class FakeImage final : public crd::rhi::Image
{
public:
    explicit FakeImage(crd::rhi::ImageDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::ImageDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::ImageDesc m_desc{};
};

class FakeBuffer final : public crd::rhi::Buffer
{
public:
    explicit FakeBuffer(crd::rhi::BufferDesc desc) : m_desc(desc)
    {
        m_storage.resize(static_cast<crd::usize>(desc.size_bytes));
    }
    [[nodiscard]] const crd::rhi::BufferDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] void* map() noexcept override { return m_storage.data(); }
    void unmap() noexcept override {}

private:
    crd::rhi::BufferDesc m_desc{};
    crd::containers::Array<crd::u8> m_storage{};
};

class FakeShaderModule final : public crd::rhi::ShaderModule
{
public:
    explicit FakeShaderModule(crd::rhi::ShaderModuleDesc desc) : m_stage(desc.stage), m_entry_point(desc.entry_point) {}
    [[nodiscard]] crd::rhi::ShaderStage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::StringView entry_point() const noexcept override { return m_entry_point; }

private:
    crd::rhi::ShaderStage m_stage = crd::rhi::ShaderStage::Vertex;
    crd::containers::String m_entry_point{};
};

class FakePipeline final : public crd::rhi::Pipeline
{
public:
    explicit FakePipeline(crd::rhi::GraphicsPipelineDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::GraphicsPipelineDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::GraphicsPipelineDesc m_desc{};
};

// Phase 3.1.7.6 v0a — FakeComputePipeline mirrors FakePipeline.
class FakeComputePipeline final : public crd::rhi::ComputePipeline
{
public:
    explicit FakeComputePipeline(crd::rhi::ComputePipelineDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::ComputePipelineDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::ComputePipelineDesc m_desc{};
};

class FakeDescriptorSetLayout final : public crd::rhi::DescriptorSetLayout
{
public:
    explicit FakeDescriptorSetLayout(crd::rhi::DescriptorSetLayoutDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::DescriptorSetLayoutDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::DescriptorSetLayoutDesc m_desc{};
};

class FakePipelineLayout final : public crd::rhi::PipelineLayout
{
public:
    explicit FakePipelineLayout(crd::rhi::PipelineLayoutDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::PipelineLayoutDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::PipelineLayoutDesc m_desc{};
};

class FakeDescriptorSet final : public crd::rhi::DescriptorSet
{
public:
    void update_buffer(crd::u32 binding, crd::rhi::Buffer& /*buffer*/,
                       crd::u64 /*offset_bytes*/, crd::u64 /*size_bytes*/) override
    {
        ++update_buffer_count;
        last_binding = binding;
    }

    int update_buffer_count = 0;
    crd::u32 last_binding = 0;
};

class FakeDescriptorAllocator final : public crd::rhi::DescriptorAllocator
{
public:
    void begin_frame(crd::u32 frame_index) override
    {
        ++begin_frame_count;
        last_frame_index = frame_index;
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorSet>
    allocate(const crd::rhi::DescriptorSetLayout& layout) override
    {
        ++allocate_count;
        last_allocated_binding_count = static_cast<int>(layout.desc().bindings.size());
        return std::make_unique<FakeDescriptorSet>();
    }

    int begin_frame_count = 0;
    int allocate_count    = 0;
    crd::u32 last_frame_index = 0;
    int last_allocated_binding_count = 0;
};

class FakeCommandBuffer final : public crd::rhi::CommandBuffer
{
public:
    void begin() override { ++begin_count; }
    void end() override { ++end_count; }
    void reset() override { ++reset_count; }
    void begin_rendering(const crd::rhi::RenderingInfo& info) override
    {
        ++begin_rendering_count;
        last_extent = info.extent;
    }
    void end_rendering() override { ++end_rendering_count; }
    void bind_pipeline(crd::rhi::Pipeline& /*pipeline*/) override { ++bind_pipeline_count; }
    void bind_vertex_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override
    {
        ++bind_vertex_buffer_count;
    }
    void bind_index_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/,
                           crd::rhi::IndexType type) override
    {
        ++bind_index_buffer_count;
        last_index_type = type;
    }
    void draw(crd::u32 vertex_count, crd::u32 first_vertex) override
    {
        ++draw_count;
        last_vertex_count = vertex_count;
        last_first_vertex = first_vertex;
    }
    void draw_instanced(crd::u32, crd::u32, crd::u32, crd::u32) override {}
    void draw_indexed(crd::u32 index_count, crd::u32 first_index, crd::i32 vertex_offset) override
    {
        ++draw_indexed_count;
        last_index_count   = index_count;
        last_first_index   = first_index;
        last_vertex_offset = vertex_offset;
    }
    void copy_buffer(crd::rhi::Buffer& /*src*/, crd::rhi::Buffer& /*dst*/,
                     crd::u64 /*src_off*/, crd::u64 /*dst_off*/, crd::u64 /*size*/) override {}
    void copy_buffer_to_image(crd::rhi::Buffer& /*src*/, crd::rhi::Image& /*dst*/,
                              crd::containers::ConstSpan<crd::rhi::BufferImageCopy> /*regions*/) override {}
    void blit_image(crd::rhi::Image& /*src*/, crd::rhi::Image& /*dst*/,
                    crd::rhi::Extent2D /*src_extent*/, crd::rhi::Extent2D /*dst_extent*/) noexcept override
    {
        ++blit_image_count;
    }
    void transition_image(crd::rhi::Image& /*image*/, crd::rhi::ImageAccess /*from*/,
                          crd::rhi::ImageAccess /*to*/) noexcept override
    {
        ++transition_count;
    }
    void push_constants(crd::rhi::PipelineLayout& /*layout*/, crd::rhi::ShaderStage stages,
                        crd::u32 offset, crd::u32 size, const void* /*data*/) override
    {
        ++push_constants_count;
        last_push_stages = stages;
        last_push_offset = offset;
        last_push_size   = size;
    }
    void bind_descriptor_sets(crd::rhi::PipelineLayout& /*layout*/, crd::u32 first_set,
                              crd::containers::ConstSpan<crd::rhi::DescriptorSet*> sets) override
    {
        ++bind_descriptor_sets_count;
        last_first_set     = first_set;
        last_set_count     = static_cast<int>(sets.size());
    }

    // Phase 3.1.7.6 v0b — compute dispatch surface overrides.
    void bind_compute_pipeline(crd::rhi::ComputePipeline& /*pipeline*/) override
    {
        ++bind_compute_pipeline_count;
    }
    void bind_compute_descriptor_sets(crd::rhi::PipelineLayout& /*layout*/, crd::u32 first_set,
                                      crd::containers::ConstSpan<crd::rhi::DescriptorSet*> sets) override
    {
        ++bind_compute_descriptor_sets_count;
        last_compute_first_set = first_set;
        last_compute_set_count = static_cast<int>(sets.size());
    }
    void dispatch(crd::u32 x, crd::u32 y, crd::u32 z) override
    {
        ++dispatch_count;
        last_dispatch_x = x;
        last_dispatch_y = y;
        last_dispatch_z = z;
    }
    void dispatch_indirect(crd::rhi::Buffer& /*buffer*/, crd::u64 offset_bytes) override
    {
        ++dispatch_indirect_count;
        last_dispatch_indirect_offset = offset_bytes;
    }
    // Phase 3.1.7.6 v0c — buffer barrier recording.
    void buffer_barrier(crd::rhi::Buffer& /*buffer*/, crd::rhi::BufferAccess from,
                        crd::rhi::BufferAccess to) noexcept override
    {
        ++buffer_barrier_count;
        last_barrier_from = from;
        last_barrier_to   = to;
    }

    int begin_count = 0;
    int end_count = 0;
    int reset_count = 0;
    int begin_rendering_count = 0;
    int end_rendering_count = 0;
    int bind_pipeline_count = 0;
    int bind_vertex_buffer_count = 0;
    int bind_index_buffer_count  = 0;
    int draw_count = 0;
    int draw_indexed_count = 0;
    int blit_image_count = 0;
    int transition_count = 0;
    int push_constants_count = 0;
    int bind_descriptor_sets_count = 0;
    int bind_compute_pipeline_count = 0;
    int bind_compute_descriptor_sets_count = 0;
    int dispatch_count = 0;
    int dispatch_indirect_count = 0;
    int buffer_barrier_count = 0;
    crd::u32 last_compute_first_set = 0;
    int last_compute_set_count = 0;
    crd::u32 last_dispatch_x = 0;
    crd::u32 last_dispatch_y = 0;
    crd::u32 last_dispatch_z = 0;
    crd::u64 last_dispatch_indirect_offset = 0;
    crd::rhi::BufferAccess last_barrier_from = crd::rhi::BufferAccess::None;
    crd::rhi::BufferAccess last_barrier_to   = crd::rhi::BufferAccess::None;
    crd::rhi::Extent2D last_extent{};
    crd::u32 last_vertex_count = 0;
    crd::u32 last_first_vertex = 0;
    crd::u32 last_index_count  = 0;
    crd::u32 last_first_index  = 0;
    crd::i32 last_vertex_offset = 0;
    crd::rhi::IndexType last_index_type = crd::rhi::IndexType::Uint32;
    crd::rhi::ShaderStage last_push_stages = crd::rhi::ShaderStage::Vertex;
    crd::u32 last_push_offset = 0;
    crd::u32 last_push_size   = 0;
    crd::u32 last_first_set   = 0;
    int last_set_count        = 0;

    void set_viewport(crd::rhi::Extent2D /*extent*/) noexcept override {}
    void set_scissor(crd::rhi::Rect2D /*rect*/) noexcept override {}
};

class FakeSwapchain final : public crd::rhi::Swapchain
{
public:
    explicit FakeSwapchain(crd::rhi::SwapchainDesc desc)
        : m_desc(desc), m_image(crd::rhi::ImageDesc{desc.extent, desc.color_format,
                                                    crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment), 1, 1})
    {
    }

    [[nodiscard]] const crd::rhi::SwapchainDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] bool acquire_next_image() override
    {
        ++acquire_count;
        return true;
    }
    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return 0; }
    [[nodiscard]] crd::rhi::Image& current_image() noexcept override { return m_image; }
    void resize(crd::rhi::Extent2D new_extent) noexcept override { m_desc.extent = new_extent; }

    int acquire_count = 0;

private:
    crd::rhi::SwapchainDesc m_desc{};
    FakeImage m_image;
};

// Phase 3.0 v1o1 — fake fence for the non-blocking submit path.
//
// The fake auto-signals the moment Queue::submit(cmd, fence) is called.
// Real Vulkan signalling is asynchronous (GPU completes work, fence flips);
// the fake doesn't model that timing — it just records the dispatch. The
// Vulkan-backend test in tests/rhi_vulkan exercises the real GPU path.
class FakeFence final : public crd::rhi::Fence
{
public:
    [[nodiscard]] bool is_signaled() const noexcept override { return signaled; }
    void               wait()                       override { ++wait_count;  signaled = true; }
    void               reset()                      override { ++reset_count; signaled = false; }

    bool signaled    = false;
    int  wait_count  = 0;
    int  reset_count = 0;
};

// Phase 3.1.7.6 v0d — fake binary semaphore. No state to track.
class FakeSemaphore final : public crd::rhi::Semaphore
{
};

class FakeQueue final : public crd::rhi::Queue
{
public:
    [[nodiscard]] bool submit(crd::rhi::CommandBuffer& /*command_buffer*/, crd::rhi::Swapchain& /*swapchain*/) override
    {
        ++submit_count;
        return true;
    }
    void submit_and_wait(crd::rhi::CommandBuffer& /*command_buffer*/) override { ++submit_count; }
    void submit(crd::rhi::CommandBuffer& /*command_buffer*/, crd::rhi::Fence& fence) override
    {
        ++submit_with_fence_count;
        // Fake: pretend the GPU finished instantly and signal the fence.
        // Real Vulkan does this asynchronously when the command buffer
        // completes on the GPU; only the dispatch is observable here.
        if (auto* f = dynamic_cast<FakeFence*>(&fence); f != nullptr)
        {
            f->signaled = true;
        }
    }
    // Phase 3.1.7.6 v0d — full submit shape with wait/signal semaphores.
    void submit(const crd::rhi::SubmitInfo& info) override
    {
        ++submit_with_info_count;
        last_wait_sem_count   = static_cast<int>(info.wait_semaphores.size());
        last_signal_sem_count = static_cast<int>(info.signal_semaphores.size());
        if (info.signal_fence != nullptr)
        {
            if (auto* f = dynamic_cast<FakeFence*>(info.signal_fence); f != nullptr)
            {
                f->signaled = true;
            }
        }
    }
    void present(crd::rhi::Swapchain& /*swapchain*/) override { ++present_count; }
    void wait_idle() override { ++wait_idle_count; }

    int submit_count = 0;
    int submit_with_fence_count = 0;
    int submit_with_info_count  = 0;
    int last_wait_sem_count     = 0;
    int last_signal_sem_count   = 0;
    int present_count = 0;
    int wait_idle_count = 0;
};

class FakeDevice final : public crd::rhi::Device
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }

    [[nodiscard]] std::unique_ptr<crd::rhi::Swapchain> create_swapchain(const crd::rhi::SwapchainDesc& desc) override
    {
        ++create_swapchain_count;
        return std::make_unique<FakeSwapchain>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Buffer> create_buffer(const crd::rhi::BufferDesc& desc) override
    {
        ++create_buffer_count;
        return std::make_unique<FakeBuffer>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Image> create_image(const crd::rhi::ImageDesc& desc) override
    {
        ++create_image_count;
        return std::make_unique<FakeImage>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::ShaderModule>
    create_shader_module(const crd::rhi::ShaderModuleDesc& desc) override
    {
        ++create_shader_module_count;
        return std::make_unique<FakeShaderModule>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Pipeline>
    create_graphics_pipeline(const crd::rhi::GraphicsPipelineDesc& desc) override
    {
        ++create_pipeline_count;
        return std::make_unique<FakePipeline>(desc);
    }

    // Phase 3.1.7.6 v0a (ADR-0080) — compute pipeline factory override.
    [[nodiscard]] std::unique_ptr<crd::rhi::ComputePipeline>
    create_compute_pipeline(const crd::rhi::ComputePipelineDesc& desc) override
    {
        ++create_compute_pipeline_count;
        return std::make_unique<FakeComputePipeline>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer> create_command_buffer() override
    {
        ++create_command_buffer_count;
        return std::make_unique<FakeCommandBuffer>();
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Fence> create_fence() override
    {
        ++create_fence_count;
        return std::make_unique<FakeFence>();
    }

    // Phase 3.1.7.6 v0d — semaphore + compute queue overrides.
    [[nodiscard]] std::unique_ptr<crd::rhi::Semaphore> create_semaphore() override
    {
        ++create_semaphore_count;
        return std::make_unique<FakeSemaphore>();
    }
    [[nodiscard]] crd::rhi::Queue& compute_queue() noexcept override { return m_queue; }
    [[nodiscard]] bool has_dedicated_compute_queue() const noexcept override { return false; }
    [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer>
    create_command_buffer_for_queue(crd::rhi::Queue&) override { return nullptr; }
    [[nodiscard]] bool supports_shader_int64() const noexcept override { return false; }

    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorSetLayout>
    create_descriptor_set_layout(const crd::rhi::DescriptorSetLayoutDesc& desc) override
    {
        ++create_descriptor_set_layout_count;
        return std::make_unique<FakeDescriptorSetLayout>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::PipelineLayout>
    create_pipeline_layout(const crd::rhi::PipelineLayoutDesc& desc) override
    {
        ++create_pipeline_layout_count;
        return std::make_unique<FakePipelineLayout>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::DescriptorAllocator>
    create_descriptor_allocator(const crd::rhi::DescriptorAllocatorDesc& /*desc*/) override
    {
        ++create_descriptor_allocator_count;
        return std::make_unique<FakeDescriptorAllocator>();
    }

    [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
    void wait_idle() override { ++wait_idle_count; }

    int create_swapchain_count = 0;
    int create_buffer_count = 0;
    int create_image_count = 0;
    int create_shader_module_count = 0;
    int create_pipeline_count = 0;
    int create_compute_pipeline_count = 0;
    int create_command_buffer_count = 0;
    int create_fence_count = 0;
    int create_semaphore_count = 0;
    int create_descriptor_set_layout_count = 0;
    int create_pipeline_layout_count = 0;
    int create_descriptor_allocator_count = 0;
    int wait_idle_count = 0;
    FakeQueue m_queue{};
};

class FakeInstance final : public crd::rhi::Instance
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }

    void enumerate_adapters(crd::containers::Array<crd::rhi::AdapterInfo>& out) const override
    {
        out.push_back(crd::rhi::AdapterInfo{crd::containers::String("Fake GPU"), crd::rhi::AdapterType::DiscreteGpu,
                                            8ULL * 1024ULL * 1024ULL * 1024ULL, true, true});
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Device> create_device(const crd::rhi::DeviceDesc& /*desc*/) override
    {
        return std::make_unique<FakeDevice>();
    }
};
} // namespace

TEST_CASE("RHI flags compose cleanly", "[rhi][types]")
{
    const crd::u32 usage = crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst;
    REQUIRE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::Vertex));
    REQUIRE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::TransferDst));
    REQUIRE_FALSE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::Uniform));
}

TEST_CASE("ShaderStage is a composable bitmask", "[rhi][types]")
{
    using crd::rhi::ShaderStage;
    const auto vs_fs = ShaderStage::Vertex | ShaderStage::Fragment;
    REQUIRE(crd::rhi::has_stage(vs_fs, ShaderStage::Vertex));
    REQUIRE(crd::rhi::has_stage(vs_fs, ShaderStage::Fragment));
    REQUIRE_FALSE(crd::rhi::has_stage(vs_fs, ShaderStage::Compute));

    // Individual stages are single-bit
    REQUIRE(crd::rhi::has_stage(ShaderStage::Vertex, ShaderStage::Vertex));
    REQUIRE_FALSE(crd::rhi::has_stage(ShaderStage::Vertex, ShaderStage::Fragment));
}

TEST_CASE("RHI instance enumerates adapters and creates a device", "[rhi][instance]")
{
    FakeInstance instance;
    crd::containers::Array<crd::rhi::AdapterInfo> adapters;
    instance.enumerate_adapters(adapters);
    REQUIRE(adapters.size() == 1U);
    REQUIRE(adapters[0].name == "Fake GPU");

    auto device = instance.create_device({});
    REQUIRE(device != nullptr);
    REQUIRE(device->api() == crd::rhi::BackendApi::Vulkan);
}

TEST_CASE("DescriptorSetLayout creation from bindings", "[rhi][descriptor]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
        {1, crd::rhi::DescriptorType::CombinedImageSampler, 4,
         crd::rhi::ShaderStage::Fragment | crd::rhi::ShaderStage::Vertex},
    };
    auto layout = device.create_descriptor_set_layout({crd::containers::make_span(bindings)});
    REQUIRE(layout != nullptr);
    REQUIRE(layout->desc().bindings.size() == 2U);
    REQUIRE(layout->desc().bindings[0].binding == 0U);
    REQUIRE(layout->desc().bindings[1].count == 4U);
    REQUIRE(device.create_descriptor_set_layout_count == 1);
}

TEST_CASE("PipelineLayout creation with push constants and set layouts", "[rhi][descriptor]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding per_frame_bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment},
    };
    auto per_frame_layout = device.create_descriptor_set_layout({crd::containers::make_span(per_frame_bindings)});

    crd::rhi::DescriptorBinding per_mat_bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
    };
    auto per_mat_layout = device.create_descriptor_set_layout({crd::containers::make_span(per_mat_bindings)});

    crd::rhi::PushConstantRange pc_range{crd::rhi::ShaderStage::Vertex, 0, 128};
    const crd::rhi::DescriptorSetLayout* set_layouts[] = {per_frame_layout.get(), per_mat_layout.get()};

    auto pipeline_layout = device.create_pipeline_layout({
        crd::containers::make_span(set_layouts),
        crd::containers::make_span(&pc_range, 1),
    });

    REQUIRE(pipeline_layout != nullptr);
    REQUIRE(pipeline_layout->desc().set_layouts.size() == 2U);
    REQUIRE(pipeline_layout->desc().push_constant_ranges.size() == 1U);
    REQUIRE(pipeline_layout->desc().push_constant_ranges[0].size == 128U);
    REQUIRE(device.create_pipeline_layout_count == 1);
}

TEST_CASE("DescriptorAllocator ring-buffer lifecycle", "[rhi][descriptor]")
{
    FakeDevice device;

    crd::rhi::DescriptorBinding bindings[] = {
        {0, crd::rhi::DescriptorType::UniformBuffer, 1, crd::rhi::ShaderStage::Fragment},
    };
    auto set_layout = device.create_descriptor_set_layout({crd::containers::make_span(bindings)});
    auto allocator  = device.create_descriptor_allocator({2, 64});
    auto* fake_alloc = static_cast<FakeDescriptorAllocator*>(allocator.get());

    // Frame 0: begin + allocate
    allocator->begin_frame(0);
    REQUIRE(fake_alloc->begin_frame_count == 1);
    REQUIRE(fake_alloc->last_frame_index == 0U);

    auto set0 = allocator->allocate(*set_layout);
    REQUIRE(set0 != nullptr);
    REQUIRE(fake_alloc->allocate_count == 1);
    REQUIRE(fake_alloc->last_allocated_binding_count == 1);

    // Frame 1: begin (resets pool[1]) + allocate
    allocator->begin_frame(1);
    REQUIRE(fake_alloc->begin_frame_count == 2);
    auto set1 = allocator->allocate(*set_layout);
    REQUIRE(set1 != nullptr);
    REQUIRE(fake_alloc->allocate_count == 2);
}

TEST_CASE("DescriptorSet update_buffer is recorded", "[rhi][descriptor]")
{
    FakeDevice device;
    auto buf = device.create_buffer({256, crd::rhi::enum_bits(crd::rhi::BufferUsage::Uniform), crd::rhi::MemoryUsage::CpuToGpu});

    FakeDescriptorSet set;
    set.update_buffer(2, *buf, 0, 0);

    REQUIRE(set.update_buffer_count == 1);
    REQUIRE(set.last_binding == 2U);
}

TEST_CASE("CommandBuffer push_constants and bind_descriptor_sets are recorded", "[rhi][descriptor]")
{
    FakeDevice device;
    auto cmd     = device.create_command_buffer();
    auto* fake   = static_cast<FakeCommandBuffer*>(cmd.get());

    FakePipelineLayout layout({});
    FakeDescriptorSet  set0;
    FakeDescriptorSet  set1;

    struct PushData { float mvp[16]; } push{};
    crd::rhi::DescriptorSet* sets[] = {&set0, &set1};

    cmd->push_constants(layout, crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment,
                        0, sizeof(push), &push);
    cmd->bind_descriptor_sets(layout, 0, crd::containers::make_span(sets));

    REQUIRE(fake->push_constants_count == 1);
    REQUIRE(fake->last_push_offset == 0U);
    REQUIRE(fake->last_push_size   == sizeof(push));
    REQUIRE(crd::rhi::has_stage(fake->last_push_stages, crd::rhi::ShaderStage::Vertex));
    REQUIRE(crd::rhi::has_stage(fake->last_push_stages, crd::rhi::ShaderStage::Fragment));

    REQUIRE(fake->bind_descriptor_sets_count == 1);
    REQUIRE(fake->last_first_set   == 0U);
    REQUIRE(fake->last_set_count   == 2);
}

TEST_CASE("RHI device can express the first-triangle resource flow", "[rhi][device]")
{
    FakeDevice device;

    crd::u8 shader_code[] = {0x03, 0x02, 0x23, 0x07};
    auto vs =
        device.create_shader_module({crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(shader_code)});
    auto fs =
        device.create_shader_module({crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(shader_code)});
    auto vb = device.create_buffer(
        {sizeof(float) * 18U, static_cast<crd::u32>(crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::CpuToGpu});

    crd::rhi::VertexBindingDesc binding{0, sizeof(float) * 6U, crd::rhi::VertexInputRate::Vertex};
    crd::rhi::VertexAttributeDesc attrs[] = {{0, 0, crd::rhi::Format::R8G8B8A8Unorm, 0},
                                             {1, 0, crd::rhi::Format::R8G8B8A8Unorm, 16}};
    auto pipeline = device.create_graphics_pipeline({vs.get(),
                                                     fs.get(),
                                                     crd::rhi::PrimitiveTopology::TriangleList,
                                                     {1280, 720},
                                                     crd::rhi::Format::B8G8R8A8Unorm,
                                                     crd::rhi::Format::Undefined,
                                                     crd::containers::make_span(&binding, 1),
                                                     crd::containers::make_span(attrs),
                                                     false,
                                                     false});

    auto command_buffer = device.create_command_buffer();
    auto* cb = static_cast<FakeCommandBuffer*>(command_buffer.get());
    auto swapchain = device.create_swapchain(
        {nullptr, {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);
    REQUIRE(swapchain->acquire_next_image());

    command_buffer->begin();
    command_buffer->begin_rendering(
        {{1280, 720}, {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.1F, 0.2F, 0.3F, 1.0F}}, nullptr});
    command_buffer->bind_pipeline(*pipeline);
    command_buffer->bind_vertex_buffer(*vb, 0);
    command_buffer->draw(3, 0);
    command_buffer->end_rendering();
    command_buffer->end();

    REQUIRE(device.graphics_queue().submit(*command_buffer, *swapchain));
    device.graphics_queue().present(*swapchain);

    REQUIRE(device.create_shader_module_count == 2);
    REQUIRE(device.create_buffer_count == 1);
    REQUIRE(device.create_pipeline_count == 1);
    REQUIRE(device.create_command_buffer_count == 1);
    REQUIRE(cb->begin_count == 1);
    REQUIRE(cb->begin_rendering_count == 1);
    REQUIRE(cb->bind_pipeline_count == 1);
    REQUIRE(cb->bind_vertex_buffer_count == 1);
    REQUIRE(cb->draw_count == 1);
    REQUIRE(cb->last_vertex_count == 3U);
    REQUIRE(device.m_queue.submit_count == 1);
    REQUIRE(device.m_queue.present_count == 1);
}

// ─── Phase 3.0 v1o1 — RHI Fence + non-blocking submit (ADR-0061 §"Layer 1") ───

TEST_CASE("Fence: factory creates an unsignalled fence", "[rhi][fence]")
{
    FakeDevice device{};
    auto fence = device.create_fence();
    REQUIRE(fence != nullptr);
    CHECK(device.create_fence_count == 1);
    // ADR-0061 contract: a freshly-created fence is unsignalled until a
    // submission completes against it.
    CHECK_FALSE(fence->is_signaled());
}

TEST_CASE("Fence: wait() transitions to signalled state", "[rhi][fence]")
{
    FakeDevice device{};
    auto fence = device.create_fence();
    REQUIRE(fence != nullptr);

    auto* fake = static_cast<FakeFence*>(fence.get());
    REQUIRE(fake->wait_count == 0);
    fence->wait();
    CHECK(fake->wait_count == 1);
    CHECK(fence->is_signaled());
}

TEST_CASE("Fence: reset() re-arms the fence", "[rhi][fence]")
{
    FakeDevice device{};
    auto fence = device.create_fence();
    REQUIRE(fence != nullptr);

    auto* fake = static_cast<FakeFence*>(fence.get());
    fence->wait();
    REQUIRE(fence->is_signaled());

    (*fence).reset();
    CHECK(fake->reset_count == 1);
    CHECK_FALSE(fence->is_signaled());

    // Multiple reset cycles must be safe.
    fence->wait();
    (*fence).reset();
    (*fence).reset();
    CHECK(fake->reset_count == 3);
    CHECK_FALSE(fence->is_signaled());
}

TEST_CASE("Queue::submit(cmd, fence) signals the fence on completion",
          "[rhi][fence][queue]")
{
    FakeDevice device{};
    auto cmd   = device.create_command_buffer();
    auto fence = device.create_fence();
    REQUIRE(cmd != nullptr);
    REQUIRE(fence != nullptr);

    // Pre-submit: the fence is unsignalled and the queue has no fence-submits.
    CHECK_FALSE(fence->is_signaled());
    CHECK(device.m_queue.submit_with_fence_count == 0);

    device.graphics_queue().submit(*cmd, *fence);

    CHECK(device.m_queue.submit_with_fence_count == 1);
    // FakeQueue auto-signals (real Vulkan signals once the GPU completes the
    // command-buffer's work; the fake doesn't model timing).
    CHECK(fence->is_signaled());

    // Reset + re-submit → fence flips back to unsignalled, then signalled again.
    (*fence).reset();
    CHECK_FALSE(fence->is_signaled());
    device.graphics_queue().submit(*cmd, *fence);
    CHECK(device.m_queue.submit_with_fence_count == 2);
    CHECK(fence->is_signaled());
}

// =====================================================================
// Phase 3.1.7.6 v0a — ComputePipeline type-system contract tests
// (ADR-0080 D1 additive-only + D2 revision: storage buffers reuse
// existing Buffer interface, no separate IStorageBuffer.)
// =====================================================================

TEST_CASE("ComputePipelineDesc has narrow, compute-only surface", "[rhi][compute][v0a]")
{
    // The desc carries ONLY a compute shader + a pipeline layout. No
    // vertex input, no viewport, no raster — none of those have meaning
    // for compute. This test pins the surface against accidental drift.
    crd::rhi::ComputePipelineDesc desc{};
    REQUIRE(desc.compute_shader == nullptr);
    REQUIRE(desc.pipeline_layout == nullptr);
    // Spec-const fields (v0b) are empty by default. The struct stays narrow:
    // no vertex input / no viewport / no raster — those don't apply to compute.
    REQUIRE(desc.specialization_entries.empty());
    REQUIRE(desc.specialization_data.empty());
}

TEST_CASE("Device::create_compute_pipeline factory contract", "[rhi][compute][v0a]")
{
    FakeDevice device{};
    REQUIRE(device.create_compute_pipeline_count == 0);

    auto shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", {}});
    REQUIRE(shader != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader = shader.get();
    auto pipeline = device.create_compute_pipeline(desc);

    REQUIRE(pipeline != nullptr);
    REQUIRE(device.create_compute_pipeline_count == 1);
    CHECK(pipeline->desc().compute_shader == shader.get());
}

TEST_CASE("ComputePipeline lifecycle: multi-create then destroy clean", "[rhi][compute][v0a]")
{
    // ASan-clean teardown across many short-lived compute pipelines.
    // The real test runs against the Vulkan backend (test_rhi_vulkan.cpp);
    // this fake-side variant pins the factory ABI contract.
    FakeDevice device{};
    constexpr int kCycles = 8;
    for (int i = 0; i < kCycles; ++i)
    {
        auto shader = device.create_shader_module(
            {crd::rhi::ShaderStage::Compute, "main", {}});
        crd::rhi::ComputePipelineDesc desc{};
        desc.compute_shader = shader.get();
        auto pipeline = device.create_compute_pipeline(desc);
        REQUIRE(pipeline != nullptr);
    } // RAII: pipeline + shader destroyed each iteration
    REQUIRE(device.create_compute_pipeline_count == kCycles);
}

TEST_CASE("ComputePipeline + PipelineLayout caller-side composition", "[rhi][compute][v0a]")
{
    // Validates the ADR-0080 D7 revision: pipeline layouts are caller-
    // constructed, the RHI does not enforce a set-0/set-1 convention.
    // Compute consumers compose whatever layout they need.
    FakeDevice device{};

    crd::rhi::DescriptorBinding bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::UniformBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc set0_desc{};
    set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, 2);
    auto set0 = device.create_descriptor_set_layout(set0_desc);
    REQUIRE(set0 != nullptr);

    const crd::rhi::DescriptorSetLayout* layouts[] = {set0.get()};
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    auto layout = device.create_pipeline_layout(layout_desc);
    REQUIRE(layout != nullptr);

    auto shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", {}});
    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader = shader.get();
    desc.pipeline_layout = layout.get();
    auto pipeline = device.create_compute_pipeline(desc);

    REQUIRE(pipeline != nullptr);
    CHECK(pipeline->desc().pipeline_layout == layout.get());
}

TEST_CASE("ComputePipelineDesc layout is trivially copyable", "[rhi][compute][v0a]")
{
    STATIC_REQUIRE(std::is_trivially_copyable_v<crd::rhi::ComputePipelineDesc>);
    STATIC_REQUIRE(std::is_standard_layout_v<crd::rhi::ComputePipelineDesc>);
}

// =====================================================================
// Phase 3.1.7.6 v0b — CommandBuffer compute dispatch surface contract
// (ADR-0080 D4 dispatch params = workgroup counts; D5 push-const reuses
// graphics path; D6 spec const baked at create-time.)
// =====================================================================

TEST_CASE("bind_compute_pipeline + bind_compute_descriptor_sets dispatch through fake recorder",
          "[rhi][compute][v0b]")
{
    FakeDevice device{};
    auto cmd_ptr = device.create_command_buffer();
    auto* cmd_raw = dynamic_cast<FakeCommandBuffer*>(cmd_ptr.get());
    REQUIRE(cmd_raw != nullptr);

    auto layout = device.create_pipeline_layout({});
    auto shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", {}});
    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader = shader.get();
    auto pipeline = device.create_compute_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    cmd_ptr->bind_compute_pipeline(*pipeline);
    REQUIRE(cmd_raw->bind_compute_pipeline_count == 1);
    REQUIRE(cmd_raw->bind_pipeline_count == 0); // graphics path NOT touched

    crd::rhi::DescriptorSet* sets[] = {nullptr};
    cmd_ptr->bind_compute_descriptor_sets(
        *layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    REQUIRE(cmd_raw->bind_compute_descriptor_sets_count == 1);
    REQUIRE(cmd_raw->bind_descriptor_sets_count == 0); // graphics path NOT touched
    REQUIRE(cmd_raw->last_compute_first_set == 0);
    REQUIRE(cmd_raw->last_compute_set_count == 1);
}

TEST_CASE("dispatch params are workgroup counts (D4)", "[rhi][compute][v0b]")
{
    FakeDevice device{};
    auto cmd_ptr = device.create_command_buffer();
    auto* cmd_raw = dynamic_cast<FakeCommandBuffer*>(cmd_ptr.get());
    REQUIRE(cmd_raw != nullptr);

    cmd_ptr->dispatch(16, 8, 4);
    REQUIRE(cmd_raw->dispatch_count == 1);
    REQUIRE(cmd_raw->last_dispatch_x == 16);
    REQUIRE(cmd_raw->last_dispatch_y == 8);
    REQUIRE(cmd_raw->last_dispatch_z == 4);
}

TEST_CASE("dispatch_indirect threads buffer + offset", "[rhi][compute][v0b]")
{
    FakeDevice device{};
    auto cmd_ptr = device.create_command_buffer();
    auto* cmd_raw = dynamic_cast<FakeCommandBuffer*>(cmd_ptr.get());
    REQUIRE(cmd_raw != nullptr);

    auto buf = device.create_buffer(
        {12, crd::rhi::enum_bits(crd::rhi::BufferUsage::Indirect), crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buf != nullptr);
    cmd_ptr->dispatch_indirect(*buf, 8);
    REQUIRE(cmd_raw->dispatch_indirect_count == 1);
    REQUIRE(cmd_raw->last_dispatch_indirect_offset == 8);
}

TEST_CASE("SpecializationConstantEntry mirrors VkSpecializationMapEntry shape (D6)", "[rhi][compute][v0b]")
{
    STATIC_REQUIRE(sizeof(crd::rhi::SpecializationConstantEntry) == 12); // 3 × u32
    STATIC_REQUIRE(std::is_trivially_copyable_v<crd::rhi::SpecializationConstantEntry>);
    STATIC_REQUIRE(std::is_standard_layout_v<crd::rhi::SpecializationConstantEntry>);
}

TEST_CASE("ComputePipelineDesc accepts specialization constants (D6 type plumbing)", "[rhi][compute][v0b]")
{
    FakeDevice device{};
    auto shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", {}});

    const crd::u32 spec_value = 1000;
    crd::rhi::SpecializationConstantEntry entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader = shader.get();
    desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&entry, 1);
    desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&spec_value), sizeof(spec_value));

    auto pipeline = device.create_compute_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    REQUIRE(pipeline->desc().specialization_entries.size() == 1);
    REQUIRE(pipeline->desc().specialization_data.size() == sizeof(crd::u32));
}

TEST_CASE("push_constants reuse for compute via ShaderStage::Compute mask (D5)", "[rhi][compute][v0b]")
{
    FakeDevice device{};
    auto cmd_ptr = device.create_command_buffer();
    auto* cmd_raw = dynamic_cast<FakeCommandBuffer*>(cmd_ptr.get());
    REQUIRE(cmd_raw != nullptr);

    auto layout = device.create_pipeline_layout({});
    const crd::u32 pc_value = 42;
    cmd_ptr->push_constants(*layout, crd::rhi::ShaderStage::Compute, 0, sizeof(pc_value), &pc_value);

    REQUIRE(cmd_raw->push_constants_count == 1);
    REQUIRE(crd::rhi::has_stage(cmd_raw->last_push_stages, crd::rhi::ShaderStage::Compute));
    REQUIRE(cmd_raw->last_push_size == sizeof(pc_value));
}

// =====================================================================
// Phase 3.1.7.6 v0c — CommandBuffer::buffer_barrier contract
// (ADR-0080 D8 backend-agnostic typed-enum barrier API; same-queue path;
// span-batching deferred per consumer-driven scoping.)
// =====================================================================

TEST_CASE("buffer_barrier threads from->to BufferAccess pair", "[rhi][compute][v0c]")
{
    FakeDevice device{};
    auto cmd_ptr = device.create_command_buffer();
    auto* cmd_raw = dynamic_cast<FakeCommandBuffer*>(cmd_ptr.get());
    REQUIRE(cmd_raw != nullptr);

    auto buf = device.create_buffer(
        {256, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage), crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buf != nullptr);

    cmd_ptr->buffer_barrier(*buf, crd::rhi::BufferAccess::ComputeShaderWrite,
                            crd::rhi::BufferAccess::ComputeShaderRead);
    REQUIRE(cmd_raw->buffer_barrier_count == 1);
    REQUIRE(cmd_raw->last_barrier_from == crd::rhi::BufferAccess::ComputeShaderWrite);
    REQUIRE(cmd_raw->last_barrier_to   == crd::rhi::BufferAccess::ComputeShaderRead);
}

TEST_CASE("BufferAccess enum has granular per-stage variants (advisor guidance)", "[rhi][compute][v0c]")
{
    // The enum DELIBERATELY enumerates compute/vertex/fragment shader
    // reads as separate variants instead of collapsing into a single
    // "GraphicsRead" — that lets the impl pick exact srcStageMask without
    // over-barrier. Pin the granularity here so it doesn't drift.
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::BufferAccess::ComputeShaderRead) !=
                   static_cast<crd::u32>(crd::rhi::BufferAccess::VertexShaderRead));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::BufferAccess::ComputeShaderRead) !=
                   static_cast<crd::u32>(crd::rhi::BufferAccess::FragmentShaderRead));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::BufferAccess::VertexAttributeRead) !=
                   static_cast<crd::u32>(crd::rhi::BufferAccess::UniformRead));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::BufferAccess::IndirectRead) !=
                   static_cast<crd::u32>(crd::rhi::BufferAccess::TransferSrc));
}

TEST_CASE("ImageAccess gained compute variants without breaking back-compat", "[rhi][compute][v0c]")
{
    // ShaderRead stays valued the same (graphics fragment-shader-read);
    // new ComputeShader* variants are additions. No existing transition_image
    // consumer changes behavior.
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::ImageAccess::ShaderRead) !=
                   static_cast<crd::u32>(crd::rhi::ImageAccess::ComputeShaderRead));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::ImageAccess::ComputeShaderWrite) !=
                   static_cast<crd::u32>(crd::rhi::ImageAccess::ComputeShaderReadWrite));
}

// =====================================================================
// Phase 3.1.7.6 v0d — async compute substrate contract
// (ADR-0080 D9 FallbackGracefully default + D10 SubmitInfo / Semaphore API)
// =====================================================================

TEST_CASE("Device::create_semaphore returns a non-null binary semaphore", "[rhi][compute][v0d]")
{
    FakeDevice device{};
    REQUIRE(device.create_semaphore_count == 0);
    auto sem = device.create_semaphore();
    REQUIRE(sem != nullptr);
    REQUIRE(device.create_semaphore_count == 1);
}

TEST_CASE("Device::compute_queue() falls back to graphics_queue (pointer identity)", "[rhi][compute][v0d]")
{
    FakeDevice device{};
    REQUIRE_FALSE(device.has_dedicated_compute_queue());
    // ADR-0080 D9 pointer-identity contract: consumers may dispatch on
    // address equality to skip cross-queue setup when on fallback.
    REQUIRE(&device.compute_queue() == &device.graphics_queue());
}

TEST_CASE("Queue::submit(SubmitInfo) threads wait/signal semaphore counts", "[rhi][compute][v0d]")
{
    FakeDevice device{};
    auto cmd  = device.create_command_buffer();
    auto sem1 = device.create_semaphore();
    auto sem2 = device.create_semaphore();
    auto sem3 = device.create_semaphore();
    auto fence = device.create_fence();

    crd::rhi::SemaphoreWait waits[] = {
        {.semaphore = sem1.get(), .wait_stage = crd::rhi::PipelineStage::ComputeShader},
        {.semaphore = sem2.get(), .wait_stage = crd::rhi::PipelineStage::VertexInput},
    };
    crd::rhi::Semaphore* signals[] = {sem3.get()};

    crd::rhi::SubmitInfo info{};
    info.command_buffer = cmd.get();
    info.signal_fence   = fence.get();
    info.wait_semaphores =
        crd::containers::ConstSpan<crd::rhi::SemaphoreWait>(waits, 2);
    info.signal_semaphores =
        crd::containers::ConstSpan<crd::rhi::Semaphore*>(signals, 1);

    auto& q = dynamic_cast<FakeQueue&>(device.graphics_queue());
    q.submit(info);

    REQUIRE(q.submit_with_info_count == 1);
    REQUIRE(q.last_wait_sem_count    == 2);
    REQUIRE(q.last_signal_sem_count  == 1);
    REQUIRE(fence->is_signaled()); // Fake signals fence on dispatch
}

TEST_CASE("PipelineStage enum: distinct values for each Vulkan stage", "[rhi][compute][v0d]")
{
    // Granularity pin — same rationale as BufferAccess. If a refactor
    // collapses stages into a "GraphicsStage" bucket, the impl loses
    // ability to wait at exact stages and over-syncs.
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::PipelineStage::ComputeShader) !=
                   static_cast<crd::u32>(crd::rhi::PipelineStage::VertexShader));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::PipelineStage::VertexInput) !=
                   static_cast<crd::u32>(crd::rhi::PipelineStage::FragmentShader));
    STATIC_REQUIRE(static_cast<crd::u32>(crd::rhi::PipelineStage::ColorAttachment) !=
                   static_cast<crd::u32>(crd::rhi::PipelineStage::Transfer));
}

TEST_CASE("AsyncComputePolicy defaults to FallbackGracefully", "[rhi][compute][v0d]")
{
    // ADR-0080 D9 default. Consumers wanting hard hardware-dedication
    // requirement opt in via DeviceDesc::async_compute_policy =
    // RequireDedicated.
    crd::rhi::DeviceDesc desc{};
    REQUIRE(desc.async_compute_policy == crd::rhi::AsyncComputePolicy::FallbackGracefully);
}
