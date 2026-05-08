#include <crd/app/event_dispatcher.hpp>
#include <crd/app/events/input_events.hpp>
#include <crd/imgui/imgui_layer.hpp>
#include <crd/imgui/log_channel.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/vulkan_native.hpp>

// ImGui backend headers contain C-style casts that trigger -Wconversion on GCC.
// Suppress for these headers only; the cast is intentional ImGui sentinel idiom.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace crd::imgui
{
namespace
{
[[nodiscard]] ImGuiKey to_imgui_key(crd::platform::Key key) noexcept
{
    switch (key)
    {
        case crd::platform::Key::Tab:
            return ImGuiKey_Tab;
        case crd::platform::Key::Left:
            return ImGuiKey_LeftArrow;
        case crd::platform::Key::Right:
            return ImGuiKey_RightArrow;
        case crd::platform::Key::Up:
            return ImGuiKey_UpArrow;
        case crd::platform::Key::Down:
            return ImGuiKey_DownArrow;
        case crd::platform::Key::Enter:
            return ImGuiKey_Enter;
        case crd::platform::Key::Escape:
            return ImGuiKey_Escape;
        case crd::platform::Key::Backspace:
            return ImGuiKey_Backspace;
        case crd::platform::Key::Delete:
            return ImGuiKey_Delete;
        case crd::platform::Key::Space:
            return ImGuiKey_Space;
        case crd::platform::Key::A:
            return ImGuiKey_A;
        case crd::platform::Key::B:
            return ImGuiKey_B;
        case crd::platform::Key::C:
            return ImGuiKey_C;
        case crd::platform::Key::D:
            return ImGuiKey_D;
        case crd::platform::Key::E:
            return ImGuiKey_E;
        case crd::platform::Key::F:
            return ImGuiKey_F;
        case crd::platform::Key::G:
            return ImGuiKey_G;
        case crd::platform::Key::H:
            return ImGuiKey_H;
        case crd::platform::Key::I:
            return ImGuiKey_I;
        case crd::platform::Key::J:
            return ImGuiKey_J;
        case crd::platform::Key::K:
            return ImGuiKey_K;
        case crd::platform::Key::L:
            return ImGuiKey_L;
        case crd::platform::Key::M:
            return ImGuiKey_M;
        case crd::platform::Key::N:
            return ImGuiKey_N;
        case crd::platform::Key::O:
            return ImGuiKey_O;
        case crd::platform::Key::P:
            return ImGuiKey_P;
        case crd::platform::Key::Q:
            return ImGuiKey_Q;
        case crd::platform::Key::R:
            return ImGuiKey_R;
        case crd::platform::Key::S:
            return ImGuiKey_S;
        case crd::platform::Key::T:
            return ImGuiKey_T;
        case crd::platform::Key::U:
            return ImGuiKey_U;
        case crd::platform::Key::V:
            return ImGuiKey_V;
        case crd::platform::Key::W:
            return ImGuiKey_W;
        case crd::platform::Key::X:
            return ImGuiKey_X;
        case crd::platform::Key::Y:
            return ImGuiKey_Y;
        case crd::platform::Key::Z:
            return ImGuiKey_Z;
        case crd::platform::Key::Num0:
            return ImGuiKey_0;
        case crd::platform::Key::Num1:
            return ImGuiKey_1;
        case crd::platform::Key::Num2:
            return ImGuiKey_2;
        case crd::platform::Key::Num3:
            return ImGuiKey_3;
        case crd::platform::Key::Num4:
            return ImGuiKey_4;
        case crd::platform::Key::Num5:
            return ImGuiKey_5;
        case crd::platform::Key::Num6:
            return ImGuiKey_6;
        case crd::platform::Key::Num7:
            return ImGuiKey_7;
        case crd::platform::Key::Num8:
            return ImGuiKey_8;
        case crd::platform::Key::Num9:
            return ImGuiKey_9;
        case crd::platform::Key::LeftCtrl:
            return ImGuiKey_LeftCtrl;
        case crd::platform::Key::RightCtrl:
            return ImGuiKey_RightCtrl;
        case crd::platform::Key::LeftShift:
            return ImGuiKey_LeftShift;
        case crd::platform::Key::RightShift:
            return ImGuiKey_RightShift;
        case crd::platform::Key::LeftAlt:
            return ImGuiKey_LeftAlt;
        case crd::platform::Key::RightAlt:
            return ImGuiKey_RightAlt;
        default:
            return ImGuiKey_None;
    }
}

[[nodiscard]] int to_imgui_mouse_button(crd::platform::MouseButton button) noexcept
{
    switch (button)
    {
        case crd::platform::MouseButton::Left:
            return 0;
        case crd::platform::MouseButton::Right:
            return 1;
        case crd::platform::MouseButton::Middle:
            return 2;
        case crd::platform::MouseButton::X1:
            return 3;
        case crd::platform::MouseButton::X2:
            return 4;
        default:
            return 0;
    }
}

void check_vk_result(VkResult result)
{
    if (result != VK_SUCCESS)
    {
        CRD_LOG_ERROR(g_log_imgui, "ImGui Vulkan backend reported VkResult={}", static_cast<int>(result));
    }
}
} // namespace

ImGuiLayer::ImGuiLayer(crd::app::Application& app, crd::rhi::Instance& instance, crd::rhi::Device& device,
                       crd::rhi::Swapchain& swapchain, const crd::config::Config& config)
    : Layer("ImGuiLayer"), m_app(app), m_instance(instance), m_device(device), m_swapchain(swapchain),
      m_settings(load_settings(config))
{
}

ImGuiLayer::~ImGuiLayer()
{
    on_detach();
}

void ImGuiLayer::on_attach()
{
    if (m_attached)
    {
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Pin imgui.ini next to the executable so the repo root stays clean.
    // ImGui requires the IniFilename pointer to outlive the context, so we own the storage.
    {
        const auto exe_dir = crd::platform::fs::executable_dir();
        if (!exe_dir.empty())
        {
            const auto ini = exe_dir / crd::containers::StringView{"imgui.ini"};
            m_ini_path = crd::containers::String{ini.generic()};
        }
        else
        {
            m_ini_path = crd::containers::String{crd::containers::StringView{"imgui.ini"}};
        }
        io.IniFilename = m_ini_path.c_str();
    }

    if (m_settings.docking)
    {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    if (m_settings.multi_viewport)
    {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }

    apply_style();

    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(m_app.window().native_handle()), false);

    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
                                         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
                                         {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
    pool_info.poolSizeCount = static_cast<crd::u32>(IM_ARRAYSIZE(pool_sizes));
    pool_info.pPoolSizes = pool_sizes;

    auto vk_device = crd::rhi::vulkan_device(m_device);
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CRD_VERIFY(vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &descriptor_pool) == VK_SUCCESS);
    m_descriptor_pool = descriptor_pool;

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = crd::rhi::vulkan_instance(m_instance);
    init_info.PhysicalDevice = crd::rhi::vulkan_physical_device(m_device);
    init_info.Device = vk_device;
    init_info.QueueFamily = crd::rhi::vulkan_graphics_queue_family_index(m_device);
    init_info.Queue = crd::rhi::vulkan_graphics_queue(m_device);
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = descriptor_pool;
    init_info.Subpass = 0;
    init_info.MinImageCount = crd::rhi::vulkan_swapchain_image_count(m_swapchain);
    init_info.ImageCount = crd::rhi::vulkan_swapchain_image_count(m_swapchain);
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = true;
    init_info.PipelineRenderingCreateInfo = {};
    init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    const VkFormat color_format = crd::rhi::vulkan_swapchain_color_format(m_swapchain);
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.CheckVkResultFn = &check_vk_result;

    ImGui_ImplVulkan_Init(&init_info);
    m_attached = true;
}

void ImGuiLayer::on_detach()
{
    if (!m_attached)
    {
        return;
    }

    m_device.wait_idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptor_pool != nullptr)
    {
        vkDestroyDescriptorPool(crd::rhi::vulkan_device(m_device), static_cast<VkDescriptorPool>(m_descriptor_pool),
                                nullptr);
        m_descriptor_pool = nullptr;
    }

    m_attached = false;
}

void ImGuiLayer::on_frame_begin()
{
    if (!m_attached)
    {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::on_render()
{
    if (!m_attached)
    {
        return;
    }
    build_default_panels();
}

void ImGuiLayer::on_event(crd::app::Event& event)
{
    if (!m_attached)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    crd::app::EventDispatcher dispatcher(event);

    (void)dispatcher.dispatch<crd::app::KeyPressedEvent>(
        [&io](crd::app::KeyPressedEvent& e)
        {
            const ImGuiKey key = to_imgui_key(e.key());
            if (key != ImGuiKey_None)
            {
                io.AddKeyEvent(key, true);
            }
            io.AddKeyEvent(ImGuiMod_Ctrl, e.mods().ctrl);
            io.AddKeyEvent(ImGuiMod_Shift, e.mods().shift);
            io.AddKeyEvent(ImGuiMod_Alt, e.mods().alt);
            return io.WantCaptureKeyboard;
        });

    (void)dispatcher.dispatch<crd::app::KeyReleasedEvent>(
        [&io](crd::app::KeyReleasedEvent& e)
        {
            const ImGuiKey key = to_imgui_key(e.key());
            if (key != ImGuiKey_None)
            {
                io.AddKeyEvent(key, false);
            }
            io.AddKeyEvent(ImGuiMod_Ctrl, e.mods().ctrl);
            io.AddKeyEvent(ImGuiMod_Shift, e.mods().shift);
            io.AddKeyEvent(ImGuiMod_Alt, e.mods().alt);
            return io.WantCaptureKeyboard;
        });

    (void)dispatcher.dispatch<crd::app::MouseMovedEvent>(
        [&io](crd::app::MouseMovedEvent& e)
        {
            io.AddMousePosEvent(e.x(), e.y());
            return io.WantCaptureMouse;
        });

    (void)dispatcher.dispatch<crd::app::MouseScrolledEvent>(
        [&io](crd::app::MouseScrolledEvent& e)
        {
            io.AddMouseWheelEvent(e.dx(), e.dy());
            return io.WantCaptureMouse;
        });

    (void)dispatcher.dispatch<crd::app::MouseButtonPressedEvent>(
        [&io](crd::app::MouseButtonPressedEvent& e)
        {
            io.AddMouseButtonEvent(to_imgui_mouse_button(e.button()), true);
            return io.WantCaptureMouse;
        });

    (void)dispatcher.dispatch<crd::app::MouseButtonReleasedEvent>(
        [&io](crd::app::MouseButtonReleasedEvent& e)
        {
            io.AddMouseButtonEvent(to_imgui_mouse_button(e.button()), false);
            return io.WantCaptureMouse;
        });
}

void ImGuiLayer::render(crd::rhi::CommandBuffer& command_buffer)
{
    if (!m_attached)
    {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), crd::rhi::vulkan_command_buffer(command_buffer));
}

void ImGuiLayer::apply_style()
{
    if (m_settings.theme_preset == "light")
    {
        ImGui::StyleColorsLight();
    }
    else if (m_settings.theme_preset == "classic")
    {
        ImGui::StyleColorsClassic();
    }
    else
    {
        ImGui::StyleColorsDark();
    }

    if (m_settings.multi_viewport)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }
}

void ImGuiLayer::build_default_panels()
{
    if (m_settings.show_stats_panel)
    {
        ImGui::Begin("Cerid Debug");
        ImGui::Text("FPS: %.2F", m_app.clock().delta_seconds() > 0.0 ? 1.0 / m_app.clock().delta_seconds() : 0.0);
        ImGui::Text("Frame ms: %.3F", m_app.clock().delta_seconds() * 1000.0);
        const auto size = m_app.window().window_size();
        ImGui::Text("Window: %d x %d", size.width, size.height);
        ImGui::Text("Docking: %s", m_settings.docking ? "on" : "off");
        ImGui::Text("Multi-viewport: %s", m_settings.multi_viewport ? "on" : "off");
        ImGui::End();
    }

    if (m_settings.show_demo_window)
    {
        ImGui::ShowDemoWindow(&m_settings.show_demo_window);
    }

    if (m_settings.show_metrics_window)
    {
        ImGui::ShowMetricsWindow(&m_settings.show_metrics_window);
    }
}
} // namespace crd::imgui
