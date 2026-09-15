// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Generation-counted handles, and the pool that hands them out.
///
/// A raw pointer to an engine resource is a promise that the resource outlives every
/// reference to it, and that promise is broken quietly: the pointer still looks valid, and
/// the failure surfaces somewhere else entirely. A handle carries the slot it refers to and
/// the generation that slot had when the handle was made. Freeing a slot bumps its
/// generation, so a handle to a destroyed resource stops resolving, immediately and
/// detectably. See docs/adr/0005-error-and-ownership-model.md.
///
/// The Tag parameter keeps handle types apart: a BufferHandle cannot be passed where a
/// TextureHandle is expected, even though both are two integers. The tag may be an
/// incomplete type, so declaring one costs a line and no definition.

#include <atlas/core/assert.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace atlas {

/// A reference to a pooled resource.
///
/// Trivially copyable and cheap to pass by value. A default-constructed handle is null and
/// never resolves.
template <typename Tag> class Handle {
  public:
    constexpr Handle() noexcept = default;

    /// Generation zero is reserved for the null handle, so a zeroed structure is null
    /// rather than a reference to slot zero.
    [[nodiscard]] constexpr bool valid() const noexcept { return m_generation != 0; }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return m_index; }

    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return m_generation; }

    [[nodiscard]] friend constexpr bool operator==(Handle, Handle) noexcept = default;

  private:
    template <typename U, typename OtherTag> friend class HandlePool;

    constexpr Handle(std::uint32_t index, std::uint32_t generation) noexcept
        : m_index(index), m_generation(generation) {}

    std::uint32_t m_index = 0;
    std::uint32_t m_generation = 0;
};

/// Stable storage that hands out handles instead of pointers.
///
/// Slots are reused, which keeps the index space dense and iteration cheap, and the
/// generation is what makes reuse safe. Inserting does not invalidate existing handles;
/// it may invalidate pointers returned by get(), like any vector.
template <typename T, typename Tag> class HandlePool {
  public:
    using HandleType = Handle<Tag>;

    /// One more than the largest index a handle can carry.
    static constexpr std::size_t kMaxEntries = 0xFFFF'FFFFU;

    [[nodiscard]] Result<HandleType> insert(T value) {
        if (!m_free.empty()) {
            const std::uint32_t index = m_free.back();
            m_free.pop_back();

            Slot& slot = m_slots[index];
            ATLAS_ASSERT_MSG(!slot.value.has_value(), "a free slot held a live value");
            slot.value.emplace(std::move(value));
            ++m_live;
            return HandleType{index, slot.generation};
        }

        if (m_slots.size() >= kMaxEntries) {
            return std::unexpected(
                Error(ErrorCode::Exhausted,
                      std::format("handle pool is full at {} entries", kMaxEntries)));
        }

        const auto index = static_cast<std::uint32_t>(m_slots.size());
        m_slots.push_back(Slot{.value = std::move(value), .generation = kFirstGeneration});
        ++m_live;
        return HandleType{index, kFirstGeneration};
    }

    /// Resolve a handle, or nullptr if it is null, out of range, or stale.
    ///
    /// Returning nullptr rather than asserting: a stale handle is a legitimate thing for a
    /// caller to hold, for instance a renderer that kept a reference to a resource an
    /// asset reload destroyed. The caller decides whether that is an error.
    [[nodiscard]] T* get(HandleType handle) noexcept {
        Slot* slot = resolve(handle);
        // The emptiness test is repeated from resolve() deliberately: operator* rather than
        // value() keeps this noexcept, and a visible check is what makes that safe to read.
        if (slot == nullptr || !slot->value.has_value()) {
            return nullptr;
        }
        return &*slot->value;
    }

    [[nodiscard]] const T* get(HandleType handle) const noexcept {
        const Slot* slot = resolve(handle);
        if (slot == nullptr || !slot->value.has_value()) {
            return nullptr;
        }
        return &*slot->value;
    }

    [[nodiscard]] bool contains(HandleType handle) const noexcept {
        return resolve(handle) != nullptr;
    }

    /// Destroy the value a handle refers to and bump the slot's generation.
    ///
    /// Returns false if the handle did not resolve, which makes destroying twice safe and
    /// detectable rather than undefined.
    ///
    /// Not noexcept: recording the freed slot allocates.
    bool destroy(HandleType handle) {
        Slot* slot = resolve(handle);
        if (slot == nullptr) {
            return false;
        }

        slot->value.reset();
        slot->generation = next_generation(slot->generation);
        m_free.push_back(handle.index());
        --m_live;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_live; }

    [[nodiscard]] bool empty() const noexcept { return m_live == 0; }

    /// Slots allocated, live or not. Useful when reporting what a pool is holding on to.
    [[nodiscard]] std::size_t slot_count() const noexcept { return m_slots.size(); }

    /// Visit every live value with its handle. The order is slot order, which is stable for
    /// a given sequence of operations, so a leak report reads the same way twice.
    template <typename F> void for_each_live(F visitor) {
        for (std::size_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            if (slot.value.has_value()) {
                visitor(HandleType{static_cast<std::uint32_t>(i), slot.generation}, *slot.value);
            }
        }
    }

    template <typename F> void for_each_live(F visitor) const {
        for (std::size_t i = 0; i < m_slots.size(); ++i) {
            const Slot& slot = m_slots[i];
            if (slot.value.has_value()) {
                visitor(HandleType{static_cast<std::uint32_t>(i), slot.generation}, *slot.value);
            }
        }
    }

    /// Destroy every value. Existing handles stop resolving, because every generation moves.
    ///
    /// Not noexcept: rebuilding the free list allocates.
    void clear() {
        for (std::size_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            if (slot.value.has_value()) {
                slot.value.reset();
                slot.generation = next_generation(slot.generation);
                m_free.push_back(static_cast<std::uint32_t>(i));
            }
        }
        m_live = 0;
    }

    void reserve(std::size_t count) { m_slots.reserve(count); }

  private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = kFirstGeneration;
    };

    /// Generation zero means null, so live generations start at one.
    static constexpr std::uint32_t kFirstGeneration = 1;

    /// Wrap past zero, which is reserved. After four billion reuses of one slot a stale
    /// handle could alias a live one; a resource pool that turns over that many times in a
    /// process is not a situation Atlas expects, and saying so is better than pretending
    /// the counter is infinite.
    [[nodiscard]] static constexpr std::uint32_t next_generation(std::uint32_t current) noexcept {
        const std::uint32_t next = current + 1;
        return next == 0 ? kFirstGeneration : next;
    }

    [[nodiscard]] Slot* resolve(HandleType handle) noexcept {
        if (!handle.valid() || handle.index() >= m_slots.size()) {
            return nullptr;
        }
        Slot& slot = m_slots[handle.index()];
        if (slot.generation != handle.generation() || !slot.value.has_value()) {
            return nullptr;
        }
        return &slot;
    }

    [[nodiscard]] const Slot* resolve(HandleType handle) const noexcept {
        if (!handle.valid() || handle.index() >= m_slots.size()) {
            return nullptr;
        }
        const Slot& slot = m_slots[handle.index()];
        if (slot.generation != handle.generation() || !slot.value.has_value()) {
            return nullptr;
        }
        return &slot;
    }

    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free;
    std::size_t m_live = 0;
};

}  // namespace atlas
