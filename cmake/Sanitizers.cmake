# SPDX-License-Identifier: GPL-3.0-or-later
# Sanitizer configuration, selected by the ATLAS_SANITIZE cache variable.
#
# ASan and TSan are never combined: they instrument the same things differently and the
# result is neither reliable nor fast. Each has its own preset.
#
# LeakSanitizer is unavailable on macOS arm64, so leak detection is the Linux ASan job's
# responsibility. TSan on Apple Silicon works but reports noise from uninstrumented system
# libraries, which is why TSan presets run only unit and determinism tests.

function(atlas_apply_sanitizers target)
    if(ATLAS_SANITIZE STREQUAL "none")
        return()
    endif()

    if(MSVC)
        if(ATLAS_SANITIZE STREQUAL "address")
            # MSVC ASan requires /Zi and is incompatible with /RTC1 and /INCREMENTAL.
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        else()
            message(FATAL_ERROR
                "ATLAS_SANITIZE=${ATLAS_SANITIZE} is not supported with MSVC. "
                "MSVC supports 'address' only.")
        endif()
        return()
    endif()

    if(ATLAS_SANITIZE STREQUAL "address")
        set(_flags -fsanitize=address,undefined
                   -fsanitize=float-cast-overflow  # Out-of-range float-to-int differs
                                                   # between arm64 and x86_64.
                   -fno-sanitize-recover=all
                   -fno-omit-frame-pointer)
    elseif(ATLAS_SANITIZE STREQUAL "thread")
        set(_flags -fsanitize=thread
                   -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "ATLAS_SANITIZE must be one of: none, address, thread. Got '${ATLAS_SANITIZE}'.")
    endif()

    target_compile_options(${target} PRIVATE ${_flags})
    target_link_options(${target} PRIVATE ${_flags})
endfunction()
