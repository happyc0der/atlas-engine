// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/edit/command.hpp>
#include <atlas/rhi/internal/sdl_gpu_access.hpp>
#include <atlas/tools/debug_ui.hpp>
#include <atlas/tools/panels.hpp>

#include <SDL3/SDL_gpu.h>

#include <array>
#include <format>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

    /// Selection belongs to the panel, not to the scene. Putting it in the scene would make
    /// a save file depend on what an engineer happened to have clicked.
    std::optional<scene::StableId> selected;

    /// The log console's own state. The filter is a view setting, like selection.
    LogFilter log_filter;
    /// A fixed buffer because that is what ImGui::InputText writes into. A std::string sized
    /// to fit would work too, but only by being resized to its capacity and then trimmed at
    /// the terminator on every read, which reads like a mistake even when it is not.
    std::array<char, 64> log_category_input{};
    bool log_autoscroll = true;
    /// Copied from the buffer only when its push count changes, because copying it is a lock
    /// and an allocation per record and this runs every frame.
    std::vector<log::LogBuffer::Entry> log_cache;
    std::uint64_t log_seen_pushes = 0;
    bool log_cache_valid = false;

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
    // Auto-resizing every frame, not just fitting once. A counter's text gets longer as the
    // numbers do, and a window sized on the first frame would clip the rows it was opened to
    // show as soon as they mattered.
    //
    // Deliberately nested rather than merged: the pairing rules are asymmetric, as the note
    // on the inner table explains.
    // NOLINTNEXTLINE(readability-redundant-nested-if)
    if (ImGui::Begin(window_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Fixed-fit, not stretch-proportional. A stretched table takes whatever width the
        // window has and contributes none of its own, so an auto-sized window collapses to
        // its minimum and clips every row. Fitting to content is what makes the window grow
        // to hold the rows it was given.
        if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit)) {
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

namespace {

/// Draw one entity's subtree. Recursive because the tree is, and the scene bounds its own
/// depth, so the recursion is bounded by the same limit.
void draw_tree_node(const scene::Scene& scene, scene::StableId id,
                    std::optional<scene::StableId>& selected) {
    const auto children = scene.children(id);
    const auto raw = static_cast<std::uint64_t>(id);

    auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                 ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selected.has_value() && *selected == id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string_view name = scene.name(id);
    const std::string label =
        name.empty() ? std::format("entity {}", raw) : std::format("{}##{}", name, raw);

    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = id;
    }

    if (open && !children.empty()) {
        for (const scene::StableId child : children) {
            draw_tree_node(scene, child, selected);
        }
        ImGui::TreePop();
    }
}

void row(std::string_view label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value.c_str());
}

/// The local-position row, as two drag fields that emit an edit command.
///
/// Emits through the history, which is the only writer. Three things here are not obvious:
///
/// An unchanged value emits nothing. Dear ImGui reports a drag field as edited on any frame
/// the pointer is held over it, so without this a user resting the mouse on the field would
/// fill the history with commands that change nothing.
///
/// The command coalesces while the field is active, so a drag is one undo step rather than one
/// per frame, and the group is closed when the interaction ends rather than on a timer.
///
/// A refused edit is logged and dropped. It cannot normally happen — the entity was just
/// checked to exist — but the return value says it can, and silently discarding an error
/// because it looks impossible is how it stops looking impossible later.
void draw_position_editor(edit::History& history, scene::StableId id,
                          const scene::LocalTransform& local) {
    std::array<float, 2> position{local.position.x, local.position.y};
    if (ImGui::DragFloat2("local position", position.data(), 0.25F)) {
        const math::Vec2 edited{.x = position[0], .y = position[1]};
        if (edited != local.position) {
            scene::LocalTransform after = local;
            after.position = edited;
            if (auto status = history.apply(std::make_unique<edit::SetLocalTransform>(id, after),
                                            edit::Coalesce::WithPrevious);
                !status) {
                ATLAS_LOG_WARN(kTools, "edit refused: {}", status.error());
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        history.break_coalescing();
    }
}

void draw_inspector(edit::History& history, scene::StableId id) {
    const scene::Scene& scene = history.scene();

    // First, because it is the only thing here that can be changed. Everything below is a
    // read-only row, and burying the one widget under thirteen of them means scrolling to
    // find the feature this panel exists for.
    const auto* local = scene.local_transform(id);
    if (local != nullptr) {
        draw_position_editor(history, id, *local);
        ImGui::Separator();
    }

    if (!ImGui::BeginTable("components", 2, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }

    row("id", std::format("{}", static_cast<std::uint64_t>(id)));
    row("name", std::string{scene.name(id)});

    const scene::StableId parent = scene.parent(id);
    row("parent", parent == scene::StableId::None
                      ? std::string{"none"}
                      : std::format("{}", static_cast<std::uint64_t>(parent)));
    row("children", std::format("{}", scene.children(id).size()));

    if (local != nullptr) {
        row("local rotation", std::format("{:.3f} rad", local->rotation));
        row("local scale", std::format("{:.3f}, {:.3f}", local->scale.x, local->scale.y));
    }

    // Shown separately from the local transform rather than instead of it: when a child is
    // in the wrong place on screen, the question is always which of the two disagrees.
    if (const auto* world = scene.world_transform(id)) {
        const auto elements = world->matrix.uniform_elements();
        row("world translation", std::format("{:.3f}, {:.3f}", elements[12], elements[13]));
    } else {
        row("world transform", "not composed yet");
    }

    if (const auto* sprite = scene.sprite(id)) {
        row("sprite texture", std::format("{:#018x}", sprite->texture.value()));
        row("sprite size", std::format("{:.3f}, {:.3f}", sprite->size.x, sprite->size.y));
        row("sprite tint", std::format("{:.2f}, {:.2f}, {:.2f}, {:.2f}", sprite->tint.r,
                                       sprite->tint.g, sprite->tint.b, sprite->tint.a));
        row("sprite layer", std::format("{}", sprite->layer));
        row("sprite visible", sprite->visible ? "yes" : "no");
    }

    if (const auto* camera = scene.camera(id)) {
        row("camera zoom", std::format("{:.3f}", camera->zoom));
        row("camera active", camera->active ? "yes" : "no");
    }

    ImGui::EndTable();
}

/// Undo and redo, with what they would do written on them.
void draw_history_controls(edit::History& history) {
    ImGui::BeginDisabled(!history.can_undo());
    const std::string undo =
        history.can_undo() ? std::format("Undo {}", history.undo_label()) : std::string{"Undo"};
    if (ImGui::Button(undo.c_str())) {
        // The return value says whether anything happened. Nothing to do when it did not: the
        // button is disabled in that case, and a failed undo has already logged and cleared
        // the history.
        (void)history.undo();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!history.can_redo());
    const std::string redo =
        history.can_redo() ? std::format("Redo {}", history.redo_label()) : std::string{"Redo"};
    if (ImGui::Button(redo.c_str())) {
        (void)history.redo();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted(
        std::format("{} undo, {} redo", history.undo_depth(), history.redo_depth()).c_str());
}

}  // namespace

void DebugUi::scene_panel(std::string_view title, edit::History& history) {
    if (m_impl == nullptr || !m_impl->frame_open) {
        return;
    }
    ImGui::SetCurrentContext(m_impl->context);

    const scene::Scene& scene = history.scene();

    // Taken as a local copy, worked on, and written back once at the end. Reaching through
    // the implementation pointer on every access means nothing can prove the value has not
    // changed between a check and a use, which is both a warning and a fair point.
    std::optional<scene::StableId> selected = m_impl->selected;

    // A selection can outlive what it pointed at, because the panel does not own the scene
    // and is not told when an entity goes away.
    if (selected.has_value() && !scene.contains(*selected)) {
        selected.reset();
    }

    // Placed once, then left to the user. Without this the panel opens exactly where the
    // statistics panel does and hides it, which makes the overlay look broken.
    ImGui::SetNextWindowPos(ImVec2(20.0F, 320.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0F, 520.0F), ImGuiCond_FirstUseEver);

    const std::string window_title{title};
    if (ImGui::Begin(window_title.c_str())) {
        ImGui::TextUnformatted(std::format("{} entities", scene.size()).c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("tree", ImVec2(0.0F, 180.0F), ImGuiChildFlags_Borders)) {
            for (const scene::StableId root : scene.roots()) {
                draw_tree_node(scene, root, selected);
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        // The controls sit below a scrolling region rather than inside it, so undo is always
        // reachable however long the component list is.
        const float controls_height = ImGui::GetFrameHeightWithSpacing() + 8.0F;
        if (ImGui::BeginChild("inspector", ImVec2(0.0F, -controls_height))) {
            if (selected.has_value()) {
                draw_inspector(history, *selected);
            } else {
                ImGui::TextUnformatted("No entity selected.");
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        draw_history_controls(history);
    }
    ImGui::End();

    m_impl->selected = selected;
}

LogConsoleReport DebugUi::log_console_panel(std::string_view title, const log::LogBuffer& buffer) {
    LogConsoleReport report;
    if (m_impl == nullptr || !m_impl->frame_open) {
        return report;
    }
    ImGui::SetCurrentContext(m_impl->context);

    // The buffer is shared with every thread that logs, and entries() copies it whole under a
    // lock. Comparing the push count first turns that into one cheap locked read per frame,
    // and the count is monotonic so eviction cannot make a change look like no change.
    const std::uint64_t pushes = buffer.push_count();
    if (!m_impl->log_cache_valid || pushes != m_impl->log_seen_pushes) {
        m_impl->log_cache = buffer.entries();
        m_impl->log_seen_pushes = pushes;
        m_impl->log_cache_valid = true;
    }

    // Below the asset panel rather than on top of it. Every panel here places itself once and
    // is then left alone, so the first-open layout is the only chance to not look broken.
    ImGui::SetNextWindowPos(ImVec2(400.0F, 510.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0F, 280.0F), ImGuiCond_FirstUseEver);

    const std::string window_title{title};
    if (ImGui::Begin(window_title.c_str())) {
        constexpr std::array<const char*, 6> kSeverities{"trace",   "debug", "info",
                                                         "warning", "error", "fatal"};
        int severity = static_cast<int>(m_impl->log_filter.min_severity);
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::Combo("severity", &severity, kSeverities.data(),
                         static_cast<int>(kSeverities.size()))) {
            m_impl->log_filter.min_severity = static_cast<log::Severity>(severity);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::InputText("category", m_impl->log_category_input.data(),
                             m_impl->log_category_input.size())) {
            m_impl->log_filter.category_substring = m_impl->log_category_input.data();
        }

        ImGui::SameLine();
        ImGui::Checkbox("follow", &m_impl->log_autoscroll);

        ImGui::SameLine();
        report.clear_requested = ImGui::Button("Clear");

        ImGui::Separator();

        if (ImGui::BeginChild("records", ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing()))) {
            for (const auto& entry : m_impl->log_cache) {
                if (!matches(entry, m_impl->log_filter)) {
                    ++report.hidden;
                    continue;
                }
                ++report.shown;
                ImGui::TextUnformatted(std::format("{:<7} [{}] {}", log::to_string(entry.severity),
                                                   entry.category, entry.message)
                                           .c_str());
            }
            if (m_impl->log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0F);
            }
        }
        ImGui::EndChild();

        ImGui::TextUnformatted(std::format("{} shown, {} hidden, {} held of {}", report.shown,
                                           report.hidden, buffer.size(), buffer.capacity())
                                   .c_str());
    }
    ImGui::End();
    return report;
}

SimulationControlsRequest DebugUi::simulation_controls_panel(std::string_view title,
                                                             const SimulationControlsView& view) {
    SimulationControlsRequest request;
    if (m_impl == nullptr || !m_impl->frame_open) {
        return request;
    }
    ImGui::SetCurrentContext(m_impl->context);

    ImGui::SetNextWindowPos(ImVec2(400.0F, 20.0F), ImGuiCond_FirstUseEver);
    // Wide enough for the mode buttons on one line. Sized from the longest name a caller has
    // rather than guessed: at 360 the lab's fourth mode ran off the edge of the panel.
    ImGui::SetNextWindowSize(ImVec2(600.0F, 210.0F), ImGuiCond_FirstUseEver);

    const std::string window_title{title};
    if (ImGui::Begin(window_title.c_str())) {
        const bool paused = view.speed.policy == sim::SpeedPolicy::Paused;
        if (ImGui::Button(paused ? "Resume" : "Pause")) {
            // Resuming to normal speed rather than to whatever it was before: the panel does
            // not know what that was, and the application does. It may substitute.
            request.speed = paused ? sim::Speed::normal() : sim::Speed::paused();
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            request.single_step = true;
        }

        ImGui::SameLine();
        for (const std::uint32_t factor : {1U, 2U, 4U, 8U}) {
            if (ImGui::Button(std::format("{}x", factor).c_str())) {
                request.speed = sim::Speed::times(factor);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Unbounded")) {
            request.speed = sim::Speed::unbounded();
        }

        ImGui::Separator();

        if (!view.modes.empty()) {
            ImGui::TextUnformatted("display mode");
            for (std::size_t index = 0; index < view.modes.size(); ++index) {
                const bool current = index == view.mode_index;
                ImGui::BeginDisabled(current);
                const std::string label{view.modes[index]};
                if (ImGui::Button(label.c_str())) {
                    request.mode_index = index;
                }
                ImGui::EndDisabled();
                // Wrap rather than run off the edge: a caller with more modes, or longer
                // names, must not lose the last of them off the side of the panel.
                if (index + 1 < view.modes.size()) {
                    const float next_width =
                        ImGui::CalcTextSize(std::string{view.modes[index + 1]}.c_str()).x +
                        (ImGui::GetStyle().FramePadding.x * 2.0F);
                    if (ImGui::GetContentRegionAvail().x > next_width) {
                        ImGui::SameLine();
                    }
                }
            }
            ImGui::Separator();
        }

        if (ImGui::Button("Reset view")) {
            request.reset_view = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!view.can_save);
        if (ImGui::Button("Save")) {
            request.save = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!view.can_load);
        if (ImGui::Button("Load")) {
            request.load = true;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted(
            std::format("tick {} at {}", view.tick, speed_name(view.speed)).c_str());
        // Status, not a control. Recording is chosen when the kernel is built, so that a replay
        // always covers a whole run rather than starting from wherever a button was pressed.
        ImGui::TextUnformatted(
            view.recording ? std::format("recording: {} commands", view.recorded_commands).c_str()
                           : "not recording");
    }
    ImGui::End();
    return request;
}

void DebugUi::asset_panel(std::string_view title, const assets::Registry& registry) {
    if (m_impl == nullptr || !m_impl->frame_open) {
        return;
    }
    ImGui::SetCurrentContext(m_impl->context);

    ImGui::SetNextWindowPos(ImVec2(400.0F, 250.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0F, 240.0F), ImGuiCond_FirstUseEver);

    const std::string window_title{title};
    if (ImGui::Begin(window_title.c_str())) {
        const auto stats = registry.stats();
        ImGui::TextUnformatted(
            std::format("{} assets: {} ready, {} failed, {} loading, {} awaiting finalisation",
                        stats.total, stats.ready, stats.failed, stats.in_progress,
                        stats.awaiting_finalisation)
                .c_str());
        ImGui::TextUnformatted(
            std::format("cache: {} hits, {} misses", stats.cache_hits, stats.cache_misses).c_str());
        ImGui::Separator();

        if (ImGui::BeginTable("assets", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("path");
            ImGui::TableSetupColumn("state");
            ImGui::TableSetupColumn("loads");
            ImGui::TableSetupColumn("bytes");
            ImGui::TableSetupColumn("error");
            ImGui::TableHeadersRow();

            // Ordered by path, which the registry guarantees, so rows do not jump about
            // between frames as assets finish loading.
            for (const auto& info : registry.all()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(std::string{info.path.text()}.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::string{assets::to_string(info.state)}.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(std::format("{}", info.load_count).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(std::format("{}", info.bytes).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(info.error.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void DebugUi::select_entity(scene::StableId id) noexcept {
    if (m_impl == nullptr) {
        return;
    }
    if (id == scene::StableId::None) {
        m_impl->selected.reset();
    } else {
        m_impl->selected = id;
    }
}

std::optional<scene::StableId> DebugUi::selected_entity() const noexcept {
    return m_impl == nullptr ? std::nullopt : m_impl->selected;
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
