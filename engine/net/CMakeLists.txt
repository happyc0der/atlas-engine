# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::net — deterministic lockstep over the command queue, and nothing else (ADR-0014).
#
# Holds what is genuinely about peers: a bounded inbox, the message codec, an in-memory link,
# and a session. The turn gate and the command-source interface live in simulation instead,
# because a mod is also a command source and must not depend on networking for an interface
# about the command queue.
#
# ENet is the transport, chosen by ADR-0017 and linked PRIVATE, exactly as audio links SDL3.
# **No ENet type appears in any header**, which is what keeps the choice reversible: the socket
# hub is one implementation of `net::Link`, and swapping it would touch one class rather than
# every caller.
#
# The reason this is a PRIVATE_DEPS line and not a DEPENDS one is the whole argument of
# ADR-0017. SDL3_net would have needed `net -> platform`, an edge ModuleGraph.cmake forbids by
# name; ENet opens its own sockets and needs nothing from this engine. So `net`'s module edges
# are still core;simulation, and the diagram is unchanged.
find_package(unofficial-enet CONFIG REQUIRED)

atlas_add_module(net
    SOURCES
        src/inbox.cpp
        src/link.cpp
        src/loopback.cpp
        src/session.cpp
        src/message.cpp
        src/protocol.cpp
    HEADERS
        include/atlas/net/inbox.hpp
        include/atlas/net/link.hpp
        include/atlas/net/loopback.hpp
        include/atlas/net/message.hpp
        include/atlas/net/protocol.hpp
        include/atlas/net/session.hpp
    DEPENDS
        core
        simulation
    PRIVATE_DEPS
        unofficial::enet::enet
)

# Winsock and the multimedia timer, on Windows, from this side.
#
# **ENet does not link them for MSVC and this is not an oversight to report upstream** — the
# port's own patch adds `winmm ws2_32` under `if (MINGW)` only, and the source carries no
# `#pragma comment(lib, ...)`, so a Microsoft-toolchain consumer supplies them itself. Without
# this line the Windows lanes fail at link with unresolved `WSAStartup`, `socket` and
# `timeGetTime` — seventeen Winsock call sites and five timer ones — and nothing on macOS or
# Linux would ever have said so.
#
# Found by reading the port and the upstream CMakeLists before the first push rather than by
# watching a lane go red, which is what M15's three continuous-integration failures bought.
if(WIN32)
    target_link_libraries(atlas_net PRIVATE ws2_32 winmm)
endif()

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
