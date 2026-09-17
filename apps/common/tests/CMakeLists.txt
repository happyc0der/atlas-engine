# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(app_common
    SOURCES
        test_run_bounds.cpp
        test_camera_controller.cpp
        test_frame_counters.cpp
        test_ppm.cpp
        test_log_options.cpp
    DEPENDS
        atlas::app_common
    LABELS
        unit
)
