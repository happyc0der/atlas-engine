// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/rhi/internal/sdl_gpu_access.hpp>
#include <atlas/tools/debug_ui.hpp>

#include <SDL3/SDL_gpu.h>

#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <string>
#include <utility>
#include <variant>

namespace atlas::tools {
namespace {

constexpr log::Category kTools{"tools"};

/// Atlas's key identifiers, mapped to the overlay library's own.
///
/// Only the keys an engineering overlay needs. The alternative was the library's SDL
/// backend, which would have required raw window-system events inside this module and
/// therefore a third-party type crossing the platform boundary. Driving the input directly
/// from Atlas's own event types costs this table and keeps the boundary intact.
[[nodiscard]] ImGuiKey to_imgui_key(platform::Key key) noexcept {
    using platform::Key;
    switch (key) {
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;
    case Key::PageUp: return ImGuiKey_PageUp;
    case Key::PageDown: return ImGuiKey_PageDown;
    case Key::Home: return ImGuiKey_Home;
    case Key::End: return ImGuiKey_End;
    case Key::Insert: return ImGuiKey_Insert;
    case Key::Delete: return ImGuiKey_Delete;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Space: return ImGuiKey_Space;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Escape: return ImGuiKey_Escape;
    case Key::LeftShift: return ImGuiKey_LeftShift;
    case Key::RightShift: return ImGuiKey_RightShift;
    case Key::LeftControl: return ImGuiKey_LeftCtrl;
    case Key::RightControl: return ImGuiKey_RightCtrl;
    case Key::LeftAlt: return ImGuiKey_LeftAlt;
    case Key::RightAlt: return ImGuiKey_RightAlt;
    case Key::LeftSuper: return ImGuiKey_LeftSuper;
    case Key::RightSuper: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

[[nodiscard]] int to_imgui_button(platform::MouseButton button) noexcept {
    switch (button) {
    case platform::MouseButton::Left: return 0;
    case platform::MouseButton::Right: return 1;
    case platform::MouseButton::Middle: return 2;
    default: return -1;
    }
}

}  // namespace

struct DebugUi::Impl {
    ImGuiContext* context = nullptr;
    bool frame_open = false;
    bool backend_ready = false;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        if (context == nullptr) {
            return;
        }
        ImGui::SetCurrentContext(context);
        if (backend_ready) {
            ImGui_ImplSDLGPU3_Shutdown();
        }
        ImGui::DestroyContext(context);
        context = nullptr;
    }
};

DebugUi::DebugUi(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

Result<DebugUi> DebugUi::create(rhi::Device& device, const platform::Window& window) {
    ATLAS_ASSERT_MAIN_THREAD();

    SDL_GPUDevice* native_device = rhi::internal::native_device(device);
    if (native_device == nullptr || !window.valid()) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the debug overlay needs a live window and device"));
    }

    IMGUI_CHECKVERSION();
    auto impl = std::make_unique<DebugUi::Impl>();
    impl->context = ImGui::CreateContext();

    ImGui::SetCurrentContext(impl->context);
    ImGuiIO& io = ImGui::GetIO();
    // NOLINTNEXTLINE(bugprone-signed-bitwise): the library's flag constants are signed.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // No settings file: this is an engineering overlay whose layout is set in code, and
    // writing an .ini beside the binary would leave a stray file in the repository.
    io.IniFilename = nullptr;

    // The overlay is drawn in the swapchain's pixels, so its coordinates are pixels too.
    const auto pixels = window.pixel_size();
    io.DisplaySize = ImVec2{static_cast<float>(pixels.width), static_cast<float>(pixels.height)};

    ImGui::StyleColorsDark();

    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = native_device;
    init.ColorTargetFormat =
        static_cast<SDL_GPUTextureFormat>(rhi::internal::swapchain_texture_format(device));
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;

    if (!ImGui_ImplSDLGPU3_Init(&init)) {
        return std::unexpected(
            Error(ErrorCode::Internal, "initialising the overlay's renderer backend failed"));
    }
    impl->backend_ready = true;

    ATLAS_LOG_INFO(kTools, "debug overlay ready");
    return DebugUi{std::move(impl)};
}

DebugUi::~DebugUi() = default;

DebugUi::DebugUi(DebugUi&& other) noexcept : m_impl(std::move(other.m_impl)) {}

DebugUi& DebugUi::operator=(DebugUi&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool DebugUi::handle_event(const platform::Event& event) {
    if (m_impl == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(m_impl->context);
    ImGuiIO& io = ImGui::GetIO();

    if (const auto* moved = std::get_if<platform::MouseMoved>(&event)) {
        io.AddMousePosEvent(moved->position.x, moved->position.y);
        return io.WantCaptureMouse;
    }
    if (const auto* pressed = std::get_if<platform::MouseButtonPressed>(&event)) {
        const int button = to_imgui_button(pressed->button);
        if (button >= 0) {
            io.AddMouseButtonEvent(button, true);
        }
        return io.WantCaptureMouse;
    }
    if (const auto* released = std::get_if<platform::MouseButtonReleased>(&event)) {
        const int button = to_imgui_button(released->button);
        if (button >= 0) {
            io.AddMouseButtonEvent(button, false);
        }
        return io.WantCaptureMouse;
    }
    if (const auto* wheel = std::get_if<platform::MouseWheel>(&event)) {
        io.AddMouseWheelEvent(wheel->delta_x, wheel->delta_y);
        return io.WantCaptureMouse;
    }
    if (const auto* key_down = std::get_if<platform::KeyPressed>(&event)) {
        const ImGuiKey key = to_imgui_key(key_down->key);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, true);
        }
        return io.WantCaptureKeyboard;
    }
    if (const auto* key_up = std::get_if<platform::KeyReleased>(&event)) {
        const ImGuiKey key = to_imgui_key(key_up->key);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, false);
        }
        return io.WantCaptureKeyboard;
    }
    if (const auto* resized = std::get_if<platform::WindowResized>(&event)) {
        io.DisplaySize = ImVec2{static_cast<float>(resized->pixel_size.width),
                                static_cast<float>(resized->pixel_size.height)};
        return false;
    }

    return false;
}

bool DebugUi::wants_mouse() const noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(m_impl->context);
    return ImGui::GetIO().WantCaptureMouse;
}

bool DebugUi::wants_keyboard() const noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(m_impl->context);
    return ImGui::GetIO().WantCaptureKeyboard;
}

void DebugUi::begin_frame(float delta_seconds, std::uint32_t pixel_width,
                          std::uint32_t pixel_height) {
    if (m_impl == nullptr || m_impl->frame_open) {
        return;
    }
    ATLAS_ZONE_NAMED("DebugUi::begin_frame");
    ATLAS_ASSERT_MAIN_THREAD();

    ImGui::SetCurrentContext(m_impl->context);
    ImGuiIO& io = ImGui::GetIO();
    if (pixel_width > 0 && pixel_height > 0) {
        io.DisplaySize = ImVec2{static_cast<float>(pixel_width), static_cast<float>(pixel_height)};
    }
    // A zero or negative delta makes the library's animations misbehave, and a frame that
    // took no measurable time is perfectly possible.
    io.DeltaTime = delta_seconds > 0.0F ? delta_seconds : 1.0F / 60.0F;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
    m_impl->frame_open = true;
}

void DebugUi::stats_panel(std::string_view title, std::span<const Stat> stats) {
    if (m_impl == nullptr || !m_impl->frame_open) {
        return;
    }
    ImGui::SetCurrentContext(m_impl->context);

    const std::string window_title{title};
    // Deliberately nested rather than merged. The library's pairing rules are asymmetric:
    // End must be called whether or not Begin returned true, while EndTable must be called
    // only when BeginTable did. Collapsing the two conditions hides that difference.
    // NOLINTNEXTLINE(readability-redundant-nested-if)
    if (ImGui::Begin(window_title.c_str())) {
        if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& stat : stats) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(stat.label.data(), stat.label.data() + stat.label.size());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(stat.value.data(), stat.value.data() + stat.value.size());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

DebugUi::PreparedFrame DebugUi::end_frame(rhi::Frame& frame) {
    if (m_impl == nullptr || !m_impl->frame_open) {
        return PreparedFrame{false};
    }
    ATLAS_ZONE_NAMED("DebugUi::end_frame");
    ATLAS_ASSERT_MAIN_THREAD();

    ImGui::SetCurrentContext(m_impl->context);
    ImGui::Render();
    m_impl->frame_open = false;

    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* commands = rhi::internal::native_command_buffer(frame);
    if (draw_data == nullptr || commands == nullptr) {
        return PreparedFrame{false};
    }

    // This begins a copy pass to upload the vertex data, which is why it cannot happen
    // inside the render pass that draws it: the graphics library refuses to nest passes.
    ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, commands);
    return PreparedFrame{true};
}

void DebugUi::draw(rhi::RenderPass& pass, const PreparedFrame& prepared) {
    if (m_impl == nullptr || !prepared.valid()) {
        return;
    }
    ATLAS_ZONE_NAMED("DebugUi::draw");
    ATLAS_ASSERT_MAIN_THREAD();

    ImGui::SetCurrentContext(m_impl->context);
    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* commands = rhi::internal::native_command_buffer_of(pass);
    SDL_GPURenderPass* native_pass = rhi::internal::native_render_pass(pass);

    if (draw_data != nullptr && commands != nullptr && native_pass != nullptr) {
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, commands, native_pass);
    }
}

}  // namespace atlas::tools
