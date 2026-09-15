// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/save_stream.hpp>

#include <bit>
#include <cstring>
#include <format>

namespace atlas::sim {
namespace {

/// Write an unsigned value little-endian, one byte at a time.
///
/// Byte at a time rather than a memcpy of the object: memcpy would copy the host's order,
/// which is the thing this exists to avoid. The shifts compile to the same store on a
/// little-endian machine and to a byte swap on a big-endian one, which is what should
/// happen.
template <typename T> void append_le(std::vector<std::byte>& out, T value) {
    // Widened before shifting. A narrow unsigned type promotes to int first, which makes the
    // operand signed and the shift a signed one; widening keeps every operand unsigned and
    // is what the code means in any case.
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>((bits >> (i * 8U)) & 0xFFULL));
    }
}

template <typename T> [[nodiscard]] T read_le(std::span<const std::byte> bytes) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |=
            static_cast<T>(static_cast<T>(std::to_integer<std::uint8_t>(bytes[i])) << (i * 8U));
    }
    return value;
}

[[nodiscard]] Error truncated(std::size_t wanted, std::size_t available, std::size_t offset) {
    return {ErrorCode::MalformedData,
            std::format("the save data ends early: wanted {} byte(s) at offset {}, {} remain",
                        wanted, offset, available)};
}

}  // namespace

SaveWriter::SaveWriter(std::size_t reserve_bytes) {
    m_bytes.reserve(reserve_bytes);
}

void SaveWriter::write_u8(std::uint8_t value) {
    m_bytes.push_back(static_cast<std::byte>(value));
}

void SaveWriter::write_u16(std::uint16_t value) {
    append_le(m_bytes, value);
}

void SaveWriter::write_u32(std::uint32_t value) {
    append_le(m_bytes, value);
}

void SaveWriter::write_u64(std::uint64_t value) {
    append_le(m_bytes, value);
}

// Signed values go through the unsigned type of the same width. The conversion is
// well-defined in both directions since C++20, which fixed two's complement as the
// representation, so this is a reinterpretation of the bits and not an arithmetic change.
void SaveWriter::write_i8(std::int8_t value) {
    write_u8(static_cast<std::uint8_t>(value));
}

void SaveWriter::write_i16(std::int16_t value) {
    write_u16(static_cast<std::uint16_t>(value));
}

void SaveWriter::write_i32(std::int32_t value) {
    write_u32(static_cast<std::uint32_t>(value));
}

void SaveWriter::write_i64(std::int64_t value) {
    write_u64(static_cast<std::uint64_t>(value));
}

void SaveWriter::write_f32(float value) {
    write_u32(std::bit_cast<std::uint32_t>(value));
}

void SaveWriter::write_f64(double value) {
    write_u64(std::bit_cast<std::uint64_t>(value));
}

void SaveWriter::write_bool(bool value) {
    write_u8(value ? 1U : 0U);
}

void SaveWriter::write_bytes(std::span<const std::byte> bytes) {
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
}

void SaveWriter::write_string(std::string_view text) {
    write_u32(static_cast<std::uint32_t>(text.size()));
    for (const char character : text) {
        m_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

Result<std::span<const std::byte>> SaveReader::take(std::size_t count) {
    if (count > remaining()) {
        return std::unexpected(truncated(count, remaining(), m_offset));
    }
    const auto view = m_bytes.subspan(m_offset, count);
    m_offset += count;
    return view;
}

Result<std::uint8_t> SaveReader::read_u8() {
    auto bytes = take(1);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return std::to_integer<std::uint8_t>((*bytes)[0]);
}

Result<std::uint16_t> SaveReader::read_u16() {
    auto bytes = take(2);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return read_le<std::uint16_t>(*bytes);
}

Result<std::uint32_t> SaveReader::read_u32() {
    auto bytes = take(4);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return read_le<std::uint32_t>(*bytes);
}

Result<std::uint64_t> SaveReader::read_u64() {
    auto bytes = take(8);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return read_le<std::uint64_t>(*bytes);
}

Result<std::int8_t> SaveReader::read_i8() {
    auto value = read_u8();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return static_cast<std::int8_t>(*value);
}

Result<std::int16_t> SaveReader::read_i16() {
    auto value = read_u16();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return static_cast<std::int16_t>(*value);
}

Result<std::int32_t> SaveReader::read_i32() {
    auto value = read_u32();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return static_cast<std::int32_t>(*value);
}

Result<std::int64_t> SaveReader::read_i64() {
    auto value = read_u64();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return static_cast<std::int64_t>(*value);
}

Result<float> SaveReader::read_f32() {
    auto value = read_u32();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return std::bit_cast<float>(*value);
}

Result<double> SaveReader::read_f64() {
    auto value = read_u64();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return std::bit_cast<double>(*value);
}

Result<bool> SaveReader::read_bool() {
    auto value = read_u8();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    if (*value > 1U) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("a boolean at offset {} is {}, which is neither true nor false",
                              m_offset - 1, *value)));
    }
    return *value == 1U;
}

Result<std::span<const std::byte>> SaveReader::read_bytes(std::size_t count) {
    return take(count);
}

Result<std::string> SaveReader::read_string(std::size_t max_length) {
    auto length = read_u32();
    if (!length) {
        return std::unexpected(std::move(length).error());
    }

    const auto wanted = static_cast<std::size_t>(*length);
    if (wanted > max_length) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("a string at offset {} claims {} characters, more than the {} "
                              "allowed here",
                              m_offset - 4, wanted, max_length)));
    }

    auto bytes = take(wanted);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }

    std::string text(wanted, '\0');
    if (wanted > 0) {
        std::memcpy(text.data(), bytes->data(), wanted);
    }
    return text;
}

Result<std::size_t> SaveReader::read_count(std::size_t max_count, std::size_t bytes_per_item) {
    auto raw = read_u64();
    if (!raw) {
        return std::unexpected(std::move(raw).error());
    }

    // Compared as 64-bit before narrowing. On a 32-bit platform size_t is smaller than the
    // field, so narrowing first would wrap a hostile count into a small, plausible one.
    if (*raw > static_cast<std::uint64_t>(max_count)) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("a count at offset {} claims {} item(s), more than the {} "
                              "allowed here",
                              m_offset - 8, *raw, max_count)));
    }

    const auto count = static_cast<std::size_t>(*raw);

    // The second check, and the one that actually stops a cheap attack: a count is only
    // believable if the file still holds enough bytes to fill it.
    if (bytes_per_item > 0) {
        const std::uint64_t needed = static_cast<std::uint64_t>(count) * bytes_per_item;
        if (needed > static_cast<std::uint64_t>(remaining())) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("a count at offset {} claims {} item(s) of at least {} byte(s), "
                                  "which needs {} byte(s), but only {} remain",
                                  m_offset - 8, count, bytes_per_item, needed, remaining())));
        }
    }

    return count;
}

Status SaveReader::expect_end() const {
    if (!at_end()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("{} byte(s) left over after reading; the file does not say what "
                              "was expected",
                              remaining())));
    }
    return ok();
}

}  // namespace atlas::sim
