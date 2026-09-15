# SPDX-License-Identifier: GPL-3.0-or-later
# clang-tidy integration.
#
# Off by default: it roughly doubles compile time, and the lint CI job is where it belongs.
# Enable locally with -DATLAS_ENABLE_TIDY=ON when working on a module.
#
# The tool is looked up in this order: an explicit ATLAS_CLANG_TIDY, then the Homebrew LLVM
# location (which is not on PATH on the development machine), then PATH.

function(atlas_enable_tidy target)
    if(NOT ATLAS_ENABLE_TIDY)
        return()
    endif()

    if(NOT ATLAS_CLANG_TIDY_EXECUTABLE)
        find_program(ATLAS_CLANG_TIDY_EXECUTABLE
            NAMES clang-tidy
            HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
            DOC "clang-tidy executable used when ATLAS_ENABLE_TIDY is ON")
    endif()

    if(NOT ATLAS_CLANG_TIDY_EXECUTABLE)
        message(FATAL_ERROR
            "ATLAS_ENABLE_TIDY is ON but clang-tidy was not found. "
            "Install it, or set ATLAS_CLANG_TIDY_EXECUTABLE.")
    endif()

    set_target_properties(${target} PROPERTIES
        CXX_CLANG_TIDY "${ATLAS_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*")
endfunction()
