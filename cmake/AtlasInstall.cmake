# SPDX-License-Identifier: GPL-3.0-or-later
# atlas_install: what `cmake --install` puts in a prefix, so that another project can call
# find_package(Atlas) (ADR-0024).
#
# The prefix holds, and nothing else:
#   include/atlas/<module>/...       every public header, installed by directory (D2)
#   lib/                             every engine module, as the static library it is
#   lib/cmake/Atlas/                 AtlasConfig.cmake, its version file, the export sets
#   share/atlas/shaders/             the sprite shaders renderer::QuadBatch loads (D8)
#   share/atlas/strings/en.json      the engine's string table (D8)
#
# The third-party libraries are not copied. A consumer finds them in the vcpkg tree this build
# used, which it puts on CMAKE_PREFIX_PATH beside the prefix (D9).
#
# Call once, after the engine and the applications have been added.

include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(atlas_install)
    # Only a plain build can be installed (D7). The sanitizer flags are PRIVATE, so a consumer
    # built without them could not link against these libraries; a profile build puts Tracy into
    # every consumer's compile through core/profile.hpp. Refused at install time rather than
    # skipped silently, so `cmake --install` says why it did nothing.
    set(refusal "")
    if(ATLAS_PROFILE)
        set(refusal "a profile build (ATLAS_PROFILE=ON)")
    elseif(NOT ATLAS_SANITIZE STREQUAL "none")
        set(refusal "a sanitizer build (ATLAS_SANITIZE=${ATLAS_SANITIZE})")
    endif()
    if(refusal)
        message(STATUS "Atlas: install refused: this is ${refusal}")
        install(CODE "message(FATAL_ERROR \"Atlas: ${refusal} cannot be installed. Install a \
plain debug or release build; see docs/adr/0024-install-and-export.md, D7.\")")
        return()
    endif()

    set(config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Atlas")
    set(data_dir "${CMAKE_INSTALL_DATADIR}/atlas")

    # Every module that exists, under the name it has always had: atlas_core exports as
    # atlas::core. The internal targets never leave the build (D3), and `runtime` is listed in
    # the graph before it exists.
    set(engine_targets atlas_sdl3)
    set_target_properties(atlas_sdl3 PROPERTIES EXPORT_NAME sdl3)
    foreach(module IN LISTS ATLAS_MODULES)
        if(module MATCHES "_internal$")
            continue()
        endif()
        if(NOT TARGET atlas_${module})
            if(module STREQUAL "runtime")
                continue()
            endif()
            message(FATAL_ERROR
                "atlas_install: module '${module}' is in cmake/ModuleGraph.cmake but has no "
                "target, so it would be missing from the package without anyone noticing.")
        endif()
        set_target_properties(atlas_${module} PROPERTIES EXPORT_NAME ${module})
        list(APPEND engine_targets atlas_${module})
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/${module}/include/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
    endforeach()

    install(TARGETS ${engine_targets} EXPORT AtlasTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(EXPORT AtlasTargets NAMESPACE atlas:: DESTINATION "${config_dir}")

    # The app kit, a component of its own (D4). It exists only when the applications are built.
    if(ATLAS_BUILD_APPS)
        set(app_targets atlas_app_common atlas_app_lockstep atlas_app_mods)
        foreach(target IN LISTS app_targets)
            string(REGEX REPLACE "^atlas_" "" export_name ${target})
            set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
        endforeach()
        install(TARGETS ${app_targets} EXPORT AtlasAppTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/apps/common/include/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
        install(EXPORT AtlasAppTargets NAMESPACE atlas:: DESTINATION "${config_dir}")
    endif()

    # Only the engine's own shader. The lab's cell shaders and the M2 triangle belong to the
    # applications that draw them.
    foreach(stage IN ITEMS vert frag)
        foreach(format IN ITEMS spv msl)
            install(FILES "${PROJECT_SOURCE_DIR}/assets/cooked/shaders/sprite.${stage}.${format}"
                DESTINATION "${data_dir}/shaders")
        endforeach()
    endforeach()
    install(FILES "${PROJECT_SOURCE_DIR}/assets/source/strings/en.json"
        DESTINATION "${data_dir}/strings")

    # What this build was, recorded so the config can refuse a consumer that could not link
    # against it (D5). An empty value is recorded as empty and checks nothing.
    set(ATLAS_INSTALL_DATADIR "${data_dir}")
    set(ATLAS_RECORDED_TRIPLET "${VCPKG_TARGET_TRIPLET}")
    set(ATLAS_RECORDED_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/AtlasConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/package/AtlasConfig.cmake"
        INSTALL_DESTINATION "${config_dir}"
        PATH_VARS ATLAS_INSTALL_DATADIR)
    # SameMinorVersion: while the major version is zero, every minor release may break the
    # installed API, and the minor number is the milestone that last did (D1).
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/package/AtlasConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMinorVersion)
    install(FILES
        "${PROJECT_BINARY_DIR}/package/AtlasConfig.cmake"
        "${PROJECT_BINARY_DIR}/package/AtlasConfigVersion.cmake"
        DESTINATION "${config_dir}")
endfunction()
