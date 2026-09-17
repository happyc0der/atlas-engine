# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::tasks — a fixed pool of worker threads, and one way to use it.
#
# Created in M8 with the first parallel benchmark, which is the condition docs/DEFERRED.md set
# for creating it at all. Deliberately small: a pool and a parallel_for whose partitioning does
# not depend on how many workers there are, because M8's exit criterion is identical per-tick
# hashes across worker counts and the cheapest way to get that is to make the work identical.
atlas_add_module(tasks
    SOURCES
        src/worker_pool.cpp
    HEADERS
        include/atlas/tasks/worker_pool.hpp
    DEPENDS
        core
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
