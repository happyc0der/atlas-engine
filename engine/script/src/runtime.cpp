// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/script/runtime.hpp>

#include <cstdlib>
#include <cstring>
#include <format>
#include <new>
#include <wasm_export.h>

namespace atlas::script {
namespace {

constexpr log::Category kScript{"script"};

/// The counted allocator's state.
///
/// File-static rather than a member, because WAMR's allocator hooks carry no user pointer
/// unless it is compiled with `WASM_MEM_ALLOC_WITH_USER_DATA`, and turning that on would be a
/// second build option to keep true across upgrades for no gain. It is safe here for a reason
/// that has to hold rather than merely be convenient: **at most one `Runtime` exists at a
/// time**, which `Runtime::create` asserts, so there is exactly one owner of these numbers for
/// as long as they mean anything. If a second runtime ever became possible this would have to
/// become per-runtime state, and the assertion is what stops that happening quietly.
struct Counters {
    std::size_t budget = 0;
    std::size_t in_use = 0;
    std::size_t peak = 0;
    std::size_t refused = 0;
    bool alive = false;
};

Counters& counters() {
    static Counters instance;
    return instance;
}

// This block *is* an allocator, so `malloc`, `free` and casting raw storage to a header are
// what it does rather than things it should be doing differently. Every other rule about manual
// memory still applies to every other file, and the suppression ends with these four functions
// rather than covering the whole translation unit.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-pro-type-reinterpret-cast)

/// Bytes kept in front of every allocation so `free` knows what to subtract.
///
/// WAMR's free hook is given only the pointer, so the size has to travel with it. Aligned to
/// `max_align_t` so what follows is aligned for anything the runtime puts there.
struct alignas(alignof(std::max_align_t)) BlockHeader {
    std::size_t size;
};

constexpr std::size_t kHeaderBytes = sizeof(BlockHeader);

void* counted_malloc(unsigned int size) {
    auto& c = counters();
    const auto wanted = static_cast<std::size_t>(size);
    // Refused rather than wrapped. A request large enough to overflow the addition is a request
    // that cannot be satisfied anyway, and computing a small total from a huge one is how a
    // bounds check becomes a heap overflow.
    if (wanted > SIZE_MAX - kHeaderBytes) {
        ++c.refused;
        return nullptr;
    }
    const std::size_t total = wanted + kHeaderBytes;
    if (c.in_use + total > c.budget) {
        ++c.refused;
        return nullptr;
    }

    void* raw = std::malloc(total);
    if (raw == nullptr) {
        ++c.refused;
        return nullptr;
    }
    auto* header = static_cast<BlockHeader*>(raw);
    header->size = total;
    c.in_use += total;
    c.peak = std::max(c.peak, c.in_use);
    return static_cast<void*>(static_cast<std::byte*>(raw) + kHeaderBytes);
}

void counted_free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(pointer) - kHeaderBytes;
    const std::size_t total = reinterpret_cast<BlockHeader*>(raw)->size;
    auto& c = counters();
    ATLAS_ASSERT_MSG(c.in_use >= total, "the script allocator freed more than it handed out");
    c.in_use -= total;
    std::free(static_cast<void*>(raw));
}

void* counted_realloc(void* pointer, unsigned int size) {
    if (pointer == nullptr) {
        return counted_malloc(size);
    }
    auto* raw = static_cast<std::byte*>(pointer) - kHeaderBytes;
    const std::size_t old_total = reinterpret_cast<BlockHeader*>(raw)->size;

    // Allocate, copy, free — rather than `std::realloc` — so the new block is counted against
    // the budget *before* the old one is released. Growing through the cap must fail with the
    // old block still valid, which is what the runtime expects from a failed realloc; freeing
    // first and failing second would hand back a pointer to memory that no longer exists.
    void* fresh = counted_malloc(size);
    if (fresh == nullptr) {
        return nullptr;
    }
    const std::size_t old_payload = old_total - kHeaderBytes;
    std::memcpy(fresh, pointer, std::min(old_payload, static_cast<std::size_t>(size)));
    counted_free(pointer);
    return fresh;
}

// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

struct Runtime::Impl {
    /// Whether this object is the one that will call `wasm_runtime_destroy`. A moved-from
    /// runtime is not, which is what stops a move tearing the runtime down.
    bool owns = false;
};

Runtime::Runtime() : m_impl(std::make_unique<Impl>()) {}

Result<Runtime> Runtime::create(std::size_t budget_bytes) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(!alive(), "a second script::Runtime while one is alive; it is process-wide");

    if (budget_bytes <= kHeaderBytes) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("a script runtime budget of {} bytes cannot hold "
                                                 "anything; the per-allocation header alone is {}",
                                                 budget_bytes, kHeaderBytes)));
    }

    auto& c = counters();
    c = Counters{};
    c.budget = budget_bytes;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) — see the next line.
    RuntimeInitArgs args{};
    // Set explicitly. `RuntimeInitArgs args{}` value-initialises `running_mode` to zero, and
    // `RunningMode` has no zero enumerator — so the default is a value the enumeration does not
    // define. It happens to work because WAMR treats zero as "unset", but a struct field whose
    // default is out of range is a trap for whoever reads this next, and clang-tidy is right to
    // say so.
    args.running_mode = Mode_Interp;
    args.mem_alloc_type = Alloc_With_Allocator;
    // WAMR takes its allocator hooks as `void*` rather than as typed function pointers, so
    // there is no conversion that is not a reinterpret_cast. The alternative is not a safer
    // cast, it is a different library.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(&counted_malloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(&counted_realloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(&counted_free);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    if (!wasm_runtime_full_init(&args)) {
        c = Counters{};
        return std::unexpected(
            Error(ErrorCode::ScriptRuntimeInitFailed,
                  "the WebAssembly runtime refused to initialise; no mod can be loaded"));
    }

    c.alive = true;
    Runtime runtime;
    runtime.m_impl->owns = true;
    ATLAS_LOG_INFO(kScript, "script runtime ready, budget {} KiB", budget_bytes / 1024);
    return runtime;
}

Runtime::~Runtime() {
    if (m_impl != nullptr && m_impl->owns) {
        // Not logged. A destructor that formats allocates, and this one runs during shutdown
        // where the log sinks may already be gone; CLAUDE.md says log around the lifetime
        // instead, which `create` does.
        wasm_runtime_destroy();
        counters() = Counters{};
    }
}

Runtime::Runtime(Runtime&& other) noexcept = default;

Runtime& Runtime::operator=(Runtime&& other) noexcept {
    if (this != &other) {
        if (m_impl != nullptr && m_impl->owns) {
            wasm_runtime_destroy();
            counters() = Counters{};
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

// Instance-shaped on purpose although the counters behind it are process-wide, because the
// counters are only process-wide for as long as WAMR's allocator hooks carry no user pointer.
// A static accessor would advertise that as part of the design; this way, giving each runtime
// its own counters later changes this file and no call site.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RuntimeStats Runtime::stats() const noexcept {
    const auto& c = counters();
    return RuntimeStats{
        .bytes_in_use = c.in_use,
        .peak_bytes_in_use = c.peak,
        .allocations_refused = c.refused,
    };
}

bool Runtime::alive() noexcept {
    return counters().alive;
}

}  // namespace atlas::script
