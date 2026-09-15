# SPDX-License-Identifier: GPL-3.0-or-later
# Warning configuration for first-party targets.
#
# Warnings are errors for Atlas code. Third-party code is built separately by vcpkg and is
# consumed through SYSTEM includes, so none of this reaches it.

include(CheckCXXCompilerFlag)

# Warnings that are deliberately off, tested for support because they do not exist in every
# compiler Atlas builds with.
#
#   -Wmissing-designated-field-initializers (Clang 19+, part of -Wextra there but not in
#   Apple clang 21) fires on `{.video = false}` when the aggregate has other members. In C
#   that silence would matter, because omitted fields are silently zeroed. In Atlas every
#   such aggregate has default member initialisers, and setting only the field that differs
#   is the intended style; the alternative is restating defaults at every call site.
check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
                        ATLAS_HAS_NO_MISSING_DESIGNATED_FIELD_INIT)

function(atlas_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-      # Standard conformance; rejects several MSVC-only constructs.
            /utf-8
            /Zc:__cplusplus   # Report the real standard version.
            /Zc:preprocessor  # Conforming preprocessor, needed for __VA_OPT__.
            /EHsc
            # Off-by-default warnings worth having: these catch real bugs.
            /w14062  # Unhandled enumerator in a switch over an enum.
            /w14265  # Class with virtual functions but a non-virtual destructor.
            /w14640  # Thread-unsafe static member initialisation.
            /w14826  # Sign-extending conversion, which may change behaviour.
            /w14905  # Wide string literal cast to LPSTR.
            /w14906  # String literal cast to LPWSTR.
            /w14928  # Illegal copy-initialisation; more than one user-defined conversion.
        )
        if(ATLAS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion         # Closest local proxy for MSVC C4244/C4267, the most
            -Wsign-conversion    # common "builds on macOS, fails on Windows" class.
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wextra-semi
        )
        if(ATLAS_HAS_NO_MISSING_DESIGNATED_FIELD_INIT)
            target_compile_options(${target} PRIVATE
                                   -Wno-missing-designated-field-initializers)
        endif()
        if(ATLAS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Flags every first-party target gets, regardless of warnings.
function(atlas_set_common_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /fp:precise   # No reassociation or contraction. See docs/DETERMINISM.md.
        )
    else()
        target_compile_options(${target} PRIVATE
            -ffp-contract=off  # Do not fuse multiply-add. arm64 would; x86_64 without FMA
                               # would not, and results would differ. See DETERMINISM.md.
        )
        # Frame pointers make Tracy and sanitizer stacks usable. Release builds that are
        # never profiled do not need them, but RelWithDebInfo is the profiled configuration.
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug,RelWithDebInfo>:-fno-omit-frame-pointer>)
    endif()

    # Both platform and rhi link SDL3 privately, and both genuinely need it. For static
    # libraries a private dependency propagates as $<LINK_ONLY:>, so SDL3 legitimately
    # appears twice on the link line of anything that uses both. Apple's linker warns about
    # that while explicitly ignoring it; every other linker is silent. Suppressing the
    # warning is honest here, because there is nothing to fix in the dependency graph.
    if(APPLE)
        target_link_options(${target} PRIVATE -Wl,-no_warn_duplicate_libraries)
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX              # <Windows.h> min/max macros break std::min/std::max.
            WIN32_LEAN_AND_MEAN   # Trim the Windows header surface.
            UNICODE
            _UNICODE
        )
    endif()
endfunction()
