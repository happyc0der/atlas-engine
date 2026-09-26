// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/script/atlas_mod.h>
#include <atlas/script/mod.hpp>
#include <atlas/script/mod_host.hpp>

#include "host_imports.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <wasm_export.h>

namespace atlas::script {
namespace {

constexpr log::Category kScript{"script"};

/// The context for the call in flight, or null outside one.
[[nodiscard]] HostCall* current(wasm_exec_env_t exec_env) {
    return static_cast<HostCall*>(wasm_runtime_get_user_data(exec_env));
}

// The imports themselves. Each one refuses rather than trapping: a mod that asks for something
// out of range gets a negative number back and carries on, because trapping would turn every
// mistake in a mod into a disabled mod, and the failure policy is meant for a mod that has gone
// wrong rather than one that asked a question with the wrong argument.

std::int64_t native_tick(wasm_exec_env_t exec_env) {
    const auto* call = current(exec_env);
    return call != nullptr ? static_cast<std::int64_t>(call->tick) : ATLAS_ERR_REFUSED;
}

std::int32_t native_command_type(wasm_exec_env_t exec_env, const char* name,
                                 std::uint32_t name_len) {
    const auto* call = current(exec_env);
    if (call == nullptr || call->queue == nullptr || name == nullptr) {
        return ATLAS_ERR_REFUSED;
    }
    const auto type = sim::command_type(std::string_view(name, name_len));
    // Zero for a type nobody handles, so a mod written against a game that is not running
    // submits nothing rather than something wrong. Checked here as well as at submit because
    // this is where a mod can do something about the answer.
    return call->queue->has_handler(type) ? static_cast<std::int32_t>(type) : 0;
}

std::int32_t native_submit(wasm_exec_env_t exec_env, std::int32_t type, const void* payload,
                           std::uint32_t payload_len) {
    auto* call = current(exec_env);
    if (call == nullptr || call->queue == nullptr) {
        return ATLAS_ERR_REFUSED;
    }
    if (call->commands_left == 0) {
        ++call->refused;
        return ATLAS_ERR_EXHAUSTED;
    }
    if (payload == nullptr && payload_len != 0) {
        ++call->refused;
        return ATLAS_ERR_RANGE;
    }

    const auto* bytes = static_cast<const std::byte*>(payload);
    const std::span<const std::byte> span(bytes, payload_len);

    // The identity is the host's, never the guest's: `atlas_submit` has no source parameter, so
    // there is nothing here to validate and nothing a mod could have got wrong.
    if (const auto status = call->queue->submit(
            call->target, call->source, sim::CommandType{static_cast<std::uint32_t>(type)}, span);
        !status) {
        ++call->refused;
        return ATLAS_ERR_REFUSED;
    }

    --call->commands_left;
    ++call->submitted;
    call->highest_target = std::max(call->highest_target, call->target);
    return 0;
}

std::int64_t native_random(wasm_exec_env_t exec_env, std::int32_t stream, std::int64_t bound) {
    auto* call = current(exec_env);
    if (call == nullptr) {
        return ATLAS_ERR_REFUSED;
    }
    if (bound <= 0) {
        return ATLAS_ERR_RANGE;
    }

    const auto key = static_cast<std::uint32_t>(stream);
    auto at = call->streams.find(key);
    if (at == call->streams.end()) {
        // Keyed by the mod's name as well as the guest's stream number, so two mods asking for
        // stream 0 do not draw the same sequence. The name is identical on every peer, which is
        // what makes this reproducible rather than merely unique.
        const auto mixed =
            sim::StreamId{static_cast<std::uint32_t>(sim::stream_id(call->mod_name)) ^ key};
        at = call->streams.emplace(key, sim::RngStream{call->seed, mixed, call->tick}).first;
    }
    return static_cast<std::int64_t>(at->second.next_below(static_cast<std::uint64_t>(bound)));
}

void native_log(wasm_exec_env_t exec_env, std::int32_t level, const char* text,
                std::uint32_t text_len) {
    auto* call = current(exec_env);
    if (call == nullptr || text == nullptr) {
        return;
    }
    if (text_len > call->log_bytes_left) {
        ++call->log_dropped;
        return;
    }
    call->log_bytes_left -= text_len;

    const std::string_view line(text, text_len);
    switch (level) {
    case ATLAS_MOD_LOG_DEBUG: ATLAS_LOG_DEBUG(kScript, "[{}] {}", call->mod_name, line); break;
    case ATLAS_MOD_LOG_WARN: ATLAS_LOG_WARN(kScript, "[{}] {}", call->mod_name, line); break;
    case ATLAS_MOD_LOG_ERROR: ATLAS_LOG_ERROR(kScript, "[{}] {}", call->mod_name, line); break;
    case ATLAS_MOD_LOG_INFO:
    default: ATLAS_LOG_INFO(kScript, "[{}] {}", call->mod_name, line); break;
    }
}

std::int32_t native_view_count(wasm_exec_env_t exec_env) {
    const auto* call = current(exec_env);
    return call != nullptr ? static_cast<std::int32_t>(call->views.size()) : ATLAS_ERR_REFUSED;
}

std::int32_t native_view_size(wasm_exec_env_t exec_env, std::int32_t view) {
    const auto* call = current(exec_env);
    if (call == nullptr || view < 0 || static_cast<std::size_t>(view) >= call->views.size()) {
        return ATLAS_ERR_RANGE;
    }
    return static_cast<std::int32_t>(call->views[static_cast<std::size_t>(view)].bytes.size());
}

std::int32_t native_view_read(wasm_exec_env_t exec_env, std::int32_t view, std::int32_t offset,
                              void* dest, std::uint32_t len) {
    const auto* call = current(exec_env);
    if (call == nullptr || dest == nullptr || offset < 0 || view < 0 ||
        static_cast<std::size_t>(view) >= call->views.size()) {
        return ATLAS_ERR_RANGE;
    }
    const auto bytes = call->views[static_cast<std::size_t>(view)].bytes;
    const auto start = static_cast<std::size_t>(offset);
    if (start > bytes.size()) {
        return ATLAS_ERR_RANGE;
    }
    // Short reads at the end rather than a refusal: a mod walking a view to its end should not
    // have to know the size to the byte, and `dest` is already bounds-checked by the runtime
    // because the signature declares it as a buffer with a length.
    const auto count = std::min(static_cast<std::size_t>(len), bytes.size() - start);
    std::memcpy(dest, bytes.data() + start, count);
    return static_cast<std::int32_t>(count);
}

/// Whether a key suffix is one a mod may say: short, lowercase, and no empty segment.
[[nodiscard]] bool valid_key_suffix(std::string_view suffix) noexcept {
    if (suffix.empty() || suffix.size() > ATLAS_MOD_MAX_SAY_KEY || suffix.front() == '.' ||
        suffix.back() == '.' || suffix.contains("..")) {
        return false;
    }
    return std::ranges::all_of(suffix, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

std::int32_t native_say(wasm_exec_env_t exec_env, const char* key, std::uint32_t key_len,
                        std::int32_t args_offset, std::int32_t arg_count) {
    auto* call = current(exec_env);
    if (call == nullptr || key == nullptr || call->messages == nullptr) {
        return ATLAS_ERR_REFUSED;
    }
    // Everything that can be judged without touching memory is judged first, so a malformed
    // call is a refusal the mod can see rather than a trap that disables it.
    const std::string_view suffix(key, key_len);
    if (!valid_key_suffix(suffix) || arg_count < 0 || arg_count > ATLAS_MOD_MAX_SAY_ARGS) {
        return ATLAS_ERR_RANGE;
    }
    if (call->messages_left == 0) {
        ++call->messages_dropped;
        return ATLAS_ERR_EXHAUSTED;
    }

    ModMessage message;
    message.tick = call->tick;
    message.arg_count = static_cast<std::uint8_t>(arg_count);
    if (arg_count > 0) {
        // Validated here rather than by the signature, because the count is of integers and the
        // signature's buffer form counts bytes. An address outside the mod's memory raises the
        // runtime's own out-of-bounds exception, and the mod traps — ADR-0015's rule for a bad
        // pointer, applied the same way the signature would have applied it.
        const auto bytes = static_cast<std::uint64_t>(arg_count) * sizeof(std::int64_t);
        wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
        if (!wasm_runtime_validate_app_addr(
                instance, static_cast<std::uint64_t>(static_cast<std::uint32_t>(args_offset)),
                bytes)) {
            return ATLAS_ERR_RANGE;
        }
        const void* native = wasm_runtime_addr_app_to_native(
            instance, static_cast<std::uint64_t>(static_cast<std::uint32_t>(args_offset)));
        // memcpy rather than a cast: a guest's integers need not be aligned for the host.
        std::memcpy(message.args.data(), native, bytes);
    }

    --call->messages_left;
    if (call->messages->size() >= call->queue_capacity) {
        ++call->messages_dropped;
        return ATLAS_ERR_EXHAUSTED;
    }
    message.key = std::format("{}{}", call->key_prefix, suffix);
    call->messages->push_back(std::move(message));
    ++call->said;
    return 0;
}

/// A host clock, offered only when something explicitly asked for it.
///
/// **This exists so that a test can prove why the interface has no clock.** Under lockstep two
/// peers read different values here, decide differently, and diverge at the first hash check —
/// which is the demonstration ADR-0015 wants, and the test that fails if anybody later adds a
/// clock to `atlas_mod.h` for real. It is deliberately not declared in that header: a mod
/// written against the documented interface cannot reach it even by accident, because the only
/// way to import it is to know the name and to be run by a host that opted in.
std::int64_t native_debug_clock_ns(wasm_exec_env_t exec_env) {
    const auto* call = current(exec_env);
    if (call == nullptr) {
        return ATLAS_ERR_REFUSED;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Whether the live runtime offered the clock. Set once by `register_host_imports`.
///
/// File-static for the same reason the allocator's counters are: at most one `Runtime` exists
/// at a time, and both sides of this question — what was registered and what the loader will
/// accept — have to give the same answer.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool g_debug_imports = false;

/// The table, in the order `atlas_mod.h` declares them, with the unsafe one last.
///
/// **Const, and never handed to WAMR.** `wasm_runtime_register_natives` sorts the array it is
/// given *in place* (`wasm_native.c`, `qsort`) and keeps the pointer for as long as the imports
/// are registered. Until M26 this table was the array WAMR sorted, and the safe set was its first
/// nine entries. A runtime created with the clock sorted all ten alphabetically, and the next
/// runtime in the same process then registered, and the loader then permitted, a "first nine"
/// that contained the clock and lacked `atlas_view_size`. Every program creates one runtime, so
/// none shipped with it; tests create several in random order, and one run in three failed. The
/// registered arrays below are copies, one per kind of runtime, refreshed on every registration,
/// and what the loader permits is read from this one, which nothing can reorder.
///
/// The casts are unavoidable — `NativeSymbol::func_ptr` is a `void*`, so there is no conversion
/// from a function pointer that is not a reinterpret_cast. The alternative is a different runtime.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
const std::array<NativeSymbol, 10> kImports{{
    {"atlas_tick", reinterpret_cast<void*>(&native_tick), "()I", nullptr},
    {"atlas_command_type", reinterpret_cast<void*>(&native_command_type), "(*~)i", nullptr},
    {"atlas_submit", reinterpret_cast<void*>(&native_submit), "(i*~)i", nullptr},
    {"atlas_random", reinterpret_cast<void*>(&native_random), "(iI)I", nullptr},
    {"atlas_log", reinterpret_cast<void*>(&native_log), "(i*~)", nullptr},
    {"atlas_view_count", reinterpret_cast<void*>(&native_view_count), "()i", nullptr},
    {"atlas_view_size", reinterpret_cast<void*>(&native_view_size), "(i)i", nullptr},
    {"atlas_view_read", reinterpret_cast<void*>(&native_view_read), "(ii*~)i", nullptr},
    // The key is a buffer the runtime checks; the arguments are a count of integers, which the
    // signature's buffer form cannot express, so they arrive as an offset and are checked by
    // the import itself (ADR-0021).
    {"atlas_say", reinterpret_cast<void*>(&native_say), "(*~ii)i", nullptr},
    // Last on purpose: registering the first N-1 is how the clock is withheld, so it must be
    // the one on the end. A new import goes **before** this line.
    {"atlas_debug_clock_ns", reinterpret_cast<void*>(&native_debug_clock_ns), "()I", nullptr},
}};

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

/// How many of the table are safe to offer. Everything but the clock.
constexpr std::size_t kSafeImportCount = 9;

/// What WAMR is given and may sort: a copy for a runtime without the clock, and one with it.
/// Mutable and long-lived because WAMR keeps the pointer while the imports are registered.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::array<NativeSymbol, kSafeImportCount> g_safe_registered{};
std::array<NativeSymbol, kImports.size()> g_all_registered{};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

std::span<const std::string_view> host_import_names() {
    // Derived from the same table the runtime is given, so the two cannot disagree. Sorted
    // because `Mod::load` binary-searches it and because a sorted list is a readable one.
    //
    // Two lists rather than one, chosen by the same flag that chose how many to register: the
    // loader must refuse an import the runtime did not register, and accept one it did, and
    // computing both from the same array is what stops those two answers drifting apart.
    static const std::vector<std::string_view> kSafeNames = [] {
        std::vector<std::string_view> out;
        out.reserve(kSafeImportCount);
        for (const auto& symbol : std::span(kImports).first(kSafeImportCount)) {
            out.emplace_back(symbol.symbol);
        }
        std::ranges::sort(out);
        return out;
    }();
    static const std::vector<std::string_view> kAllNames = [] {
        std::vector<std::string_view> out;
        out.reserve(kImports.size());
        for (const auto& symbol : kImports) {
            out.emplace_back(symbol.symbol);
        }
        std::ranges::sort(out);
        return out;
    }();
    return g_debug_imports ? std::span<const std::string_view>(kAllNames)
                           : std::span<const std::string_view>(kSafeNames);
}

bool register_host_imports(bool with_debug) {
    g_debug_imports = with_debug;
    // The literal, not `std::string(kImportModule).c_str()`, which is what this was first and
    // is a dangling pointer: WAMR **keeps** the module-name pointer for as long as the imports
    // are registered, and a temporary string dies at the end of the statement. The symptom was
    // every import failing to link with the registration reporting success, which is a long way
    // from the cause.
    //
    // A fresh copy from the const table every time, into the array for this kind of runtime, so
    // whatever order an earlier registration left behind cannot decide what this one offers.
    if (with_debug) {
        std::ranges::copy(kImports, g_all_registered.begin());
        return wasm_runtime_register_natives(ATLAS_IMPORT_MODULE, g_all_registered.data(),
                                             static_cast<std::uint32_t>(g_all_registered.size()));
    }
    std::ranges::copy(std::span(kImports).first(kSafeImportCount), g_safe_registered.begin());
    return wasm_runtime_register_natives(ATLAS_IMPORT_MODULE, g_safe_registered.data(),
                                         static_cast<std::uint32_t>(g_safe_registered.size()));
}

struct ModHost::Impl {
    Mod mod;
    sim::SourceId source = sim::SourceId::Local;
    ModHostConfig config;
    std::vector<ModView> views;
    ModHostStats stats;
    bool started = false;
    std::vector<ModMessage> messages;
    std::string key_prefix;

    explicit Impl(Mod loaded) : mod(std::move(loaded)) {}
};

ModHost::ModHost() = default;

ModHost::~ModHost() = default;

Result<std::unique_ptr<ModHost>> ModHost::create(Runtime& runtime, std::uint32_t mod_index,
                                                 std::span<const std::byte> bytes,
                                                 std::string_view name, const ModHostConfig& config,
                                                 const ModLimits& limits) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (config.input_delay == 0) {
        // A command stamped for the tick being decided would be drained before the mod that
        // produced it had finished producing, so it would apply on some peers and not others
        // depending on nothing at all. Refused rather than clamped: a caller asking for zero
        // has misunderstood something, and silently giving it one would hide that.
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("mod '{}' was given an input delay of zero; a mod's commands must be "
                              "stamped for a later tick than the one it is deciding",
                              name)));
    }
    if ((mod_index & sim::kModSourceBit) != 0) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("mod index {} for '{}' overlaps the bit that marks a mod source",
                              mod_index, name)));
    }

    auto loaded = Mod::load(runtime, bytes, name, limits);
    if (!loaded) {
        return std::unexpected(std::move(loaded).error());
    }

    auto host = std::unique_ptr<ModHost>(new ModHost());
    host->m_impl = std::make_unique<Impl>(*std::move(loaded));
    host->m_impl->source = sim::mod_source(mod_index);
    host->m_impl->config = config;
    host->m_impl->key_prefix = mod_key_prefix(host->m_impl->mod.name());
    return host;
}

void ModHost::set_views(std::span<const ModView> views) {
    m_impl->views.assign(views.begin(), views.end());
}

sim::SourceId ModHost::id() const noexcept {
    return m_impl->source;
}

Status ModHost::start() {
    ATLAS_ASSERT_MAIN_THREAD();
    m_impl->started = true;
    return m_impl->mod.init();
}

Result<sim::PollReport> ModHost::poll(Tick now, sim::CommandQueue& queue, sim::TurnGate& turns) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(m_impl->started, "a mod host polled before start()");

    // Deliberately untouched. A mod takes no turns: every peer runs the same mods and each
    // produces the same commands locally, so there is nothing to announce and nothing to wait
    // for. Named rather than left as an unused parameter, because "did somebody forget to mark
    // the gate?" is exactly the question a reader will have here.
    (void)turns;

    sim::PollReport report;
    if (m_impl->mod.disabled()) {
        return report;
    }

    HostCall call;
    call.tick = now;
    call.target = now + m_impl->config.input_delay;
    call.source = m_impl->source;
    call.queue = &queue;
    call.views = m_impl->views;
    call.seed = m_impl->config.seed;
    call.mod_name = m_impl->mod.name();
    call.commands_left = m_impl->config.max_commands_per_tick;
    call.log_bytes_left = m_impl->config.max_log_bytes_per_tick;
    call.messages = &m_impl->messages;
    call.key_prefix = m_impl->key_prefix;
    call.messages_left = m_impl->config.max_messages_per_tick;
    call.queue_capacity = m_impl->config.max_queued_messages;

    m_impl->mod.set_call_context(&call);
    const auto ran = m_impl->mod.tick(static_cast<std::int64_t>(now));
    m_impl->mod.set_call_context(nullptr);

    report.commands_submitted = static_cast<std::size_t>(call.submitted);
    report.commands_refused = static_cast<std::size_t>(call.refused);
    report.highest_target = call.highest_target;
    m_impl->stats.commands_submitted += call.submitted;
    m_impl->stats.commands_refused += call.refused;
    m_impl->stats.log_lines_dropped += call.log_dropped;
    m_impl->stats.messages_said += call.said;
    m_impl->stats.messages_dropped += call.messages_dropped;
    ++m_impl->stats.ticks_run;

    if (!ran) {
        // The mod is already disabled by the time this returns, and that is the whole of the
        // consequence. Reported as a closed source rather than as an error, because an error
        // here would stop the driver polling everything else — which is exactly what one bad
        // mod must not be able to do.
        ATLAS_LOG_WARN(kScript, "mod '{}' produced nothing further: {}", m_impl->mod.name(),
                       ran.error());
        report.closed = true;
    }
    return report;
}

bool ModHost::disabled() const noexcept {
    return m_impl->mod.disabled();
}

std::string_view ModHost::disabled_because() const noexcept {
    return m_impl->mod.disabled_because();
}

std::string_view ModHost::name() const noexcept {
    return m_impl->mod.name();
}

ModHostStats ModHost::stats() const noexcept {
    return m_impl->stats;
}

std::vector<ModMessage> ModHost::take_messages() {
    std::vector<ModMessage> out;
    out.swap(m_impl->messages);
    return out;
}

std::string mod_key_prefix(std::string_view mod_name) {
    constexpr std::string_view kExtension = ".wasm";
    std::string_view stem = mod_name;
    if (stem.size() > kExtension.size() && stem.ends_with(kExtension)) {
        stem.remove_suffix(kExtension.size());
    }
    return std::format("mod.{}.", stem);
}

Status check_mod_table(const assets::ImportedStringTable& table, std::string_view mod_name) {
    const std::string prefix = mod_key_prefix(mod_name);
    for (const auto& [key, value] : table.strings) {
        if (!key.starts_with(prefix) || key.size() == prefix.size()) {
            return std::unexpected(Error(
                ErrorCode::PermissionDenied,
                std::format("mod '{}' may add strings only under '{}', and its table names '{}'",
                            mod_name, prefix, key)));
        }
    }
    return {};
}

}  // namespace atlas::script
