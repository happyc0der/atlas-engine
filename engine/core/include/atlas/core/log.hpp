// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Structured, categorised logging.
///
/// A log record carries a timestamp, a thread identifier, a category, a severity, a
/// formatted message, and (in debug builds) a source location. Records go to every
/// registered sink. The sink registry is the one process-wide mutable object Atlas
/// permits, and it is initialised and torn down explicitly by the composition root.
///
/// Messages are formatted with std::format and a compile-time checked format string, so a
/// mismatched argument is a build error rather than a runtime throw. std::print is
/// deliberately not used: the Ubuntu CI image's default libstdc++ lacks <print>.
///
/// Usage:
/// \code
///   ATLAS_LOG_INFO(log::category::kCore, "loaded {} assets in {} ms", count, elapsed);
/// \endcode

#include <atlas/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace atlas::log {

enum class Severity : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Fatal = 5,
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

/// Short, stable severity tag for log output: TRC, DBG, INF, WRN, ERR, FTL.
[[nodiscard]] std::string_view to_short_string(Severity severity) noexcept;

/// A log category.
///
/// Categories are interned string views with stable addresses, not an enum, so a module can
/// declare its own without editing a central list. Compare by name.
struct Category {
    std::string_view name;

    [[nodiscard]] friend bool operator==(const Category& a, const Category& b) noexcept {
        return a.name == b.name;
    }
};

namespace category {
inline constexpr Category kCore{"core"};
inline constexpr Category kApp{"app"};
}  // namespace category

/// One log record, as handed to a sink.
///
/// The message is already formatted. Sinks must not retain the string views beyond the
/// call; a sink that queues records must copy them.
struct Record {
    std::chrono::system_clock::time_point timestamp;
    std::thread::id thread;
    Category category;
    Severity severity;
    std::string_view message;
    std::source_location where;
};

/// A log destination.
///
/// write() may be called from any thread and must be internally thread-safe.
class Sink {
  public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;
    virtual ~Sink() = default;

    virtual void write(const Record& record) = 0;

    virtual void flush() {}
};

/// Writes human-readable lines to stderr, with colour when the stream is a terminal.
[[nodiscard]] std::unique_ptr<Sink> make_console_sink();

/// Appends human-readable lines to a file. Headless and CI runs need this, because there
/// is no terminal to read.
[[nodiscard]] Result<std::unique_ptr<Sink>> make_file_sink(const std::string& path);

/// Holds the most recent log records in memory.
///
/// The buffer is separate from the sink that feeds it: the sink registry owns the sink,
/// while whoever wants to read the records keeps a shared_ptr to the buffer. That is what
/// lets the future log console panel, and the tests, observe records without owning the
/// sink or reaching into the registry.
class LogBuffer {
  public:
    struct Entry {
        std::chrono::system_clock::time_point timestamp;
        std::thread::id thread;
        std::string category;
        Severity severity;
        std::string message;
    };

    explicit LogBuffer(std::size_t capacity);
    ~LogBuffer();

    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;
    LogBuffer(LogBuffer&&) = delete;
    LogBuffer& operator=(LogBuffer&&) = delete;

    /// Append a record. Thread-safe; called from whichever thread logged.
    void push(const Record& record);

    /// Records currently held, oldest first.
    [[nodiscard]] std::vector<Entry> entries() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// A sink that appends to `buffer`.
///
/// The sink holds a weak reference: if the buffer is destroyed first, the sink becomes a
/// no-op rather than a dangling write. Sinks and buffers have independent lifetimes
/// because the registry outlives most things that want to read from it.
[[nodiscard]] std::unique_ptr<Sink> make_buffer_sink(std::weak_ptr<LogBuffer> buffer);

/// Adds a sink. Returns an identifier that can be used to remove it again.
std::uint32_t add_sink(std::unique_ptr<Sink> sink);
void remove_sink(std::uint32_t id);

/// Removes every sink and flushes. Called by the composition root during shutdown.
void shutdown();

/// Minimum severity that will be emitted. Records below it are discarded cheaply, before
/// the message is formatted.
void set_min_severity(Severity severity);
[[nodiscard]] Severity min_severity() noexcept;

/// Per-category override of the minimum severity.
void set_category_severity(Category category, Severity severity);
void clear_category_severity(Category category);

/// Whether a record at this category and severity would be emitted.
[[nodiscard]] bool should_log(Category category, Severity severity) noexcept;

/// Emits a record. Prefer the macros below, which skip formatting when disabled.
void write(Category category, Severity severity, std::string_view message,
           std::source_location where);

namespace detail {

/// Formats and emits. The severity check has already passed: the ATLAS_LOG macro guards
/// the call so that a disabled record never evaluates its arguments.
template <typename... Args>
void log_formatted(Category category, Severity severity, std::source_location where,
                   std::format_string<Args...> fmt, Args&&... args) {
    write(category, severity, std::format(fmt, std::forward<Args>(args)...), where);
}

}  // namespace detail

}  // namespace atlas::log

/// Log macros.
///
/// These check the severity before formatting, so a disabled call costs one comparison and
/// does not touch its arguments. ATLAS_LOG_TRACE and ATLAS_LOG_DEBUG compile out entirely
/// in release builds.
#define ATLAS_LOG(category, severity, ...)                                                         \
    do {                                                                                           \
        if (::atlas::log::should_log((category), (severity))) {                                    \
            ::atlas::log::detail::log_formatted((category), (severity),                            \
                                                std::source_location::current(), __VA_ARGS__);     \
        }                                                                                          \
    } while (false)

#ifdef NDEBUG
#define ATLAS_LOG_TRACE(category, ...)                                                             \
    do {                                                                                           \
    } while (false)
#define ATLAS_LOG_DEBUG(category, ...)                                                             \
    do {                                                                                           \
    } while (false)
#else
#define ATLAS_LOG_TRACE(category, ...)                                                             \
    ATLAS_LOG(category, ::atlas::log::Severity::Trace, __VA_ARGS__)
#define ATLAS_LOG_DEBUG(category, ...)                                                             \
    ATLAS_LOG(category, ::atlas::log::Severity::Debug, __VA_ARGS__)
#endif

#define ATLAS_LOG_INFO(category, ...) ATLAS_LOG(category, ::atlas::log::Severity::Info, __VA_ARGS__)
#define ATLAS_LOG_WARN(category, ...)                                                              \
    ATLAS_LOG(category, ::atlas::log::Severity::Warning, __VA_ARGS__)
#define ATLAS_LOG_ERROR(category, ...)                                                             \
    ATLAS_LOG(category, ::atlas::log::Severity::Error, __VA_ARGS__)
#define ATLAS_LOG_FATAL(category, ...)                                                             \
    ATLAS_LOG(category, ::atlas::log::Severity::Fatal, __VA_ARGS__)
