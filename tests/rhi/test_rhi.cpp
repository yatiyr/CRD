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
    void draw_indexed(crd::u32 index_count, crd::u32 first_index, crd::i32 vertex_offset) override
    {
        ++draw_indexed_count;
        last_index_count   = index_count;
        last_first_index   = first_index;
        last_vertex_offset = vertex_offset;
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
    int transition_count = 0;
    int push_constants_count = 0;
    int bind_descriptor_sets_count = 0;
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

    int acquire_count = 0;

private:
    crd::rhi::SwapchainDesc m_desc{};
    FakeImage m_image;
};

class FakeQueue final : public crd::rhi::Queue
{
public:
    [[nodiscard]] bool submit(crd::rhi::CommandBuffer& /*command_buffer*/, crd::rhi::Swapchain& /*swapchain*/) override
    {
        ++submit_count;
        return true;
    }
    void present(crd::rhi::Swapchain& /*swapchain*/) override { ++present_count; }
    void wait_idle() override { ++wait_idle_count; }

    int submit_count = 0;
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

    [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer> create_command_buffer() override
    {
        ++create_command_buffer_count;
        return std::make_unique<FakeCommandBuffer>();
    }

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
    int create_command_buffer_count = 0;
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
                                            8ull * 1024ull * 1024ull * 1024ull, true, true});
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
    REQUIRE(adapters.size() == 1u);
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
    REQUIRE(layout->desc().bindings.size() == 2u);
    REQUIRE(layout->desc().bindings[0].binding == 0u);
    REQUIRE(layout->desc().bindings[1].count == 4u);
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
    REQUIRE(pipeline_layout->desc().set_layouts.size() == 2u);
    REQUIRE(pipeline_layout->desc().push_constant_ranges.size() == 1u);
    REQUIRE(pipeline_layout->desc().push_constant_ranges[0].size == 128u);
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
    REQUIRE(fake_alloc->last_frame_index == 0u);

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
    REQUIRE(set.last_binding == 2u);
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
    REQUIRE(fake->last_push_offset == 0u);
    REQUIRE(fake->last_push_size   == sizeof(push));
    REQUIRE(crd::rhi::has_stage(fake->last_push_stages, crd::rhi::ShaderStage::Vertex));
    REQUIRE(crd::rhi::has_stage(fake->last_push_stages, crd::rhi::ShaderStage::Fragment));

    REQUIRE(fake->bind_descriptor_sets_count == 1);
    REQUIRE(fake->last_first_set   == 0u);
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
        {sizeof(float) * 18u, static_cast<crd::u32>(crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::CpuToGpu});

    crd::rhi::VertexBindingDesc binding{0, sizeof(float) * 6u, crd::rhi::VertexInputRate::Vertex};
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
        {{1280, 720}, {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.1f, 0.2f, 0.3f, 1.0f}}, nullptr});
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
    REQUIRE(cb->last_vertex_count == 3u);
    REQUIRE(device.m_queue.submit_count == 1);
    REQUIRE(device.m_queue.present_count == 1);
}
