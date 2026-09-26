// SPDX-License-Identifier: GPL-3.0-or-later
//
// The internal_header mutation: an installed package has no internal headers (ADR-0024 D3), so
// this must fail to compile. It is built only when run.py asks for that mutation.
#include <atlas/platform/internal/sdl_access.hpp>
