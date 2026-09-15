// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Reading and writing authoritative state as bytes.
///
/// Binary rather than text, unlike the scene format (ADR-0007). A scene is authored and
/// reviewed by people; simulation state is generated, large, and read only by the engine, so
/// the properties that made text right there are worth nothing here and the size is not.
///
/// **Every value is explicit-width and little-endian.** Not the host's order: a save written
/// on one machine has to read on another, and "whatever the compiler does" is not a format.
/// Floating-point values go through their bit pattern, never through formatted text.
///
/// **The reader treats its input as hostile.** A save file may arrive from a mod, a download,
/// or a colleague. Every read is bounds-checked against what is actually left, so a truncated
/// or crafted file produces an error rather than a read past the end. A length in the file is
/// never used to allocate before it has been checked against the bytes that remain, because a
/// four-byte count claiming four billion rows is one of the cheapest attacks there is.
///
/// Thread affinity: neither type is synchronised. One thread at a time.

#include <atlas/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::sim {

/// Appends values to a growing buffer.
class SaveWriter {
  public:
    SaveWriter() = default;

    /// Start with room for `reserve_bytes`, to avoid regrowing during a large write.
    explicit SaveWriter(std::size_t reserve_bytes);

    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_u32(std::uint32_t value);
    void write_u64(std::uint64_t value);

    void write_i8(std::int8_t value);
    void write_i16(std::int16_t value);
    void write_i32(std::int32_t value);
    void write_i64(std::int64_t value);

    /// Written as an IEEE 754 bit pattern, so the value survives exactly.
    void write_f32(float value);
    void write_f64(double value);

    void write_bool(bool value);

    /// Raw bytes with no length of their own. The caller writes the length.
    void write_bytes(std::span<const std::byte> bytes);

    /// A 32-bit length followed by the characters.
    void write_string(std::string_view text);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }

    /// Hand over the buffer, leaving this writer empty.
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(m_bytes); }

  private:
    std::vector<std::byte> m_bytes;
};

/// Reads values back, refusing anything the buffer cannot support.
class SaveReader {
  public:
    explicit SaveReader(std::span<const std::byte> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] Result<std::uint8_t> read_u8();
    [[nodiscard]] Result<std::uint16_t> read_u16();
    [[nodiscard]] Result<std::uint32_t> read_u32();
    [[nodiscard]] Result<std::uint64_t> read_u64();

    [[nodiscard]] Result<std::int8_t> read_i8();
    [[nodiscard]] Result<std::int16_t> read_i16();
    [[nodiscard]] Result<std::int32_t> read_i32();
    [[nodiscard]] Result<std::int64_t> read_i64();

    [[nodiscard]] Result<float> read_f32();
    [[nodiscard]] Result<double> read_f64();

    /// Any byte other than 0 or 1 is a corrupt file, not a true value.
    [[nodiscard]] Result<bool> read_bool();

    /// A view into the buffer, valid as long as the buffer is. No copy.
    [[nodiscard]] Result<std::span<const std::byte>> read_bytes(std::size_t count);

    /// A length-prefixed string, refused if it claims more than `max_length` or more than
    /// the buffer holds.
    [[nodiscard]] Result<std::string> read_string(std::size_t max_length = 4096);

    /// A count that is about to be used to size a container.
    ///
    /// Checked against both `max_count` and the bytes actually remaining, so a file cannot
    /// ask for an allocation it has no data to fill. `bytes_per_item` is the smallest a row
    /// can be; passing it is what makes the second check possible, and passing zero disables
    /// it, which should be rare and deliberate.
    [[nodiscard]] Result<std::size_t> read_count(std::size_t max_count, std::size_t bytes_per_item);

    [[nodiscard]] std::size_t remaining() const noexcept { return m_bytes.size() - m_offset; }

    [[nodiscard]] std::size_t offset() const noexcept { return m_offset; }

    [[nodiscard]] bool at_end() const noexcept { return m_offset >= m_bytes.size(); }

    /// Refuse to finish while bytes are left over.
    ///
    /// Trailing data means the file does not say what the reader thinks it says, which is
    /// either a version mismatch or something crafted. Either way, accepting it would mean
    /// accepting a file that was only partly understood.
    [[nodiscard]] Status expect_end() const;

  private:
    [[nodiscard]] Result<std::span<const std::byte>> take(std::size_t count);

    std::span<const std::byte> m_bytes;
    std::size_t m_offset = 0;
};

}  // namespace atlas::sim
