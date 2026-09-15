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
