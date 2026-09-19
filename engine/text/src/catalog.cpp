// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
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

Status Catalog::load(const assets::ImportedStringTable& table) {
    // Replaced, never merged: see the header.
    clear();

    for (const auto& [key, value] : table.strings) {
        if (auto status = insert(key, value); !status) {
            // The importer already refuses duplicates and oversized entries, so reaching here
            // means this catalog's own bound was the binding one. Either way nothing is left
            // half-applied.
            clear();
            return status;
        }
    }

    m_locale = table.locale;
    return {};
}

std::size_t Catalog::finalise_pending(assets::Registry& registry) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ZONE_NAMED("Catalog::finalise_pending");

    std::size_t taken = 0;

    for (const assets::AssetId id : registry.pending_finalisation()) {
        // Skipped rather than relied on: `take_string_table` returns nothing for an asset with
        // no string-table payload, so removing this line changes the work done and not the
        // result. A mutation run confirmed that — it survives, and it is meant to. The same
        // arrangement the texture cache has had since M4 and the clip cache since M13, and the
        // same honest caveat the audio device's finaliser records.
        if (id.type() != assets::AssetType::StringTable) {
            continue;
        }
        auto imported = registry.take_string_table(id);
        if (!imported) {
            // Another finaliser claimed it, or it was claimed on an earlier pump. Skipping is
            // right and silent; the registry's own stall counter is what reports an asset that
            // nobody claims at all.
            continue;
        }

        const Status status = load(*imported);
        if (!status) {
            // `load` has already emptied the catalog, so nothing is half-applied. The asset
            // is marked failed so the registry reports it rather than leaving it looking
            // loaded and merely silent.
            ATLAS_LOG_ERROR(kText, "string table {} was refused: {}", id.to_string(),
                            status.error());
            registry.mark_failed(id, status.error().to_string());
            continue;
        }

        registry.mark_ready(id);
        ++taken;
        ATLAS_LOG_INFO(kText, "string table '{}' ready with {} entries", m_locale,
                       m_strings.size());
    }

    return taken;
}

void Catalog::clear() noexcept {
    m_strings.clear();
    m_locale.clear();
    m_missing.clear();
    m_unlogged.clear();
    m_misses = 0;
    m_miss_overflow_logged = false;
}

}  // namespace atlas::text
