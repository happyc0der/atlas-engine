// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

// Deliberately not named `log`. The C mathematics library declares `log` in the global
// namespace, and on MSVC the standard headers pull it in, so an alias by that name is
// ambiguous with it and the file does not compile. It compiles everywhere else, which is why
// this survived until Windows was built for the first time.
namespace alog = atlas::log;

namespace {

constexpr alog::Category kTest{"test"};
constexpr alog::Category kOther{"other"};

/// Installs a buffer sink and restores the previous logging configuration afterwards, so
/// that tests do not leak state into each other through the global registry.
class LogFixture {
  public:
    explicit LogFixture(std::size_t capacity = 64)
        : m_buffer(std::make_shared<alog::LogBuffer>(capacity)) {
        m_id = alog::add_sink(alog::make_buffer_sink(m_buffer));
        alog::set_min_severity(alog::Severity::Trace);
    }

    ~LogFixture() {
        alog::remove_sink(m_id);
        alog::clear_category_severity(kTest);
        alog::clear_category_severity(kOther);
        alog::set_min_severity(alog::Severity::Info);
    }

    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;

    [[nodiscard]] std::vector<alog::LogBuffer::Entry> entries() const {
        return m_buffer->entries();
    }

    [[nodiscard]] std::size_t size() const { return m_buffer->size(); }

  private:
    std::shared_ptr<alog::LogBuffer> m_buffer;
    std::uint32_t m_id = 0;
};

}  // namespace

TEST_CASE("severity names are stable", "[core][log]") {
    CHECK(alog::to_string(alog::Severity::Info) == "info");
    CHECK(alog::to_short_string(alog::Severity::Info) == "INF");
    CHECK(alog::to_short_string(alog::Severity::Warning) == "WRN");
    CHECK(alog::to_short_string(alog::Severity::Fatal) == "FTL");
}

TEST_CASE("a record reaches a registered sink", "[core][log]") {
    const LogFixture fixture;

    ATLAS_LOG(kTest, alog::Severity::Info, "hello {}", 42);

    const auto entries = fixture.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "hello 42");
    CHECK(entries[0].category == "test");
    CHECK(entries[0].severity == alog::Severity::Info);
    CHECK(entries[0].thread == std::this_thread::get_id());
}

TEST_CASE("records below the minimum severity are discarded", "[core][log]") {
    const LogFixture fixture;
    alog::set_min_severity(alog::Severity::Warning);

    ATLAS_LOG(kTest, alog::Severity::Info, "dropped");
    ATLAS_LOG(kTest, alog::Severity::Warning, "kept");
    ATLAS_LOG(kTest, alog::Severity::Error, "also kept");

    const auto entries = fixture.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].message == "kept");
    CHECK(entries[1].message == "also kept");
}

TEST_CASE("a category override takes precedence over the global minimum", "[core][log]") {
    const LogFixture fixture;
    alog::set_min_severity(alog::Severity::Error);
    alog::set_category_severity(kTest, alog::Severity::Trace);

    ATLAS_LOG(kTest, alog::Severity::Trace, "verbose for this category");
    ATLAS_LOG(kOther, alog::Severity::Info, "dropped: below the global minimum");

    const auto entries = fixture.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "verbose for this category");
}

TEST_CASE("should_log answers without formatting the message", "[core][log]") {
    const LogFixture fixture;
    alog::set_min_severity(alog::Severity::Warning);

    CHECK_FALSE(alog::should_log(kTest, alog::Severity::Debug));
    CHECK(alog::should_log(kTest, alog::Severity::Error));
}

TEST_CASE("a disabled log call does not evaluate its arguments", "[core][log]") {
    const LogFixture fixture;
    alog::set_min_severity(alog::Severity::Error);

    int side_effects = 0;
    const auto counted = [&side_effects] -> int {
        ++side_effects;
        return 1;
    };

    ATLAS_LOG(kTest, alog::Severity::Debug, "{}", counted());
    CHECK(side_effects == 0);

    ATLAS_LOG(kTest, alog::Severity::Error, "{}", counted());
    CHECK(side_effects == 1);
}

TEST_CASE("the log buffer keeps only the most recent records", "[core][log]") {
    const LogFixture fixture{3};

    for (int i = 0; i < 5; ++i) {
        ATLAS_LOG(kTest, alog::Severity::Info, "record {}", i);
    }

    const auto entries = fixture.entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].message == "record 2");
    CHECK(entries[1].message == "record 3");
    CHECK(entries[2].message == "record 4");
}

TEST_CASE("logging is safe from several threads at once", "[core][log]") {
    const LogFixture fixture{1024};

    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;

    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                ATLAS_LOG(kTest, alog::Severity::Info, "thread {} record {}", t, i);
            }
        });
    }
    threads.clear();  // joins

    // Ordering across threads is not defined, but nothing may be lost or corrupted.
    CHECK(fixture.size() == std::size_t{kThreads} * kPerThread);
}

TEST_CASE("removing a sink stops delivery to it", "[core][log]") {
    const auto buffer = std::make_shared<alog::LogBuffer>(8);
    const std::uint32_t id = alog::add_sink(alog::make_buffer_sink(buffer));
    alog::set_min_severity(alog::Severity::Trace);

    ATLAS_LOG(kTest, alog::Severity::Info, "before");
    alog::remove_sink(id);
    ATLAS_LOG(kTest, alog::Severity::Info, "after");

    const auto entries = buffer->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "before");

    alog::set_min_severity(alog::Severity::Info);
}

TEST_CASE("a sink outliving its buffer does not write into freed memory", "[core][log]") {
    std::uint32_t id = 0;
    {
        const auto buffer = std::make_shared<alog::LogBuffer>(4);
        id = alog::add_sink(alog::make_buffer_sink(buffer));
        alog::set_min_severity(alog::Severity::Trace);
        ATLAS_LOG(kTest, alog::Severity::Info, "while the buffer is alive");
        CHECK(buffer->size() == 1);
    }

    // The buffer is gone; the sink is still registered. This must be a no-op, not a crash.
    // Under AddressSanitizer a dangling write would be caught here.
    ATLAS_LOG(kTest, alog::Severity::Info, "after the buffer is gone");

    alog::remove_sink(id);
    alog::set_min_severity(alog::Severity::Info);
}

TEST_CASE("a buffer reports the capacity it was given", "[core][log]") {
    const alog::LogBuffer buffer{16};
    CHECK(buffer.capacity() == 16);
    CHECK(buffer.size() == 0);
}
