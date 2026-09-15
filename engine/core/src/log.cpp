// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define ATLAS_ISATTY(fd) _isatty(fd)
#define ATLAS_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define ATLAS_ISATTY(fd) isatty(fd)
#define ATLAS_FILENO(f) fileno(f)
#endif

namespace atlas::log {
namespace {

/// The sink registry.
///
/// This is the one process-wide mutable object Atlas permits, justified in
/// docs/adr/0005-error-and-ownership-model.md: logging must be callable from anywhere,
/// including from failure paths that cannot be handed a context object.
///
/// A shared_mutex rather than a plain mutex: writes to the registry are rare (startup and
/// shutdown) while reads happen on every emitted record.
struct Registry {
    mutable std::shared_mutex mutex;
    std::vector<std::pair<std::uint32_t, std::unique_ptr<Sink>>> sinks;
    std::uint32_t next_id = 1;

    // Severity filters are read on every call including disabled ones, so they are atomics
    // outside the lock. A category override is rare, so that map stays under the mutex.
    std::atomic<Severity> min_severity{Severity::Info};
    std::vector<std::pair<std::string, Severity>> category_severities;
};

Registry& registry() {
    // Function-local static: constructed on first use, which avoids any dependence on
    // initialisation order between translation units.
    static Registry instance;
    return instance;
}

const char* severity_colour(Severity severity) noexcept {
    switch (severity) {
    case Severity::Trace: return "\033[37m";    // grey
    case Severity::Debug: return "\033[36m";    // cyan
    case Severity::Info: return "\033[0m";      // default
    case Severity::Warning: return "\033[33m";  // yellow
    case Severity::Error: return "\033[31m";    // red
    case Severity::Fatal: return "\033[1;31m";  // bold red
    }
    return "\033[0m";
}

/// Formats a record as one human-readable line, without a trailing newline.
///
/// Shape: "HH:MM:SS.mmm INF [core] message (file:line)".
/// The source location is included only in debug builds, where it is available and useful;
/// in release it is noise that also leaks paths.
std::string format_line(const Record& record, bool include_location) {
    const auto since_epoch = record.timestamp.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch - secs).count();

    const std::chrono::hh_mm_ss time_of_day{std::chrono::duration_cast<std::chrono::seconds>(secs) %
                                            std::chrono::days{1}};

    std::string line =
        std::format("{:02}:{:02}:{:02}.{:03} {} [{}] {}", time_of_day.hours().count(),
                    time_of_day.minutes().count(), time_of_day.seconds().count(), millis,
                    to_short_string(record.severity), record.category.name, record.message);

    if (include_location) {
        // Basename only: the absolute path of the build machine is not useful to a reader.
        std::string_view file{record.where.file_name()};
        if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
            file.remove_prefix(slash + 1);
        }
        line += std::format(" ({}:{})", file, record.where.line());
    }

    return line;
}

class ConsoleSink final : public Sink {
  public:
    ConsoleSink() : m_use_colour(ATLAS_ISATTY(ATLAS_FILENO(stderr)) != 0) {}

    void write(const Record& record) override {
        const std::string line = format_line(record, kIncludeLocation);

        const std::scoped_lock lock{m_mutex};
        if (m_use_colour) {
            std::fprintf(stderr, "%s%s\033[0m\n", severity_colour(record.severity), line.c_str());
        } else {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }

    void flush() override {
        const std::scoped_lock lock{m_mutex};
        std::fflush(stderr);
    }

  private:
#ifdef NDEBUG
    static constexpr bool kIncludeLocation = false;
#else
    static constexpr bool kIncludeLocation = true;
#endif

    std::mutex m_mutex;
    bool m_use_colour;
};

class FileSink final : public Sink {
  public:
    explicit FileSink(std::FILE* file) : m_file(file) {}

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&&) = delete;
    FileSink& operator=(FileSink&&) = delete;

    ~FileSink() override {
        if (m_file != nullptr) {
            std::fclose(m_file);
        }
    }

    void write(const Record& record) override {
        // Always include the location in a file: nobody is reading this live, and it is
        // what makes a post-mortem CI log useful.
        const std::string line = format_line(record, true);

        const std::scoped_lock lock{m_mutex};
        std::fprintf(m_file, "%s\n", line.c_str());
    }

    void flush() override {
        const std::scoped_lock lock{m_mutex};
        std::fflush(m_file);
    }

  private:
    std::mutex m_mutex;
    std::FILE* m_file;
};

}  // namespace

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::Trace: return "trace";
    case Severity::Debug: return "debug";
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    case Severity::Fatal: return "fatal";
    }
    return "unrecognised";
}

std::string_view to_short_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::Trace: return "TRC";
    case Severity::Debug: return "DBG";
    case Severity::Info: return "INF";
    case Severity::Warning: return "WRN";
    case Severity::Error: return "ERR";
    case Severity::Fatal: return "FTL";
    }
    return "???";
}

struct LogBuffer::Impl {
    mutable std::mutex mutex;
    std::deque<Entry> entries;
    std::size_t capacity;

    explicit Impl(std::size_t cap) : capacity(cap == 0 ? 1 : cap) {}
};

LogBuffer::LogBuffer(std::size_t capacity) : m_impl(std::make_unique<Impl>(capacity)) {}

LogBuffer::~LogBuffer() = default;

void LogBuffer::push(const Record& record) {
    const std::scoped_lock lock{m_impl->mutex};
    if (m_impl->entries.size() >= m_impl->capacity) {
        m_impl->entries.pop_front();
    }
    m_impl->entries.push_back(Entry{
        .timestamp = record.timestamp,
        .thread = record.thread,
        .category = std::string{record.category.name},
        .severity = record.severity,
        .message = std::string{record.message},
    });
}

std::vector<LogBuffer::Entry> LogBuffer::entries() const {
    const std::scoped_lock lock{m_impl->mutex};
    return {m_impl->entries.begin(), m_impl->entries.end()};
}

std::size_t LogBuffer::size() const {
    const std::scoped_lock lock{m_impl->mutex};
    return m_impl->entries.size();
}

std::size_t LogBuffer::capacity() const noexcept {
    return m_impl->capacity;
}

void LogBuffer::clear() {
    const std::scoped_lock lock{m_impl->mutex};
    m_impl->entries.clear();
}

namespace {

class BufferSink final : public Sink {
  public:
    explicit BufferSink(std::weak_ptr<LogBuffer> buffer) : m_buffer(std::move(buffer)) {}

    void write(const Record& record) override {
        // A weak reference: the buffer may outlive its reader, or the reader may drop it
        // while the sink is still registered. Neither is an error.
        if (const auto buffer = m_buffer.lock()) {
            buffer->push(record);
        }
    }

  private:
    std::weak_ptr<LogBuffer> m_buffer;
};

}  // namespace

std::unique_ptr<Sink> make_buffer_sink(std::weak_ptr<LogBuffer> buffer) {
    return std::make_unique<BufferSink>(std::move(buffer));
}

std::unique_ptr<Sink> make_console_sink() {
    return std::make_unique<ConsoleSink>();
}

Result<std::unique_ptr<Sink>> make_file_sink(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "ab");
    if (file == nullptr) {
        return std::unexpected(
            Error(ErrorCode::PermissionDenied, std::format("cannot open log file '{}'", path)));
    }
    return std::unique_ptr<Sink>{std::make_unique<FileSink>(file)};
}

std::uint32_t add_sink(std::unique_ptr<Sink> sink) {
    if (sink == nullptr) {
        return 0;
    }
    auto& reg = registry();
    const std::unique_lock lock{reg.mutex};
    const std::uint32_t id = reg.next_id++;
    reg.sinks.emplace_back(id, std::move(sink));
    return id;
}

void remove_sink(std::uint32_t id) {
    auto& reg = registry();
    const std::unique_lock lock{reg.mutex};
    std::erase_if(reg.sinks, [id](const auto& entry) { return entry.first == id; });
}

void shutdown() {
    auto& reg = registry();
    const std::unique_lock lock{reg.mutex};
    for (auto& [id, sink] : reg.sinks) {
        sink->flush();
    }
    reg.sinks.clear();
    reg.category_severities.clear();
}

void set_min_severity(Severity severity) {
    registry().min_severity.store(severity, std::memory_order_relaxed);
}

Severity min_severity() noexcept {
    return registry().min_severity.load(std::memory_order_relaxed);
}

void set_category_severity(Category category, Severity severity) {
    auto& reg = registry();
    const std::unique_lock lock{reg.mutex};
    for (auto& [name, value] : reg.category_severities) {
        if (name == category.name) {
            value = severity;
            return;
        }
    }
    reg.category_severities.emplace_back(std::string{category.name}, severity);
}

void clear_category_severity(Category category) {
    auto& reg = registry();
    const std::unique_lock lock{reg.mutex};
    std::erase_if(reg.category_severities,
                  [&](const auto& entry) { return entry.first == category.name; });
}

bool should_log(Category category, Severity severity) noexcept {
    auto& reg = registry();

    // Fast path: no category overrides, so one relaxed atomic load decides.
    {
        const std::shared_lock lock{reg.mutex};
        if (!reg.category_severities.empty()) {
            for (const auto& [name, threshold] : reg.category_severities) {
                if (name == category.name) {
                    return severity >= threshold;
                }
            }
        }
    }

    return severity >= reg.min_severity.load(std::memory_order_relaxed);
}

void write(Category category, Severity severity, std::string_view message,
           std::source_location where) {
    const Record record{
        .timestamp = std::chrono::system_clock::now(),
        .thread = std::this_thread::get_id(),
        .category = category,
        .severity = severity,
        .message = message,
        .where = where,
    };

    auto& reg = registry();
    const std::shared_lock lock{reg.mutex};
    for (const auto& [id, sink] : reg.sinks) {
        sink->write(record);
    }
}

}  // namespace atlas::log
