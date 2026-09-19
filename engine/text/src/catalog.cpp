// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/text/catalog.hpp>

#include <format>
#include <string>
#include <utility>

namespace atlas::text {
namespace {

constexpr log::Category kText{"text"};

}  // namespace

Status Catalog::insert(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "string table entry with an empty key"));
    }
    if (m_strings.size() >= kMaxStrings) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("string table is full at {} entries", kMaxStrings)));
    }

    const auto [entry, inserted] = m_strings.try_emplace(std::string{key}, value);
    if (!inserted) {
        // Refused rather than replaced. Two entries for one key mean the file disagrees with
        // itself, and picking one silently makes the interface depend on parse order.
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("duplicate string table key '{}'", key)));
    }
    return {};
}

std::string_view Catalog::lookup(std::string_view key) const {
    if (const auto entry = m_strings.find(key); entry != m_strings.end()) {
        return entry->second;
    }

    ++m_misses;

    // Recorded, not logged: see the header. `contains` before `emplace` so that a key missed on
    // every frame does not build a string only to discard it.
    if (!m_missing.contains(key) && m_missing.size() < kMaxRecordedMisses) {
        m_unlogged.push_back(*m_missing.emplace(key).first);
    }

    return key;
}

std::size_t Catalog::log_new_misses() const {
    const std::size_t named = m_unlogged.size();
    for (const std::string_view key : m_unlogged) {
        ATLAS_LOG_WARN(kText, "no string for key '{}'; showing the key", key);
    }
    m_unlogged.clear();

    // Said once, and only once something was actually dropped on the floor. A caller that stops
    // at the bound has a mistake larger than any one key.
    if (m_missing.size() >= kMaxRecordedMisses && !m_miss_overflow_logged) {
        m_miss_overflow_logged = true;
        ATLAS_LOG_WARN(kText,
                       "more than {} distinct string keys are missing; further ones are counted "
                       "but not named",
                       kMaxRecordedMisses);
    }
    return named;
}

void Catalog::clear() noexcept {
    m_strings.clear();
    m_missing.clear();
    m_unlogged.clear();
    m_misses = 0;
    m_miss_overflow_logged = false;
}

}  // namespace atlas::text
