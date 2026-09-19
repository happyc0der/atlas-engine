# SPDX-License-Identifier: GPL-3.0-or-later
#
# Integration tests: these run the sandbox binary rather than linking a module.
#
# The sandbox is the composition root. Subsystem construction order, shutdown order, exit
# codes and lifecycle logging exist only there, so unit tests cannot reach them: every one of
# those is a property of the whole program starting and stopping.
#
# Each case is a separate CTest entry so that a failure names the behaviour that broke.
# Labelled "integration", except the one that needs a video backend, which is labelled
# "display" alongside the other tests that run under the dummy driver.
if(NOT ATLAS_BUILD_TESTS OR NOT ATLAS_BUILD_APPS)
    return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Atlas: Python not found; the integration checks will not run as tests")
    return()
endif()

set(_atlas_sandbox_checks "${CMAKE_CURRENT_SOURCE_DIR}/integration/sandbox_checks.py")

# Cases that need no video backend at all.
set(_atlas_headless_cases
    version
    help
    headless_lifecycle
    headless_reaches_exact_tick
    unbounded_throughput
    edit_round_trip
    scene_composition
    animation_plays
    headless_needs_a_bound
    rejects_unknown_option
    rejects_bad_values
    log_file
    log_level_filters
)

foreach(case IN LISTS _atlas_headless_cases)
    add_test(NAME "sandbox_${case}"
             COMMAND "${Python3_EXECUTABLE}" "${_atlas_sandbox_checks}"
                     --binary "$<TARGET_FILE:atlas_sandbox>"
                     --case "${case}")
    # The working directory matters: the sandbox resolves its asset and shader paths
    # relative to wherever it was started, exactly as a developer runs it.
    set_tests_properties("sandbox_${case}" PROPERTIES
        LABELS "integration"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endforeach()

# Needs SDL's video subsystem, which the dummy driver provides without a display.
add_test(NAME "sandbox_window_under_dummy_driver"
         COMMAND "${Python3_EXECUTABLE}" "${_atlas_sandbox_checks}"
                 --binary "$<TARGET_FILE:atlas_sandbox>"
                 --case window_under_dummy_driver)
set_tests_properties("sandbox_window_under_dummy_driver" PROPERTIES
    LABELS "display;integration"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# Reads the one log line that says which subsystems a composition root actually asked for.
# M11 shipped a gamepad neither application enabled; no unit test could see that, because
# every gamepad test builds its own platform. This is the check that would have.
add_test(NAME "sandbox_gamepad_follows_the_window"
         COMMAND "${Python3_EXECUTABLE}" "${_atlas_sandbox_checks}"
                 --binary "$<TARGET_FILE:atlas_sandbox>"
                 --case gamepad_follows_the_window)
set_tests_properties("sandbox_gamepad_follows_the_window" PROPERTIES
    LABELS "display;integration"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# The whole asset path for a sound: requested, decoded on a worker, finalised into a clip,
# resampled, played, and replaced when the file changes on disk.
add_test(NAME "sandbox_audio_clip_loads_and_reloads"
         COMMAND "${Python3_EXECUTABLE}" "${_atlas_sandbox_checks}"
                 --binary "$<TARGET_FILE:atlas_sandbox>"
                 --case audio_clip_loads_and_reloads)
set_tests_properties("sandbox_audio_clip_loads_and_reloads" PROPERTIES
    LABELS "display;integration"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# ----------------------------------------------------------------------------- the lab
# The Strategy Lab is a second composition root and carries the milestone's determinism
# claims, which only whole-program runs can test: a second run, a save, a replay.
set(_atlas_lab_checks "${CMAKE_CURRENT_SOURCE_DIR}/integration/lab_checks.py")
set(_atlas_lab_headless_cases
    version
    help
    headless_lifecycle
    headless_reaches_exact_tick
    headless_needs_a_bound
    catch_up_is_bounded
    deterministic_hash
    seed_changes_hash
    commands_change_hash
    mod_runs_and_changes_state
    mod_path_is_validated
    mod_is_not_a_peer
    mod_replay_needs_no_runtime
    mod_peers_agree
    mod_clock_diverges
    save_writes_file
    load_continues
    load_different_grid_adopts_layout
    refuses_truncated_save
    refuses_mismatched_grid
    record_then_play_matches
    loopback_peers_agree
    loopback_differs_from_solo
    loopback_held_turn_stalls_then_completes
    loopback_unreleased_hold_fails_fast
    loopback_corrupt_turn_is_caught
    replay_detects_tampering
    replay_refuses_wrong_seed
    rejects_unknown_option
    rejects_bad_values
    log_file
)
if(TARGET atlas_lab)
    foreach(case IN LISTS _atlas_lab_headless_cases)
        add_test(NAME "lab_${case}"
                 COMMAND "${Python3_EXECUTABLE}" "${_atlas_lab_checks}"
                         --binary "$<TARGET_FILE:atlas_lab>"
                         --case "${case}")
        set_tests_properties("lab_${case}" PROPERTIES
            LABELS "integration"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    endforeach()

    add_test(NAME "lab_window_under_dummy_driver"
             COMMAND "${Python3_EXECUTABLE}" "${_atlas_lab_checks}"
                     --binary "$<TARGET_FILE:atlas_lab>"
                     --case window_under_dummy_driver)
    # "integration" as well as "display": it is a whole-binary run like the sandbox's
    # equivalent, which has carried both labels since it was written.
    set_tests_properties("lab_window_under_dummy_driver" PROPERTIES
        LABELS "display;integration"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

    add_test(NAME "lab_gamepad_follows_the_window"
             COMMAND "${Python3_EXECUTABLE}" "${_atlas_lab_checks}"
                     --binary "$<TARGET_FILE:atlas_lab>"
                     --case gamepad_follows_the_window)
    set_tests_properties("lab_gamepad_follows_the_window" PROPERTIES
        LABELS "display;integration"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

    add_test(NAME "lab_audio_follows_the_window"
             COMMAND "${Python3_EXECUTABLE}" "${_atlas_lab_checks}"
                     --binary "$<TARGET_FILE:atlas_lab>"
                     --case audio_follows_the_window)
    set_tests_properties("lab_audio_follows_the_window" PROPERTIES
        LABELS "display;integration"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

    # Needs a real device: it draws the identifier pass and reads a pixel back.
    add_test(NAME "lab_pick_returns_expected_cell"
             COMMAND "${Python3_EXECUTABLE}" "${_atlas_lab_checks}"
                     --binary "$<TARGET_FILE:atlas_lab>"
                     --case pick_returns_expected_cell)
    set_tests_properties("lab_pick_returns_expected_cell" PROPERTIES
        LABELS "gpu"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
