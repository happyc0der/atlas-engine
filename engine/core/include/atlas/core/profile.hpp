// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Profiling macros.
///
/// These expand to Tracy zones when the build is configured with -DATLAS_PROFILE=ON, and
/// to nothing otherwise: a default build contains no Tracy symbol at all. That is what
/// "instrumented from milestone zero, compiled out in normal builds" means in practice.
///
/// The Tracy client and the Tracy GUI must be the same version, because the wire protocol
/// changes between releases. The pinned pair is recorded in docs/DEPENDENCIES.md.
///
/// Tracy headers are permitted in this public header by the exception recorded in
/// cmake/ModuleGraph.cmake: the macros *are* the API, and they vanish when disabled.
///
/// Usage:
/// \code
///   void step() {
///       ATLAS_ZONE();                  // named after the enclosing function
///       ATLAS_ZONE_NAMED("commit");    // explicitly named
///   }
///   ATLAS_FRAME_MARK();                // once per presented frame
/// \endcode

#if ATLAS_TRACY_ENABLED
#include <tracy/Tracy.hpp>

#define ATLAS_ZONE() ZoneScoped
#define ATLAS_ZONE_NAMED(name) ZoneScopedN(name)
#define ATLAS_ZONE_COLORED(name, color) ZoneScopedNC(name, color)
#define ATLAS_FRAME_MARK() FrameMark
#define ATLAS_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#define ATLAS_THREAD_NAME(name) tracy::SetThreadName(name)
#define ATLAS_PLOT(name, value) TracyPlot(name, value)
#define ATLAS_MESSAGE(text) TracyMessageL(text)
#else
#define ATLAS_ZONE() ((void)0)
#define ATLAS_ZONE_NAMED(name) ((void)0)
#define ATLAS_ZONE_COLORED(name, color) ((void)0)
#define ATLAS_FRAME_MARK() ((void)0)
#define ATLAS_FRAME_MARK_NAMED(name) ((void)0)
#define ATLAS_THREAD_NAME(name) ((void)0)
#define ATLAS_PLOT(name, value) ((void)0)
#define ATLAS_MESSAGE(text) ((void)0)
#endif
