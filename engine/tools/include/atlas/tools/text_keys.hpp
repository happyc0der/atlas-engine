// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Every catalogue key the interface asks for (ADR-0016).
///
/// **One list, so "everything the overlay shows goes through the table" is checkable rather
/// than a matter of judgement.** A key used but not listed here would still work — a miss
/// renders as the key — and would silently escape the check that every key resolves. So the
/// rule is that a key appears here and the call site names the constant, never the string.
///
/// `kAllKeys` is what `--text-check` walks. It is a plain array rather than something computed,
/// because a list that derives itself from the call sites would agree with them by construction
/// and prove nothing.
///
/// Keys are identifiers, not prose. They are also what a reader sees when an entry is missing,
/// so they are written to be legible in that state: `ui.undo` is a usable thing to find in a
/// button and `s17` is not.

#include <array>
#include <string_view>

namespace atlas::tools::keys {

// Undo and redo. The `_with` forms take the command's own key as {0}, so the order of the verb
// and its object belongs to the translator rather than to English.
inline constexpr std::string_view kUndo = "ui.undo";
inline constexpr std::string_view kUndoWith = "ui.undo_with";
inline constexpr std::string_view kRedo = "ui.redo";
inline constexpr std::string_view kRedoWith = "ui.redo_with";
inline constexpr std::string_view kHistoryDepth = "ui.history.depth";

// Values that stand in for a component that is absent or trivial.
inline constexpr std::string_view kNone = "ui.value.none";
inline constexpr std::string_view kYes = "ui.value.yes";
inline constexpr std::string_view kNo = "ui.value.no";
inline constexpr std::string_view kNotComposed = "ui.value.not_composed";
inline constexpr std::string_view kWholeTexture = "ui.value.whole_texture";

// Inspector rows.
inline constexpr std::string_view kRowId = "ui.entity.id";
inline constexpr std::string_view kRowParent = "ui.entity.parent";
inline constexpr std::string_view kRowChildren = "ui.entity.children";
inline constexpr std::string_view kRowLocalRotation = "ui.entity.local_rotation";
inline constexpr std::string_view kRowLocalScale = "ui.entity.local_scale";
inline constexpr std::string_view kRowWorldTranslation = "ui.entity.world_translation";
inline constexpr std::string_view kRowWorldTransform = "ui.entity.world_transform";
inline constexpr std::string_view kRowSpriteTexture = "ui.entity.sprite_texture";
inline constexpr std::string_view kRowSpriteSize = "ui.entity.sprite_size";
inline constexpr std::string_view kRowSpriteTint = "ui.entity.sprite_tint";
inline constexpr std::string_view kRowSpriteLayer = "ui.entity.sprite_layer";
inline constexpr std::string_view kRowSpriteVisible = "ui.entity.sprite_visible";
inline constexpr std::string_view kRowAnimatorClip = "ui.entity.animator_clip";
inline constexpr std::string_view kRowPoseTime = "ui.entity.pose_time";
inline constexpr std::string_view kRowPoseOffset = "ui.entity.pose_offset";
inline constexpr std::string_view kRowPoseRotation = "ui.entity.pose_rotation";
inline constexpr std::string_view kRowPoseScale = "ui.entity.pose_scale";
inline constexpr std::string_view kRowPoseFrame = "ui.entity.pose_frame";
inline constexpr std::string_view kRowCameraZoom = "ui.entity.camera_zoom";
inline constexpr std::string_view kRowCameraActive = "ui.entity.camera_active";

// Inspector widgets.
inline constexpr std::string_view kEditorLocalPosition = "ui.editor.local_position";
inline constexpr std::string_view kEditorPlaying = "ui.editor.playing";
inline constexpr std::string_view kEditorRemoveAnimator = "ui.editor.remove_animator";
inline constexpr std::string_view kEditorSpeed = "ui.editor.speed";
inline constexpr std::string_view kEditorStart = "ui.editor.start_ms";
inline constexpr std::string_view kEditorLoop = "ui.editor.loop";
inline constexpr std::string_view kEditorNameTooLong = "ui.editor.name_too_long";

// The scene panel.
inline constexpr std::string_view kSceneEntityCount = "ui.scene.entity_count";
inline constexpr std::string_view kSceneUnnamedEntity = "ui.scene.unnamed_entity";
inline constexpr std::string_view kSceneNoSelection = "ui.scene.no_selection";

// Loop modes, agreeing by position with scene::LoopMode.
inline constexpr std::string_view kLoopOnce = "ui.loop.once";
inline constexpr std::string_view kLoopLoop = "ui.loop.loop";
inline constexpr std::string_view kLoopPingPong = "ui.loop.ping_pong";

// The log console.
inline constexpr std::string_view kLogSeverity = "ui.log.severity";
inline constexpr std::string_view kLogCategory = "ui.log.category";
inline constexpr std::string_view kLogFollow = "ui.log.follow";
inline constexpr std::string_view kLogClear = "ui.log.clear";
inline constexpr std::string_view kLogCounts = "ui.log.counts";

// Severities, agreeing by position with log::Severity.
inline constexpr std::string_view kSeverityTrace = "ui.severity.trace";
inline constexpr std::string_view kSeverityDebug = "ui.severity.debug";
inline constexpr std::string_view kSeverityInfo = "ui.severity.info";
inline constexpr std::string_view kSeverityWarning = "ui.severity.warning";
inline constexpr std::string_view kSeverityError = "ui.severity.error";
inline constexpr std::string_view kSeverityFatal = "ui.severity.fatal";

// Simulation controls.
inline constexpr std::string_view kSimResume = "ui.sim.resume";
inline constexpr std::string_view kSimPause = "ui.sim.pause";
inline constexpr std::string_view kSimStep = "ui.sim.step";
inline constexpr std::string_view kSimUnbounded = "ui.sim.unbounded";
inline constexpr std::string_view kSimDisplayMode = "ui.sim.display_mode";
inline constexpr std::string_view kSimResetView = "ui.sim.reset_view";
inline constexpr std::string_view kSimSave = "ui.sim.save";
inline constexpr std::string_view kSimLoad = "ui.sim.load";
inline constexpr std::string_view kSimTickAt = "ui.sim.tick_at";
inline constexpr std::string_view kSimRecording = "ui.sim.recording";
inline constexpr std::string_view kSimNotRecording = "ui.sim.not_recording";
inline constexpr std::string_view kSimMultiplier = "ui.sim.multiplier";

// Speeds, as `speed_name` reports them.
inline constexpr std::string_view kSpeedPaused = "ui.speed.paused";
inline constexpr std::string_view kSpeedStep = "ui.speed.step";
inline constexpr std::string_view kSpeedUnbounded = "ui.speed.unbounded";
inline constexpr std::string_view kSpeed1x = "ui.speed.1x";
inline constexpr std::string_view kSpeed2x = "ui.speed.2x";
inline constexpr std::string_view kSpeed4x = "ui.speed.4x";
inline constexpr std::string_view kSpeed8x = "ui.speed.8x";
inline constexpr std::string_view kSpeedCustom = "ui.speed.custom";

// The asset panel.
inline constexpr std::string_view kAssetsSummary = "ui.assets.summary";
inline constexpr std::string_view kAssetsCache = "ui.assets.cache";
inline constexpr std::string_view kAssetsColumnPath = "ui.assets.column.path";
inline constexpr std::string_view kAssetsColumnState = "ui.assets.column.state";
inline constexpr std::string_view kAssetsColumnLoads = "ui.assets.column.loads";
inline constexpr std::string_view kAssetsColumnBytes = "ui.assets.column.bytes";
inline constexpr std::string_view kAssetsColumnError = "ui.assets.column.error";

// Asset states, agreeing by position with assets::AssetState.
inline constexpr std::string_view kStateUnloaded = "ui.asset_state.unloaded";
inline constexpr std::string_view kStateQueued = "ui.asset_state.queued";
inline constexpr std::string_view kStateLoading = "ui.asset_state.loading";
inline constexpr std::string_view kStateDecoded = "ui.asset_state.decoded";
inline constexpr std::string_view kStateReady = "ui.asset_state.ready";
inline constexpr std::string_view kStateFailed = "ui.asset_state.failed";

// Panel titles and statistics rows the applications supply. Listed here with the rest, because
// they are shown by the overlay and the point of one list is that it is one list.
inline constexpr std::string_view kTitleLab = "app.title.lab";
inline constexpr std::string_view kTitleControls = "app.title.controls";
inline constexpr std::string_view kTitleLog = "app.title.log";
inline constexpr std::string_view kTitleSandbox = "app.title.sandbox";
inline constexpr std::string_view kTitleScene = "app.title.scene";
inline constexpr std::string_view kTitleAssets = "app.title.assets";

inline constexpr std::string_view kStatFrame = "app.stat.frame";
inline constexpr std::string_view kStatTick = "app.stat.tick";
inline constexpr std::string_view kStatStateHash = "app.stat.state_hash";
inline constexpr std::string_view kStatSpeed = "app.stat.speed";
inline constexpr std::string_view kStatMapMode = "app.stat.map_mode";
inline constexpr std::string_view kStatVisibleChunks = "app.stat.visible_chunks";
inline constexpr std::string_view kStatBatch = "app.stat.batch";
inline constexpr std::string_view kStatEventsCpu = "app.stat.events_cpu";
inline constexpr std::string_view kStatSimulationCpu = "app.stat.simulation_cpu";
inline constexpr std::string_view kStatSnapshotCpu = "app.stat.snapshot_cpu";
inline constexpr std::string_view kStatDrawCpu = "app.stat.draw_cpu";
inline constexpr std::string_view kStatAcquirePresent = "app.stat.acquire_present";
inline constexpr std::string_view kStatZoom = "app.stat.zoom";
inline constexpr std::string_view kStatSeed = "app.stat.seed";
inline constexpr std::string_view kStatCommands = "app.stat.commands";
inline constexpr std::string_view kStatHashVersion = "app.stat.hash_version";
inline constexpr std::string_view kStatAudio = "app.stat.audio";
inline constexpr std::string_view kStatSpritesDrawn = "app.stat.sprites_drawn";
inline constexpr std::string_view kStatQuadsVisible = "app.stat.quads_visible";
inline constexpr std::string_view kStatDrawCalls = "app.stat.draw_calls";
inline constexpr std::string_view kStatUploaded = "app.stat.uploaded";

// Map modes, agreeing by position with the lab's own MapMode. Supplied by the lab, which cannot
// depend on this header from `lab_sim` — the mapping happens in its composition root.
inline constexpr std::string_view kMapRegionValue = "app.map_mode.region_value";
inline constexpr std::string_view kMapOwnerIndex = "app.map_mode.owner_index";
inline constexpr std::string_view kMapPopulationValue = "app.map_mode.population_value";
inline constexpr std::string_view kMapColourIndex = "app.map_mode.colour_index";
inline constexpr std::string_view kMapInvalid = "app.map_mode.invalid";

// Commands, as `edit::Command::label()` reports them. Listed so `--text-check` covers them;
// `edit` names them itself and depends on nothing here.
inline constexpr std::string_view kCommandRename = "edit.command.rename";
inline constexpr std::string_view kCommandMove = "edit.command.move";
inline constexpr std::string_view kCommandSetSprite = "edit.command.set_sprite";
inline constexpr std::string_view kCommandRemoveSprite = "edit.command.remove_sprite";
inline constexpr std::string_view kCommandSetAnimator = "edit.command.set_animator";
inline constexpr std::string_view kCommandRemoveAnimator = "edit.command.remove_animator";
inline constexpr std::string_view kCommandSetCamera = "edit.command.set_camera";
inline constexpr std::string_view kCommandRemoveCamera = "edit.command.remove_camera";
inline constexpr std::string_view kCommandReparent = "edit.command.reparent";
inline constexpr std::string_view kCommandCreate = "edit.command.create";
inline constexpr std::string_view kCommandDestroy = "edit.command.destroy";

/// Every key above, for `--text-check` to resolve against the shipped table.
///
/// Written out rather than generated. A list that derived itself from the call sites would
/// agree with them however wrong both were.
inline constexpr std::array kAllKeys{
    kUndo,
    kUndoWith,
    kRedo,
    kRedoWith,
    kHistoryDepth,
    kNone,
    kYes,
    kNo,
    kNotComposed,
    kWholeTexture,
    kRowId,
    kRowParent,
    kRowChildren,
    kRowLocalRotation,
    kRowLocalScale,
    kRowWorldTranslation,
    kRowWorldTransform,
    kRowSpriteTexture,
    kRowSpriteSize,
    kRowSpriteTint,
    kRowSpriteLayer,
    kRowSpriteVisible,
    kRowAnimatorClip,
    kRowPoseTime,
    kRowPoseOffset,
    kRowPoseRotation,
    kRowPoseScale,
    kRowPoseFrame,
    kRowCameraZoom,
    kRowCameraActive,
    kEditorLocalPosition,
    kEditorPlaying,
    kEditorRemoveAnimator,
    kEditorSpeed,
    kEditorStart,
    kEditorLoop,
    kEditorNameTooLong,
    kSceneEntityCount,
    kSceneUnnamedEntity,
    kSceneNoSelection,
    kLoopOnce,
    kLoopLoop,
    kLoopPingPong,
    kLogSeverity,
    kLogCategory,
    kLogFollow,
    kLogClear,
    kLogCounts,
    kSeverityTrace,
    kSeverityDebug,
    kSeverityInfo,
    kSeverityWarning,
    kSeverityError,
    kSeverityFatal,
    kSimResume,
    kSimPause,
    kSimStep,
    kSimUnbounded,
    kSimDisplayMode,
    kSimResetView,
    kSimSave,
    kSimLoad,
    kSimTickAt,
    kSimRecording,
    kSimNotRecording,
    kSimMultiplier,
    kSpeedPaused,
    kSpeedStep,
    kSpeedUnbounded,
    kSpeed1x,
    kSpeed2x,
    kSpeed4x,
    kSpeed8x,
    kSpeedCustom,
    kAssetsSummary,
    kAssetsCache,
    kAssetsColumnPath,
    kAssetsColumnState,
    kAssetsColumnLoads,
    kAssetsColumnBytes,
    kAssetsColumnError,
    kStateUnloaded,
    kStateQueued,
    kStateLoading,
    kStateDecoded,
    kStateReady,
    kStateFailed,
    kTitleLab,
    kTitleControls,
    kTitleLog,
    kTitleSandbox,
    kTitleScene,
    kTitleAssets,
    kStatFrame,
    kStatTick,
    kStatStateHash,
    kStatSpeed,
    kStatMapMode,
    kStatVisibleChunks,
    kStatBatch,
    kStatEventsCpu,
    kStatSimulationCpu,
    kStatSnapshotCpu,
    kStatDrawCpu,
    kStatAcquirePresent,
    kStatZoom,
    kStatSeed,
    kStatCommands,
    kStatHashVersion,
    kStatAudio,
    kStatSpritesDrawn,
    kStatQuadsVisible,
    kStatDrawCalls,
    kStatUploaded,
    kMapRegionValue,
    kMapOwnerIndex,
    kMapPopulationValue,
    kMapColourIndex,
    kMapInvalid,
    kCommandRename,
    kCommandMove,
    kCommandSetSprite,
    kCommandRemoveSprite,
    kCommandSetAnimator,
    kCommandRemoveAnimator,
    kCommandSetCamera,
    kCommandRemoveCamera,
    kCommandReparent,
    kCommandCreate,
    kCommandDestroy,
};

}  // namespace atlas::tools::keys
