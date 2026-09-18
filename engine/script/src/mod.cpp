// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/script/mod.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include <wasm_export.h>

namespace atlas::script {
namespace {

constexpr log::Category kScript{"script"};

/// WAMR writes its diagnosis here. Sized generously because the message is the only thing a mod
/// author gets, and a truncated one costs more than the stack it saves.
constexpr std::size_t kErrorBufferBytes = 256;

/// One page of WebAssembly linear memory, fixed by the specification.
constexpr std::size_t kWasmPageBytes = std::size_t{64} * 1024;

[[nodiscard]] std::string trimmed(std::span<const char> buffer) {
    const auto end = std::ranges::find(buffer, '\0');
    return {buffer.begin(), end};
}

/// The linear memory a module declares, read from its own bytes.
///
/// **Not from WAMR's introspection, which is wrong here.** `wasm_memory_type_get_init_page_count`
/// reports one page for a module that declares four, and `InstantiationArgs::max_memory_pages`
/// is ignored rather than honoured when it disagrees with the module — both observed on WAMR
/// 2.4.5 and both silent. A memory limit that quietly does not apply is worse than none, so the
/// number is taken from the one place that cannot be wrong about it: the module.
///
/// Called only after `wasm_runtime_load` has succeeded, so the section structure is already
/// validated and this walk is over bytes known to be well formed. Every read is still bounds
/// checked, because "already validated" is a claim about another library's code.
struct DeclaredMemory {
    std::uint64_t initial_pages = 0;
    bool has_maximum = false;
    std::uint64_t maximum_pages = 0;
    bool found = false;
};

[[nodiscard]] std::optional<std::uint64_t> read_uleb(std::span<const std::uint8_t> bytes,
                                                     std::size_t& at) {
    std::uint64_t value = 0;
    std::uint32_t shift = 0;
    while (at < bytes.size()) {
        const std::uint8_t byte = bytes[at++];
        if (shift >= 64) {
            return std::nullopt;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    return std::nullopt;
}

[[nodiscard]] DeclaredMemory declared_memory(std::span<const std::uint8_t> module_bytes) {
    constexpr std::size_t kHeaderBytes = 8;
    constexpr std::uint8_t kMemorySection = 5;

    DeclaredMemory out;
    if (module_bytes.size() < kHeaderBytes) {
        return out;
    }
    std::size_t at = kHeaderBytes;
    while (at < module_bytes.size()) {
        const std::uint8_t id = module_bytes[at++];
        const auto size = read_uleb(module_bytes, at);
        if (!size.has_value() || *size > module_bytes.size() - at) {
            return out;
        }
        const std::size_t payload_end = at + static_cast<std::size_t>(*size);
        if (id == kMemorySection) {
            const auto count = read_uleb(module_bytes, at);
            if (!count.has_value() || *count == 0) {
                return out;
            }
            // One memory per module in this version of the format; if that ever changes, the
            // first is the one every export refers to.
            if (at >= payload_end) {
                return out;
            }
            const std::uint8_t flags = module_bytes[at++];
            const auto initial = read_uleb(module_bytes, at);
            if (!initial.has_value()) {
                return out;
            }
            out.initial_pages = *initial;
            out.found = true;
            if ((flags & 0x01U) != 0) {
                const auto maximum = read_uleb(module_bytes, at);
                if (!maximum.has_value()) {
                    return out;
                }
                out.has_maximum = true;
                out.maximum_pages = *maximum;
            }
            return out;
        }
        at = payload_end;
    }
    return out;
}

}  // namespace

struct Mod::Impl {
    /// Our own copy of the module bytes, and it must stay alive for as long as the module does.
    ///
    /// **WAMR does not copy the bytecode**: the interpreter runs straight out of this buffer, so
    /// a module loaded from a caller's span would read freed memory the moment that span's owner
    /// went away. Owning it here makes the lifetime a property of this object rather than a
    /// rule callers have to know.
    std::vector<std::uint8_t> bytes;

    wasm_module_t module = nullptr;
    wasm_module_inst_t instance = nullptr;
    wasm_exec_env_t exec_env = nullptr;

    std::string name;
    std::string disabled_reason;
    ModLimits limits;

    void tear_down() {
        if (exec_env != nullptr) {
            wasm_runtime_destroy_exec_env(exec_env);
            exec_env = nullptr;
        }
        if (instance != nullptr) {
            wasm_runtime_deinstantiate(instance);
            instance = nullptr;
        }
        if (module != nullptr) {
            wasm_runtime_unload(module);
            module = nullptr;
        }
    }

    /// Disable this mod for the rest of the session and say why, exactly once.
    ///
    /// The instance is torn down here rather than left disabled-but-alive, so a disabled mod
    /// costs no linear memory: a mod that has gone wrong holding sixteen megabytes until
    /// shutdown is a leak with an explanation attached.
    void disable(std::string why) {
        if (!disabled_reason.empty()) {
            return;
        }
        disabled_reason = std::move(why);
        ATLAS_LOG_ERROR(kScript, "mod '{}' disabled for the rest of this session: {}", name,
                        disabled_reason);
        tear_down();
    }

    /// Call one exported function with the instruction budget armed, and translate whatever
    /// went wrong into a disabled mod.
    ///
    /// The budget is re-armed on every call rather than once at load: it is a limit **per
    /// tick**, and a counter that carried over would disable a mod for work it did several
    /// ticks ago.
    [[nodiscard]] Status call_export(std::string_view export_name, std::span<std::uint32_t> argv,
                                     std::uint32_t argc) {
        wasm_function_inst_t function =
            wasm_runtime_lookup_function(instance, std::string(export_name).c_str());
        if (function == nullptr) {
            disable(std::format("'{}' is not callable", export_name));
            return std::unexpected(
                Error(ErrorCode::ModExportMissing,
                      std::format("mod '{}' has no callable '{}'", name, export_name)));
        }

        wasm_runtime_set_instruction_count_limit(
            exec_env, static_cast<int>(limits.max_instructions_per_tick));

        const bool ok_call = wasm_runtime_call_wasm(exec_env, function, argc, argv.data());
        if (!ok_call) {
            const char* exception = wasm_runtime_get_exception(instance);
            const std::string why = exception != nullptr ? exception : "no diagnosis";
            // WAMR reports an exhausted instruction budget as an ordinary trap with a distinctive
            // message. Separated here because the two mean different things to whoever reads the
            // log: a trap is a bug in the mod, and an exhausted budget may be a mod that is simply
            // too ambitious for the ceiling.
            const bool metered_out = why.contains("instruction limit exceeded");
            disable(std::format("{} in '{}'", why, export_name));
            return std::unexpected(
                Error(metered_out ? ErrorCode::ModBudgetExhausted : ErrorCode::ModTrapped,
                      std::format("mod '{}' stopped in '{}': {}", name, export_name, why)));
        }
        return ok();
    }
};

Mod::Mod() : m_impl(std::make_unique<Impl>()) {}

Mod::~Mod() {
    if (m_impl != nullptr) {
        m_impl->tear_down();
    }
}

Mod::Mod(Mod&&) noexcept = default;

Mod& Mod::operator=(Mod&& other) noexcept {
    if (this != &other) {
        if (m_impl != nullptr) {
            m_impl->tear_down();
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Result<Mod> Mod::load(Runtime& runtime, std::span<const std::byte> bytes,
                      std::string_view debug_name, const ModLimits& limits) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ASSERT_MSG(Runtime::alive(), "loading a mod without a live script::Runtime");
    (void)runtime;  // Taken by reference to make the lifetime relationship explicit in the type.

    // Size first, before anything looks at the content. Everything below this line is work done
    // on behalf of whoever supplied the bytes, and the point of a cap is not to do it.
    if (bytes.size() > limits.max_module_bytes) {
        return std::unexpected(Error(
            ErrorCode::OutOfRange, std::format("mod '{}' is {} bytes, over the {} byte limit",
                                               debug_name, bytes.size(), limits.max_module_bytes)));
    }
    if (bytes.empty()) {
        return std::unexpected(
            Error(ErrorCode::ModInvalid, std::format("mod '{}' is empty", debug_name)));
    }

    Mod mod;
    auto& impl = *mod.m_impl;
    impl.name = std::string(debug_name);
    impl.limits = limits;
    impl.bytes.resize(bytes.size());
    std::memcpy(impl.bytes.data(), bytes.data(), bytes.size());

    std::array<char, kErrorBufferBytes> error{};
    impl.module =
        wasm_runtime_load(impl.bytes.data(), static_cast<std::uint32_t>(impl.bytes.size()),
                          error.data(), static_cast<std::uint32_t>(error.size()));
    if (impl.module == nullptr) {
        return std::unexpected(Error(
            ErrorCode::ModInvalid, std::format("mod '{}' is not a module this runtime accepts: {}",
                                               debug_name, trimmed(error))));
    }

    // Imports, before instantiation. The import list is the whole of a guest's authority, so a
    // module asking for something it was not offered is refused entirely rather than
    // instantiated with that one import missing — "missing" is a state a guest can probe and
    // work around, "refused" is not.
    const std::int32_t imports = wasm_runtime_get_import_count(impl.module);
    for (std::int32_t i = 0; i < imports; ++i) {
        wasm_import_t import{};
        wasm_runtime_get_import_type(impl.module, i, &import);
        const std::string_view module_name =
            import.module_name != nullptr ? import.module_name : "";
        const std::string_view field = import.name != nullptr ? import.name : "";
        if (module_name != kImportModule) {
            return std::unexpected(Error(
                ErrorCode::ModImportRefused,
                std::format("mod '{}' imports '{}.{}'; the only imports a mod may have are from "
                            "'{}', and everything else is authority it was not given",
                            debug_name, module_name, field, kImportModule)));
        }
        // Every `atlas.*` import is refused too, for now. The host provides none yet, and a
        // module importing one would otherwise instantiate with an unresolved function it could
        // call. This becomes a lookup against the host's table when that table exists.
        return std::unexpected(
            Error(ErrorCode::ModImportRefused,
                  std::format("mod '{}' imports '{}.{}', which this host does not provide",
                              debug_name, module_name, field)));
    }

    // Exports. Collected as a set so the message names everything missing at once: somebody
    // porting a mod wants the whole list rather than the first item on it.
    constexpr std::array<std::string_view, 4> kRequired{kExportMemory, kExportInit, kExportTick,
                                                        kExportShutdown};
    std::array<bool, kRequired.size()> found{};

    const std::int32_t exports = wasm_runtime_get_export_count(impl.module);
    for (std::int32_t i = 0; i < exports; ++i) {
        wasm_export_t item{};
        wasm_runtime_get_export_type(impl.module, i, &item);
        const std::string_view name = item.name != nullptr ? item.name : "";
        for (std::size_t r = 0; r < kRequired.size(); ++r) {
            if (name == kRequired[r]) {
                found[r] = true;
            }
        }
    }

    std::string missing;
    for (std::size_t r = 0; r < kRequired.size(); ++r) {
        if (!found[r]) {
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += kRequired[r];
        }
    }
    if (!missing.empty()) {
        return std::unexpected(
            Error(ErrorCode::ModExportMissing,
                  std::format("mod '{}' does not export: {}", debug_name, missing)));
    }

    // Linear memory, refused here and by our own reading of the module rather than at
    // instantiation. Both the initial size and the declared maximum are checked: a module that
    // starts inside the limit and may grow past it is a module that will.
    const std::uint64_t max_pages = limits.max_linear_memory_bytes / kWasmPageBytes;
    const auto memory = declared_memory(impl.bytes);
    const auto refuse_pages = [&](std::string_view which, std::uint64_t pages) {
        return Error(ErrorCode::OutOfRange,
                     std::format("mod '{}' declares {} linear memory of {} pages ({} KiB); the "
                                 "limit is {} pages ({} KiB)",
                                 debug_name, which, pages, pages * kWasmPageBytes / 1024, max_pages,
                                 limits.max_linear_memory_bytes / 1024));
    };
    if (memory.initial_pages > max_pages) {
        return std::unexpected(refuse_pages("an initial", memory.initial_pages));
    }
    // A module with no declared maximum could ask to grow without one. Refused rather than
    // capped, because a mod that does not say how much memory it wants has not been written
    // with a limit in mind, and silently capping it would show up later as a failed `memory.grow`
    // nobody can explain.
    if (memory.found && !memory.has_maximum) {
        return std::unexpected(Error(
            ErrorCode::ModInvalid,
            std::format("mod '{}' declares linear memory with no maximum; a mod must say how far "
                        "it may grow so the limit can be checked before it runs",
                        debug_name)));
    }
    if (memory.has_maximum && memory.maximum_pages > max_pages) {
        return std::unexpected(refuse_pages("a maximum", memory.maximum_pages));
    }

    InstantiationArgs args{};
    args.default_stack_size = static_cast<std::uint32_t>(limits.max_stack_bytes);
    // No host-managed heap: a guest that wants an allocator brings its own inside linear memory.
    // WAMR's app heap is a second allocation this cap would not see.
    args.host_managed_heap_size = 0;
    // The module's own declared maximum, which the check above has already proved is within
    // our limit. Not our limit itself: WAMR refuses an override larger than the module's
    // maximum and carries on regardless, so passing the bigger number would print a complaint
    // on every ordinary load and change nothing.
    args.max_memory_pages = static_cast<std::uint32_t>(memory.maximum_pages);

    impl.instance = wasm_runtime_instantiate_ex(impl.module, &args, error.data(),
                                                static_cast<std::uint32_t>(error.size()));
    if (impl.instance == nullptr) {
        return std::unexpected(
            Error(ErrorCode::Exhausted, std::format("mod '{}' could not be instantiated: {}",
                                                    debug_name, trimmed(error))));
    }

    impl.exec_env = wasm_runtime_create_exec_env(
        impl.instance, static_cast<std::uint32_t>(limits.max_stack_bytes));
    if (impl.exec_env == nullptr) {
        return std::unexpected(
            Error(ErrorCode::Exhausted,
                  std::format("mod '{}' could not be given an execution environment; the runtime "
                              "budget is spent",
                              debug_name)));
    }

    ATLAS_LOG_INFO(kScript, "mod '{}' loaded: {} bytes, {} page(s) of memory", debug_name,
                   bytes.size(), memory.initial_pages);
    return mod;
}

Status Mod::init() {
    ATLAS_ASSERT_MAIN_THREAD();
    if (disabled()) {
        return ok();
    }

    std::array<std::uint32_t, 1> argv{};
    if (auto status = m_impl->call_export(kExportInit, argv, 0); !status) {
        return status;
    }

    // A non-zero return is the mod saying it cannot run, and it is believed rather than argued
    // with. Disabling here rather than letting it tick means a mod that knows it is misconfigured
    // does not get to produce commands anyway.
    const auto result = static_cast<std::int32_t>(argv[0]);
    if (result != 0) {
        m_impl->disable(std::format("mod_init returned {}", result));
        return std::unexpected(
            Error(ErrorCode::ModInvalid,
                  std::format("mod '{}' refused to initialise: mod_init returned {}", m_impl->name,
                              result)));
    }
    return ok();
}

Status Mod::tick(std::int64_t tick_index) {
    ATLAS_ASSERT_MAIN_THREAD();
    // A disabled mod is not an error once per tick for the rest of the session. It is a mod that
    // is not there, and the log line that said so was written when it was disabled.
    if (disabled()) {
        return ok();
    }

    // One i64 argument occupies two slots in WAMR's argv, low word first.
    std::array<std::uint32_t, 2> argv{
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(tick_index) & 0xFFFF'FFFFULL),
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(tick_index) >> 32U),
    };
    return m_impl->call_export(kExportTick, argv, 2);
}

bool Mod::disabled() const noexcept {
    return !m_impl->disabled_reason.empty();
}

std::string_view Mod::disabled_because() const noexcept {
    return m_impl->disabled_reason;
}

std::string_view Mod::name() const noexcept {
    return m_impl->name;
}

}  // namespace atlas::script
