// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/edit/command.hpp>
#include <atlas/rhi/internal/sdl_gpu_access.hpp>
#include <atlas/text/catalog.hpp>
#include <atlas/text/substitute.hpp>
#include <atlas/tools/debug_ui.hpp>
#include <atlas/tools/panels.hpp>
#include <atlas/tools/text_keys.hpp>

#include "imgui_keymap.hpp"
#include <SDL3/SDL_gpu.h>

#include <array>
#include <format>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace atlas::tools {
namespace {

constexpr log::Category kTools{"tools"};

/// Long enough for any name a person types; short enough to live in the panel rather than
/// on the heap. A name longer than this is shown read-only rather than truncated.
constexpr std::size_t kNameBufferSize = 128;

/// Resolve a key, with no catalog meaning every key resolves to itself.
///
/// The absent-catalog path is deliberately the same code as the missing-key path rather than a
/// second one: a panel drawn with no table shows `ui.entity.id`, which is legible and obviously
/// unfinished, and every existing test keeps working without a registry wired into it.
[[nodiscard]] std::string_view tr(const text::Catalog* catalog, std::string_view key) {
    return catalog != nullptr ? catalog->lookup(key) : key;
}

/// Substitute into a looked-up pattern, for the strings that carry numbers.
template <typename... Args>
[[nodiscard]] std::string trf(const text::Catalog* catalog, std::string_view key,
                              const Args&... args) {
    const std::array<std::string, sizeof...(Args)> owned{std::format("{}", args)...};
    std::array<std::string_view, sizeof...(Args)> views{};
    for (std::size_t i = 0; i < owned.size(); ++i) {
        views[i] = owned[i];
    }
    return text::substitute(tr(catalog, key), views);
}

/// A window title the person reads, over a window identifier that never changes.
///
/// Dear ImGui keys a window's docked position and collapsed state on its title, so a title
/// that changed with the locale would forget where the user put the window. `###` makes the
/// part after it the identifier and the part before it the text, which is why the key goes
/// after: it is the one thing about a window that survives a retranslation.
///
/// This helper is the M16 fix. That milestone routed every label *inside* the panels through
/// the catalog and left the five titles, the statistic labels and the mode buttons showing
/// their keys, because `tr` was defined below the first panel that needed it and nothing
/// checked what reached the screen. `test_stats_panel.cpp` now counts the lookups.
[[nodiscard]] std::string title_for(const text::Catalog* catalog, std::string_view key) {
    return std::format("{}###{}", tr(catalog, key), key);
}

/// Tell the overlay which modifiers are held.
///
/// Sent with every key event rather than tracked, because the overlay's shortcuts are checked
/// against its own idea of the modifier state: without this it never sees Ctrl held, and every
/// shortcut is dead however complete the key table is. The physical modifiers are submitted;
/// the library does the Cmd-for-Ctrl substitution on Apple systems itself.
/// Record where the library wants an input method's candidate list.
///
/// Static, and reached through the user-data pointer, because the library's hook is a plain
/// function pointer. The implementation lives behind a stable address for the overlay's
/// lifetime, so storing it is safe.
void record_ime_request(ImGuiContext* /*context*/, ImGuiViewport* /*viewport*/,
                        ImGuiPlatformImeData* data);

void submit_modifiers(ImGuiIO& io, const platform::KeyModifiers& modifiers) {
    io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.control);
    io.AddKeyEvent(ImGuiMod_Shift, modifiers.shift);
    io.AddKeyEvent(ImGuiMod_Alt, modifiers.alt);
    io.AddKeyEvent(ImGuiMod_Super, modifiers.super);
}

}  // namespace

struct DebugUi::Impl {
    ImGuiContext* context = nullptr;

    /// Where text comes from. Borrowed, may be null, and null is a supported state: with no
    /// catalog every key resolves to itself, so the overlay is labelled with keys rather than
    /// being blank. See `DebugUi::set_catalog`.
    const text::Catalog* catalog = nullptr;
    bool frame_open = false;
    bool backend_ready = false;

    /// Selection belongs to the panel, not to the scene. Putting it in the scene would make
    /// a save file depend on what an engineer happened to have clicked.
    std::optional<scene::StableId> selected;

    /// Where an input method should put its candidate list, as the library last reported it.
    /// Sticky: the library calls only when it changes, so this holds the current desire.
    ImeRequest ime;

    /// The name field's buffer and whether it is being edited, so a reseed does not fight a
    /// half-typed name. Panel state, like selection.
    std::array<char, kNameBufferSize> name_input{};
    bool name_editing = false;

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

    // Where an input method should put its candidate list. The library calls this only when
    // the answer changes, so what it stores is the current desire rather than a per-frame
    // event. The application reads it after end_frame and tells the window.
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_SetImeDataFn = record_ime_request;
    // The request field, not the whole implementation: the hook needs nothing else, and a
    // public type keeps it a free function rather than a friend.
    platform_io.Platform_ImeUserData = &impl->ime;

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
        const int button = detail::to_imgui_button(pressed->button);
        if (button >= 0) {
            io.AddMouseButtonEvent(button, true);
        }
        return io.WantCaptureMouse;
    }
    if (const auto* released = std::get_if<platform::MouseButtonReleased>(&event)) {
        const int button = detail::to_imgui_button(released->button);
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
        submit_modifiers(io, key_down->modifiers);
        const ImGuiKey key = detail::to_imgui_key(key_down->key);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, true);
        }
        return io.WantCaptureKeyboard;
    }
    if (const auto* text = std::get_if<platform::TextInput>(&event)) {
        io.AddInputCharactersUTF8(text->c_str());
        return io.WantCaptureKeyboard;
    }
    if (std::holds_alternative<platform::TextEditing>(event)) {
        // Delivered by the platform and deliberately not shown here. The overlay library has
        // no composition interface, and its own window-system backend ignores this event too;
        // the preedit is drawn by the operating system where the operating system draws it.
        // Recorded as a limitation in docs/DEFERRED.md rather than faked.
        return io.WantCaptureKeyboard;
    }
    if (const auto* key_up = std::get_if<platform::KeyReleased>(&event)) {
        submit_modifiers(io, key_up->modifiers);
        const ImGuiKey key = detail::to_imgui_key(key_up->key);
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

bool DebugUi::wants_text_input() const noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(m_impl->context);
    return ImGui::GetIO().WantTextInput;
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

    const std::string window_title = title_for(m_impl->catalog, title);
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
                const std::string_view label = tr(m_impl->catalog, stat.label);
                ImGui::TextUnformatted(label.data(), label.data() + label.size());
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
/// The key naming an asset state.
///
/// `assets::to_string(AssetState)` keeps its own words and gains no dependency on this module:
/// they are log text as well, and routing them through a table would make every integration
/// case's grep depend on a locale. The mapping lives here, on the side that shows them.
[[nodiscard]] std::string_view state_key(assets::AssetState state) {
    switch (state) {
    case assets::AssetState::Unloaded: return keys::kStateUnloaded;
    case assets::AssetState::Queued: return keys::kStateQueued;
    case assets::AssetState::Loading: return keys::kStateLoading;
    case assets::AssetState::Decoded: return keys::kStateDecoded;
    case assets::AssetState::Ready: return keys::kStateReady;
    case assets::AssetState::Failed: return keys::kStateFailed;
    }
    return keys::kStateUnloaded;
}

void draw_tree_node(const text::Catalog* catalog, const scene::Scene& scene, scene::StableId id,
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
    const std::string label = name.empty() ? trf(catalog, keys::kSceneUnnamedEntity, raw)
                                           : std::format("{}##{}", name, raw);

    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = id;
    }

    if (open && !children.empty()) {
        for (const scene::StableId child : children) {
            draw_tree_node(catalog, scene, child, selected);
        }
        ImGui::TreePop();
    }
}

void row(const text::Catalog* catalog, std::string_view key, const std::string& value) {
    const std::string_view label = tr(catalog, key);
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
void draw_position_editor(const text::Catalog* catalog, edit::History& history, scene::StableId id,
                          const scene::LocalTransform& local) {
    std::array<float, 2> position{local.position.x, local.position.y};
    if (ImGui::DragFloat2(std::string{tr(catalog, keys::kEditorLocalPosition)}.c_str(),
                          position.data(), 0.25F)) {
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

/// The animator, as the four things a person actually reaches for while tuning one.
///
/// Above the component table for the same reason the position editor is: a drop-down and two
/// drag fields inside a fixed-fit column are squeezed to nothing.
///
/// Every change goes through `SetAnimator`, which merges with a previous one on the same
/// entity, so scrubbing the start time is one undo step rather than one per frame — and the
/// group is closed when the interaction ends rather than on a timer. Removing the component is
/// a separate command and deliberately does not merge: taking an animation off is one act.
///
/// **What is not here: choosing the clip.** An animator names an asset, and naming one means
/// typing a path and hashing it, which is a file picker rather than a widget. The clip is shown
/// as the identifier the component carries so that a mismatch is at least visible, and the
/// deferral is recorded with what would change it.
void draw_animator_editor(const text::Catalog* catalog, edit::History& history, scene::StableId id,
                          const scene::Animator& animator) {
    const auto emit = [&](const scene::Animator& edited) {
        if (auto status = history.apply(std::make_unique<edit::SetAnimator>(id, edited),
                                        edit::Coalesce::WithPrevious);
            !status) {
            ATLAS_LOG_WARN(kTools, "animator edit refused: {}", status.error());
        }
    };

    bool playing = animator.playing;
    if (ImGui::Checkbox(std::string{tr(catalog, keys::kEditorPlaying)}.c_str(), &playing)) {
        scene::Animator edited = animator;
        edited.playing = playing;
        emit(edited);
        // A checkbox is one act, not a drag: without this the next drag on another field
        // would merge into it and one undo would take both back.
        history.break_coalescing();
    }

    ImGui::SameLine();
    if (ImGui::Button(std::string{tr(catalog, keys::kEditorRemoveAnimator)}.c_str())) {
        if (auto status = history.apply(std::make_unique<edit::RemoveAnimator>(id)); !status) {
            ATLAS_LOG_WARN(kTools, "removing the animator was refused: {}", status.error());
        }
        // The component is gone; nothing below may read it this frame.
        return;
    }

    float speed = animator.speed;
    // The same bounds the component documents and the animator clamps to. A field that let a
    // value past them would show a number the engine does not use.
    //
    // An unchanged value emits nothing, for the reason written at the position editor: a drag
    // field reports itself edited on every frame the pointer rests on it.
    if (ImGui::DragFloat(std::string{tr(catalog, keys::kEditorSpeed)}.c_str(), &speed, 0.01F, 0.0F,
                         scene::kMaxAnimatorSpeed, "%.2f") &&
        speed != animator.speed) {
        scene::Animator edited = animator;
        edited.speed = std::clamp(speed, 0.0F, scene::kMaxAnimatorSpeed);
        emit(edited);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        history.break_coalescing();
    }

    auto start = static_cast<int>(animator.start_ms);
    if (ImGui::DragInt(std::string{tr(catalog, keys::kEditorStart)}.c_str(), &start, 10.0F, 0,
                       static_cast<int>(scene::kMaxAnimatorStartMs))) {
        const auto clamped = static_cast<std::uint32_t>(
            std::clamp(start, 0, static_cast<int>(scene::kMaxAnimatorStartMs)));
        if (clamped != animator.start_ms) {
            scene::Animator edited = animator;
            edited.start_ms = clamped;
            emit(edited);
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        history.break_coalescing();
    }

    // `scene` keeps the names the serialiser writes into a file; those are not display text
    // and do not move. What the combo shows is looked up, and the two lists agree by position
    // exactly as the easing names and the animation module's enumeration do.
    constexpr std::array<std::string_view, 3> kLoopKeys{keys::kLoopOnce, keys::kLoopLoop,
                                                        keys::kLoopPingPong};
    const auto names = scene::animation_loop_names();
    const auto current = static_cast<std::size_t>(animator.loop);
    const std::string shown{current < kLoopKeys.size() ? tr(catalog, kLoopKeys[current])
                                                       : scene::animation_loop_name(animator.loop)};
    if (ImGui::BeginCombo(std::string{tr(catalog, keys::kEditorLoop)}.c_str(), shown.c_str())) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool selected = i == current;
            const std::string item{i < kLoopKeys.size() ? tr(catalog, kLoopKeys[i]) : names[i]};
            if (ImGui::Selectable(item.c_str(), selected) && !selected) {
                scene::Animator edited = animator;
                edited.loop = static_cast<std::uint8_t>(i);
                emit(edited);
                history.break_coalescing();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

/// The name field, as an editable text box that commits one undoable rename.
///
/// Above the component table rather than a row in it, because a text field inside a
/// fixed-fit column is squeezed to nothing, which is the same trap the position editor hit.
///
/// The buffer belongs to the panel and is reseeded from the scene whenever the field is not
/// being edited, so an outside change is picked up but a half-typed name is not thrown away
/// mid-keystroke. A name too long for the buffer is shown read-only instead of truncated:
/// committing a silently shortened name would be worse than not offering to edit it.
///
/// Commits on Enter or on losing focus, as one undo step. `Rename::merge` exists but is not
/// asked for: a rename is one act, not a drag.
void draw_name_editor(const text::Catalog* catalog, edit::History& history, scene::StableId id,
                      std::array<char, kNameBufferSize>& buffer, bool& editing,
                      std::optional<PixelRect>& field_rect) {
    const std::string_view current = history.scene().name(id);
    if (current.size() >= buffer.size()) {
        ImGui::TextUnformatted(trf(catalog, keys::kEditorNameTooLong, current).c_str());
        return;
    }

    if (!editing) {
        buffer.fill('\0');
        std::ranges::copy(current, buffer.begin());
    }

    ImGui::SetNextItemWidth(-1.0F);
    const bool entered = ImGui::InputText("##name", buffer.data(), buffer.size(),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    field_rect = PixelRect{.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};

    editing = ImGui::IsItemActive();

    const bool finished = entered || ImGui::IsItemDeactivatedAfterEdit();
    if (finished) {
        editing = false;
        std::string typed{buffer.data()};
        if (typed != current) {
            if (auto status = history.apply(std::make_unique<edit::Rename>(id, std::move(typed)));
                !status) {
                ATLAS_LOG_WARN(kTools, "rename refused: {}", status.error());
            }
        }
    }
}

void draw_inspector(const text::Catalog* catalog, edit::History& history, scene::StableId id,
                    std::array<char, kNameBufferSize>& name_buffer, bool& name_editing,
                    std::optional<PixelRect>& name_field) {
    const scene::Scene& scene = history.scene();

    // First, because these are the only things here that can be changed. Everything below is a
    // read-only row, and burying the widgets under thirteen of them means scrolling to
    // find the feature this panel exists for.
    draw_name_editor(catalog, history, id, name_buffer, name_editing, name_field);

    const auto* local = scene.local_transform(id);
    if (local != nullptr) {
        draw_position_editor(catalog, history, id, *local);
        ImGui::Separator();
    }

    if (const auto* animator = scene.animator(id)) {
        draw_animator_editor(catalog, history, id, *animator);
        ImGui::Separator();
    }

    if (!ImGui::BeginTable("components", 2, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }

    row(catalog, keys::kRowId, std::format("{}", static_cast<std::uint64_t>(id)));

    const scene::StableId parent = scene.parent(id);
    row(catalog, keys::kRowParent,
        parent == scene::StableId::None ? std::string{tr(catalog, keys::kNone)}
                                        : std::format("{}", static_cast<std::uint64_t>(parent)));
    row(catalog, keys::kRowChildren, std::format("{}", scene.children(id).size()));

    if (local != nullptr) {
        row(catalog, keys::kRowLocalRotation, std::format("{:.3f} rad", local->rotation));
        row(catalog, keys::kRowLocalScale,
            std::format("{:.3f}, {:.3f}", local->scale.x, local->scale.y));
    }

    // Shown separately from the local transform rather than instead of it: when a child is
    // in the wrong place on screen, the question is always which of the two disagrees.
    if (const auto* world = scene.world_transform(id)) {
        const auto elements = world->matrix.uniform_elements();
        row(catalog, keys::kRowWorldTranslation,
            std::format("{:.3f}, {:.3f}", elements[12], elements[13]));
    } else {
        row(catalog, keys::kRowWorldTransform, std::string{tr(catalog, keys::kNotComposed)});
    }

    if (const auto* sprite = scene.sprite(id)) {
        row(catalog, keys::kRowSpriteTexture, std::format("{:#018x}", sprite->texture.value()));
        row(catalog, keys::kRowSpriteSize,
            std::format("{:.3f}, {:.3f}", sprite->size.x, sprite->size.y));
        row(catalog, keys::kRowSpriteTint,
            std::format("{:.2f}, {:.2f}, {:.2f}, {:.2f}", sprite->tint.r, sprite->tint.g,
                        sprite->tint.b, sprite->tint.a));
        row(catalog, keys::kRowSpriteLayer, std::format("{}", sprite->layer));
        row(catalog, keys::kRowSpriteVisible,
            std::string{tr(catalog, sprite->visible ? keys::kYes : keys::kNo)});
    }

    // The authored side of animation. The clip is shown as the identifier the component
    // carries rather than as a path: the component holds a hash, and the panel has no way back
    // from one to the file it came from.
    if (const auto* animator = scene.animator(id)) {
        row(catalog, keys::kRowAnimatorClip, std::format("{:#018x}", animator->clip.value()));
    }

    // And the derived side, read only, beside it. These are what the animator wrote this frame
    // and what the world transform above was composed from; nothing here is authored and none
    // of it is saved. Shown for the same reason the world translation is shown beside the local
    // one: when an entity is in the wrong place the question is always which of the two
    // disagrees, and before this milestone the answer was invisible.
    if (const auto* pose = scene.animation_pose(id)) {
        row(catalog, keys::kRowPoseTime,
            std::format("{:.3f} s", static_cast<double>(pose->elapsed_ns) / 1e9));
        row(catalog, keys::kRowPoseOffset,
            std::format("{:.3f}, {:.3f}", pose->position_offset.x, pose->position_offset.y));
        row(catalog, keys::kRowPoseRotation, std::format("{:.3f} rad", pose->rotation_offset));
        row(catalog, keys::kRowPoseScale,
            std::format("{:.3f}, {:.3f}", pose->scale_factor.x, pose->scale_factor.y));
        row(catalog, keys::kRowPoseFrame,
            pose->frame_uv.has_value()
                ? std::format("{:.3f}, {:.3f} + {:.3f}, {:.3f}", pose->frame_uv->position.x,
                              pose->frame_uv->position.y, pose->frame_uv->size.x,
                              pose->frame_uv->size.y)
                : std::string{tr(catalog, keys::kWholeTexture)});
    }

    if (const auto* camera = scene.camera(id)) {
        row(catalog, keys::kRowCameraZoom, std::format("{:.3f}", camera->zoom));
        row(catalog, keys::kRowCameraActive,
            std::string{tr(catalog, camera->active ? keys::kYes : keys::kNo)});
    }

    ImGui::EndTable();
}

/// Undo and redo, with what they would do written on them.
void draw_history_controls(const text::Catalog* catalog, edit::History& history) {
    ImGui::BeginDisabled(!history.can_undo());
    // Two catalogue entries composed through the substituter, which is why substitution is
    // positional: whether the verb comes before its object is the translator's to decide.
    const std::string undo = history.can_undo()
                                 ? trf(catalog, keys::kUndoWith, tr(catalog, history.undo_label()))
                                 : std::string{tr(catalog, keys::kUndo)};
    if (ImGui::Button(undo.c_str())) {
        // The return value says whether anything happened. Nothing to do when it did not: the
        // button is disabled in that case, and a failed undo has already logged and cleared
        // the history.
        (void)history.undo();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!history.can_redo());
    const std::string redo = history.can_redo()
                                 ? trf(catalog, keys::kRedoWith, tr(catalog, history.redo_label()))
                                 : std::string{tr(catalog, keys::kRedo)};
    if (ImGui::Button(redo.c_str())) {
        (void)history.redo();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted(
        trf(catalog, keys::kHistoryDepth, history.undo_depth(), history.redo_depth()).c_str());
}

}  // namespace

namespace {

// The signature is the library's function-pointer type, which takes a mutable pointer. A const
// parameter would read better and would not match, so the hook would never install.
// NOLINTBEGIN(misc-const-correctness)
void record_ime_request(ImGuiContext* /*context*/, ImGuiViewport* /*viewport*/,
                        ImGuiPlatformImeData* data) {
    auto* request = static_cast<ImeRequest*>(ImGui::GetPlatformIO().Platform_ImeUserData);
    if (request == nullptr || data == nullptr) {
        return;
    }
    *request = ImeRequest{.visible = data->WantVisible,
                          .x = data->InputPos.x,
                          .y = data->InputPos.y,
                          .line_height = data->InputLineHeight};
}

// NOLINTEND(misc-const-correctness)

}  // namespace

ImeRequest DebugUi::ime_request() const noexcept {
    if (m_impl == nullptr) {
        return ImeRequest{};
    }
    return m_impl->ime;
}

ScenePanelReport DebugUi::scene_panel(std::string_view title, edit::History& history) {
    ScenePanelReport report;
    if (m_impl == nullptr || !m_impl->frame_open) {
        return report;
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

    const std::string window_title = title_for(m_impl->catalog, title);
    if (ImGui::Begin(window_title.c_str())) {
        ImGui::TextUnformatted(trf(m_impl->catalog, keys::kSceneEntityCount, scene.size()).c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("tree", ImVec2(0.0F, 180.0F), ImGuiChildFlags_Borders)) {
            for (const scene::StableId root : scene.roots()) {
                draw_tree_node(m_impl->catalog, scene, root, selected);
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        // The controls sit below a scrolling region rather than inside it, so undo is always
        // reachable however long the component list is.
        const float controls_height = ImGui::GetFrameHeightWithSpacing() + 8.0F;
        if (ImGui::BeginChild("inspector", ImVec2(0.0F, -controls_height))) {
            if (selected.has_value()) {
                draw_inspector(m_impl->catalog, history, *selected, m_impl->name_input,
                               m_impl->name_editing, report.name_field);
            } else {
                ImGui::TextUnformatted(
                    std::string{tr(m_impl->catalog, keys::kSceneNoSelection)}.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        draw_history_controls(m_impl->catalog, history);
    }
    ImGui::End();

    m_impl->selected = selected;
    return report;
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

    const std::string window_title = title_for(m_impl->catalog, title);
    if (ImGui::Begin(window_title.c_str())) {
        // The severity words used to exist three times over: here, in `core`'s
        // `to_string(Severity)`, and again in its `to_short_string`. Those two stay as they
        // are, because they are also log text and routing them through a table would make
        // every integration case's grep depend on a locale. This is the one copy that is
        // shown to a person, so it is the one that resolves.
        constexpr std::array<std::string_view, 6> kSeverityKeys{
            keys::kSeverityTrace,   keys::kSeverityDebug, keys::kSeverityInfo,
            keys::kSeverityWarning, keys::kSeverityError, keys::kSeverityFatal};
        std::array<std::string, 6> severity_text{};
        std::array<const char*, 6> severity_items{};
        for (std::size_t i = 0; i < kSeverityKeys.size(); ++i) {
            severity_text[i] = tr(m_impl->catalog, kSeverityKeys[i]);
            severity_items[i] = severity_text[i].c_str();
        }
        int severity = static_cast<int>(m_impl->log_filter.min_severity);
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::Combo(std::string{tr(m_impl->catalog, keys::kLogSeverity)}.c_str(), &severity,
                         severity_items.data(), static_cast<int>(severity_items.size()))) {
            m_impl->log_filter.min_severity = static_cast<log::Severity>(severity);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::InputText(std::string{tr(m_impl->catalog, keys::kLogCategory)}.c_str(),
                             m_impl->log_category_input.data(),
                             m_impl->log_category_input.size())) {
            m_impl->log_filter.category_substring = m_impl->log_category_input.data();
        }
        const ImVec2 filter_min = ImGui::GetItemRectMin();
        const ImVec2 filter_max = ImGui::GetItemRectMax();
        report.filter_field = PixelRect{.x = filter_min.x,
                                        .y = filter_min.y,
                                        .width = filter_max.x - filter_min.x,
                                        .height = filter_max.y - filter_min.y};

        ImGui::SameLine();
        ImGui::Checkbox(std::string{tr(m_impl->catalog, keys::kLogFollow)}.c_str(),
                        &m_impl->log_autoscroll);

        ImGui::SameLine();
        report.clear_requested =
            ImGui::Button(std::string{tr(m_impl->catalog, keys::kLogClear)}.c_str());

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

        ImGui::TextUnformatted(trf(m_impl->catalog, keys::kLogCounts, report.shown, report.hidden,
                                   buffer.size(), buffer.capacity())
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

    const std::string window_title = title_for(m_impl->catalog, title);
    if (ImGui::Begin(window_title.c_str())) {
        const bool paused = view.speed.policy == sim::SpeedPolicy::Paused;
        if (ImGui::Button(
                std::string{tr(m_impl->catalog, paused ? keys::kSimResume : keys::kSimPause)}
                    .c_str())) {
            // Resuming to normal speed rather than to whatever it was before: the panel does
            // not know what that was, and the application does. It may substitute.
            request.speed = paused ? sim::Speed::normal() : sim::Speed::paused();
        }
        ImGui::SameLine();
        if (ImGui::Button(std::string{tr(m_impl->catalog, keys::kSimStep)}.c_str())) {
            request.single_step = true;
        }

        ImGui::SameLine();
        for (const std::uint32_t factor : {1U, 2U, 4U, 8U}) {
            if (ImGui::Button(trf(m_impl->catalog, keys::kSimMultiplier, factor).c_str())) {
                request.speed = sim::Speed::times(factor);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(std::string{tr(m_impl->catalog, keys::kSimUnbounded)}.c_str())) {
            request.speed = sim::Speed::unbounded();
        }

        ImGui::Separator();

        if (!view.modes.empty()) {
            ImGui::TextUnformatted(std::string{tr(m_impl->catalog, keys::kSimDisplayMode)}.c_str());
            for (std::size_t index = 0; index < view.modes.size(); ++index) {
                const bool current = index == view.mode_index;
                ImGui::BeginDisabled(current);
                // Same arrangement as a window title, and for the same reason: the button's
                // identifier is the key, so its text can change without it becoming a
                // different button mid-click.
                const std::string label = title_for(m_impl->catalog, view.modes[index]);
                if (ImGui::Button(label.c_str())) {
                    request.mode_index = index;
                }
                ImGui::EndDisabled();
                // Wrap rather than run off the edge: a caller with more modes, or longer
                // names, must not lose the last of them off the side of the panel.
                if (index + 1 < view.modes.size()) {
                    const float next_width =
                        ImGui::CalcTextSize(
                            std::string{tr(m_impl->catalog, view.modes[index + 1])}.c_str())
                            .x +
                        (ImGui::GetStyle().FramePadding.x * 2.0F);
                    if (ImGui::GetContentRegionAvail().x > next_width) {
                        ImGui::SameLine();
                    }
                }
            }
            ImGui::Separator();
        }

        if (ImGui::Button(std::string{tr(m_impl->catalog, keys::kSimResetView)}.c_str())) {
            request.reset_view = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!view.can_save);
        if (ImGui::Button(std::string{tr(m_impl->catalog, keys::kSimSave)}.c_str())) {
            request.save = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!view.can_load);
        if (ImGui::Button(std::string{tr(m_impl->catalog, keys::kSimLoad)}.c_str())) {
            request.load = true;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted(trf(m_impl->catalog, keys::kSimTickAt, view.tick,
                                   tr(m_impl->catalog, speed_name(view.speed)))
                                   .c_str());
        // Status, not a control. Recording is chosen when the kernel is built, so that a replay
        // always covers a whole run rather than starting from wherever a button was pressed.
        ImGui::TextUnformatted(
            view.recording
                ? trf(m_impl->catalog, keys::kSimRecording, view.recorded_commands).c_str()
                : std::string{tr(m_impl->catalog, keys::kSimNotRecording)}.c_str());
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

    const std::string window_title = title_for(m_impl->catalog, title);
    if (ImGui::Begin(window_title.c_str())) {
        const auto stats = registry.stats();
        ImGui::TextUnformatted(trf(m_impl->catalog, keys::kAssetsSummary, stats.total, stats.ready,
                                   stats.failed, stats.in_progress, stats.awaiting_finalisation)
                                   .c_str());
        ImGui::TextUnformatted(
            trf(m_impl->catalog, keys::kAssetsCache, stats.cache_hits, stats.cache_misses).c_str());
        ImGui::Separator();

        if (ImGui::BeginTable("assets", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(
                std::string{tr(m_impl->catalog, keys::kAssetsColumnPath)}.c_str());
            ImGui::TableSetupColumn(
                std::string{tr(m_impl->catalog, keys::kAssetsColumnState)}.c_str());
            ImGui::TableSetupColumn(
                std::string{tr(m_impl->catalog, keys::kAssetsColumnLoads)}.c_str());
            ImGui::TableSetupColumn(
                std::string{tr(m_impl->catalog, keys::kAssetsColumnBytes)}.c_str());
            ImGui::TableSetupColumn(
                std::string{tr(m_impl->catalog, keys::kAssetsColumnError)}.c_str());
            ImGui::TableHeadersRow();

            // Ordered by path, which the registry guarantees, so rows do not jump about
            // between frames as assets finish loading.
            for (const auto& info : registry.all()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(std::string{info.path.text()}.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(
                    std::string{tr(m_impl->catalog, state_key(info.state))}.c_str());
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

void DebugUi::set_catalog(const text::Catalog* catalog) noexcept {
    if (m_impl != nullptr) {
        m_impl->catalog = catalog;
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
