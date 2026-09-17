// SPDX-License-Identifier: GPL-3.0-or-later
//
// How fast can a canonical state hash be, and what would a faster one cost?
//
// M7 measured a million-cell tick at 13.4 ms and found 11.8 ms of it inside `World::hash()`.
// `hash.hpp` anticipated this: FNV-1a was chosen for being reimplementable from the
// specification in a few lines, "not the fastest hash available", with `kHashAlgorithmVersion`
// there so a faster one can replace it without silently invalidating stored values. This
// measures the candidates before anything is replaced.
//
// **The prediction, written down before the first run.** FNV-1a consumes one byte per
// multiply, and each multiply depends on the previous one, so the loop is bound by a
// dependency chain rather than by memory: about three cycles of multiply latency per byte,
// which at this machine's clock is roughly one byte per nanosecond. The measured 11 MB in
// 11.8 ms is 0.93 GB/s, which agrees. If that reasoning is right then:
//
//   1. Consuming eight bytes per multiply should give close to eight times the throughput,
//      because it shortens the chain by a factor of eight and nothing else changes.
//   2. Four independent chains over interleaved words should be faster again, because the
//      chain stops being the limit at all and the loads become it.
//   3. Hashing independent blocks and then hashing their results should cost about the same
//      as (2) on one thread, since it is the same arithmetic in a different order.
//   4. Threads should divide (3), minus whatever dispatch costs — and dispatch is measured
//      here rather than assumed, because a worker pool does not exist yet.
//
// If (1) does not hold, the dependency-chain explanation is wrong and the rest of this file
// is measuring the wrong thing.
//
// None of these candidates is engine code. They live here until a measurement justifies one,
// and adopting any of them is a decision about a stored format, not a performance tweak.

#include <atlas/core/hash.hpp>

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using atlas::bench::Result;

/// The lab's million-cell world: seven bytes of cell columns and four of population.
constexpr std::size_t kWorldBytes = std::size_t{11} * 1024 * 1024;

constexpr std::uint64_t kPrime = 0x0000'0100'0000'01B3ULL;
constexpr std::uint64_t kBasis = 0xCBF2'9CE4'8422'2325ULL;

/// Eight bytes as one word, little-endian on every machine and safe when unaligned. Compiles
/// to a single load on all three tier-one targets; the byte swap is dead code on all of them.
[[nodiscard]] std::uint64_t load_le64(const std::byte* at) noexcept {
    std::uint64_t word = 0;
    std::memcpy(&word, at, sizeof(word));
    if constexpr (std::endian::native == std::endian::big) {
        word = std::byteswap(word);
    }
    return word;
}

/// What the engine does today, inlined here so the comparison is like for like.
[[nodiscard]] std::uint64_t fnv1a(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = kBasis;
    for (const std::byte byte : bytes) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        value *= kPrime;
    }
    return value;
}

/// The tail of a word-wise hash: the last few bytes, one at a time, FNV-1a's step.
[[nodiscard]] std::uint64_t finish_tail(std::uint64_t value,
                                        std::span<const std::byte> tail) noexcept {
    for (const std::byte byte : tail) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        value *= kPrime;
    }
    return value;
}

/// A final mix, applied once to the accumulated value.
///
/// This is not decoration, and the measurements are what show it. Consuming eight bytes at a
/// time puts the last word through exactly one multiply, and a multiply by an odd constant
/// diffuses upward only: bit i of the product depends on bits i and below. So a flip in a high
/// bit of the final word reaches almost nothing, and the unmixed word-wise candidates change
/// 16.6 output bits out of 64 for a one-bit input change where 32 is ideal — worse than the
/// byte-at-a-time hash they were meant to replace, which manages 30.7 because every byte goes
/// through a full multiply of its own.
///
/// The shift-multiply-shift finaliser is MurmurHash3's fmix64, also used by splitmix64. Five
/// operations once per hash, whatever the input size.
[[nodiscard]] constexpr std::uint64_t mix_final(std::uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= 0xFF51'AFD7'ED55'8CCDULL;
    value ^= value >> 33U;
    value *= 0xC4CE'B9FE'1A85'EC53ULL;
    value ^= value >> 33U;
    return value;
}

/// Candidate 1: FNV-1a's shape, eight bytes per multiply. One chain, one eighth as long.
[[nodiscard]] std::uint64_t fnv_words(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = kBasis;
    std::size_t at = 0;
    for (; at + 8 <= bytes.size(); at += 8) {
        value ^= load_le64(bytes.data() + at);
        value *= kPrime;
    }
    return finish_tail(value, bytes.subspan(at));
}

/// Candidate 2: four independent chains over interleaved words, combined at the end.
///
/// The combine is not decoration. Four lanes that are merely exclusive-ored together would
/// hash a permutation of four words to the same value, so each lane is mixed by position
/// first.
[[nodiscard]] std::uint64_t lanes4(std::span<const std::byte> bytes) noexcept {
    std::uint64_t a = kBasis;
    std::uint64_t b = kBasis ^ 1ULL;
    std::uint64_t c = kBasis ^ 2ULL;
    std::uint64_t d = kBasis ^ 3ULL;
    std::size_t at = 0;
    for (; at + 32 <= bytes.size(); at += 32) {
        a = (a ^ load_le64(bytes.data() + at)) * kPrime;
        b = (b ^ load_le64(bytes.data() + at + 8)) * kPrime;
        c = (c ^ load_le64(bytes.data() + at + 16)) * kPrime;
        d = (d ^ load_le64(bytes.data() + at + 24)) * kPrime;
    }
    std::uint64_t value = kBasis;
    for (const std::uint64_t lane : {a, b, c, d}) {
        value = (value ^ lane) * kPrime;
    }
    return finish_tail(value, bytes.subspan(at));
}

/// Candidate 3: independent blocks, then the block results in order. The shape any parallel
/// hash must have, measured on one thread so the reordering cost is visible on its own.
[[nodiscard]] std::uint64_t blocked(std::span<const std::byte> bytes,
                                    std::size_t block_bytes) noexcept {
    std::uint64_t value = kBasis;
    for (std::size_t at = 0; at < bytes.size(); at += block_bytes) {
        const std::size_t span = std::min(block_bytes, bytes.size() - at);
        value = (value ^ lanes4(bytes.subspan(at, span))) * kPrime;
    }
    return value;
}

/// The same three candidates, finalised. The only difference is mix_final.
[[nodiscard]] std::uint64_t fnv_words_mixed(std::span<const std::byte> bytes) noexcept {
    return mix_final(fnv_words(bytes));
}

[[nodiscard]] std::uint64_t lanes4_mixed(std::span<const std::byte> bytes) noexcept {
    return mix_final(lanes4(bytes));
}

[[nodiscard]] std::uint64_t blocked_mixed(std::span<const std::byte> bytes,
                                          std::size_t block_bytes) noexcept {
    return mix_final(blocked(bytes, block_bytes));
}

/// Streaming, which is the one property a word-wise hash does not get for free.
///
/// `hash.hpp` promises that hashing A and then B with A's result gives the same value as
/// hashing the concatenation, and `Hasher` is built on it: every table adds a row count and
/// then each column as a separate span. A hash that consumes eight bytes at a time keeps that
/// promise only if it holds partial words across calls, so the cost of holding them is part
/// of the cost of adopting one. The engine's tables happen to add spans that are all multiples
/// of eight today, which is exactly the kind of accident that should not be load-bearing.
class StreamHash {
  public:
    void add(std::span<const std::byte> bytes) noexcept {
        if (m_held > 0) {
            const std::size_t take = std::min(8 - m_held, bytes.size());
            std::memcpy(m_buffer.data() + m_held, bytes.data(), take);
            m_held += take;
            bytes = bytes.subspan(take);
            if (m_held < 8) {
                return;
            }
            m_value = (m_value ^ load_le64(m_buffer.data())) * kPrime;
            m_held = 0;
        }
        std::size_t at = 0;
        for (; at + 8 <= bytes.size(); at += 8) {
            m_value = (m_value ^ load_le64(bytes.data() + at)) * kPrime;
        }
        m_held = bytes.size() - at;
        std::memcpy(m_buffer.data(), bytes.data() + at, m_held);
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        return mix_final(finish_tail(m_value, std::span{m_buffer}.subspan(0, m_held)));
    }

  private:
    std::array<std::byte, 8> m_buffer{};
    std::uint64_t m_value = kBasis;
    std::size_t m_held = 0;
};

/// The same buffering applied to the four-lane candidate, which is the one worth adopting,
/// so that its streaming cost is measured rather than argued by analogy with the one above.
/// A thirty-two byte block instead of eight; the buffering is still per call, not per byte.
class StreamLanes {
  public:
    void add(std::span<const std::byte> bytes) noexcept {
        if (m_held > 0) {
            const std::size_t take = std::min(32 - m_held, bytes.size());
            std::memcpy(m_buffer.data() + m_held, bytes.data(), take);
            m_held += take;
            bytes = bytes.subspan(take);
            if (m_held < 32) {
                return;
            }
            consume(m_buffer.data());
            m_held = 0;
        }
        std::size_t at = 0;
        for (; at + 32 <= bytes.size(); at += 32) {
            consume(bytes.data() + at);
        }
        m_held = bytes.size() - at;
        std::memcpy(m_buffer.data(), bytes.data() + at, m_held);
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t combined = kBasis;
        for (const std::uint64_t lane : {m_a, m_b, m_c, m_d}) {
            combined = (combined ^ lane) * kPrime;
        }
        return mix_final(finish_tail(combined, std::span{m_buffer}.subspan(0, m_held)));
    }

  private:
    void consume(const std::byte* at) noexcept {
        m_a = (m_a ^ load_le64(at)) * kPrime;
        m_b = (m_b ^ load_le64(at + 8)) * kPrime;
        m_c = (m_c ^ load_le64(at + 16)) * kPrime;
        m_d = (m_d ^ load_le64(at + 24)) * kPrime;
    }

    std::array<std::byte, 32> m_buffer{};
    std::uint64_t m_a = kBasis;
    std::uint64_t m_b = kBasis ^ 1ULL;
    std::uint64_t m_c = kBasis ^ 2ULL;
    std::uint64_t m_d = kBasis ^ 3ULL;
    std::size_t m_held = 0;
};

/// Candidate 4: the same, across threads created for the call.
///
/// Threads per call is exactly what a worker pool exists to avoid, so the dispatch cost is
/// part of this number rather than hidden from it. The result is identical to blocked()'s
/// whatever the thread count, because the blocks are combined in index order.
[[nodiscard]] std::uint64_t blocked_threads(std::span<const std::byte> bytes,
                                            std::size_t block_bytes, unsigned threads) {
    const std::size_t blocks = (bytes.size() + block_bytes - 1) / block_bytes;
    std::vector<std::uint64_t> results(blocks, 0);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&bytes, &results, block_bytes, blocks, threads, worker] {
            for (std::size_t block = worker; block < blocks; block += threads) {
                const std::size_t at = block * block_bytes;
                const std::size_t span = std::min(block_bytes, bytes.size() - at);
                results[block] = lanes4(bytes.subspan(at, span));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::uint64_t value = kBasis;
    for (const std::uint64_t block : results) {
        value = (value ^ block) * kPrime;
    }
    return value;
}

/// The average number of output bits that change when one input bit is flipped, times a
/// thousand. An ideal 64-bit hash changes thirty-two of them, so the ideal reading is 32000.
///
/// Speed is only half of a decision about a hash that is a compatibility commitment. A faster
/// hash that detects change less reliably would be a worse state hash however quick it is,
/// and the only way to know which way that trade falls is to measure both. This is a coarse
/// test, not a substitute for a statistical test suite, and the report says so.
[[nodiscard]] std::uint64_t avalanche(std::uint64_t (*hash)(std::span<const std::byte>),
                                      std::span<std::byte> scratch) {
    const std::uint64_t base = hash(scratch);
    std::uint64_t changed = 0;
    std::uint64_t flips = 0;
    for (std::size_t byte = 0; byte < scratch.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::byte>(1U << bit);
            scratch[byte] ^= mask;
            changed += static_cast<std::uint64_t>(std::popcount(hash(scratch) ^ base));
            scratch[byte] ^= mask;
            ++flips;
        }
    }
    return (changed * 1000) / flips;
}

void die(const std::string& why) {
    std::fprintf(stderr, "bench_hash: %s\n", why.c_str());
    std::abort();
}

/// A benchmark of a hash that ignores its input would report a magnificent number. Each
/// candidate is checked for being deterministic, for depending on every byte, and for
/// depending on the order of those bytes, before any of them is timed.
void verify(std::span<const std::byte> bytes) {
    struct Candidate {
        const char* name;
        std::uint64_t (*hash)(std::span<const std::byte>);
    };

    const auto blocked_64k = [](std::span<const std::byte> in) { return blocked(in, 64 * 1024); };
    const auto blocked_64k_mixed = [](std::span<const std::byte> in) {
        return blocked_mixed(in, 64 * 1024);
    };
    const Candidate candidates[] = {
        {"fnv1a", fnv1a},
        {"fnv_words", fnv_words},
        {"lanes4", lanes4},
        {"blocked", +blocked_64k},
        {"fnv_words_mixed", fnv_words_mixed},
        {"lanes4_mixed", lanes4_mixed},
        {"blocked_mixed", +blocked_64k_mixed},
    };

    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    for (const Candidate& candidate : candidates) {
        const std::uint64_t first = candidate.hash(bytes);
        if (first != candidate.hash(bytes)) {
            die(std::format("{} is not deterministic", candidate.name));
        }
        // Every byte matters: flip one bit in the first, the middle and the last byte.
        for (const std::size_t where : {std::size_t{0}, copy.size() / 2, copy.size() - 1}) {
            copy[where] ^= std::byte{1};
            if (candidate.hash(copy) == first) {
                die(std::format("{} ignores byte {}", candidate.name, where));
            }
            copy[where] ^= std::byte{1};
        }
        // Order matters: swapping two words must change the value, which is what a careless
        // combination of independent lanes gets wrong.
        std::swap_ranges(copy.begin(), copy.begin() + 8, copy.begin() + 64);
        if (candidate.hash(copy) == first) {
            die(std::format("{} is blind to the order of its input", candidate.name));
        }
        std::swap_ranges(copy.begin(), copy.begin() + 8, copy.begin() + 64);
    }

    // Streaming must agree with one shot however the input is split, which is the whole
    // reason it buffers. Split at sizes that are deliberately not multiples of eight.
    const auto streamed = [bytes](std::initializer_list<std::size_t> splits) {
        StreamHash hash;
        std::size_t at = 0;
        for (const std::size_t split : splits) {
            const std::size_t span = std::min(split, bytes.size() - at);
            hash.add(bytes.subspan(at, span));
            at += span;
        }
        hash.add(bytes.subspan(at));
        return hash.value();
    };
    const std::uint64_t one_shot = streamed({});
    for (const auto splits : {std::initializer_list<std::size_t>{1, 2, 3, 4, 5, 6, 7},
                              std::initializer_list<std::size_t>{7, 1023, 4097},
                              std::initializer_list<std::size_t>{8, 16, 4'000'000}}) {
        if (streamed(splits) != one_shot) {
            die("StreamHash depends on where its input is split");
        }
    }
    if (one_shot != fnv_words_mixed(bytes)) {
        die("StreamHash disagrees with the one-shot hash it streams");
    }

    const auto streamed_lanes = [bytes](std::initializer_list<std::size_t> splits) {
        StreamLanes hash;
        std::size_t at = 0;
        for (const std::size_t split : splits) {
            const std::size_t span = std::min(split, bytes.size() - at);
            hash.add(bytes.subspan(at, span));
            at += span;
        }
        hash.add(bytes.subspan(at));
        return hash.value();
    };
    for (const auto splits : {std::initializer_list<std::size_t>{1, 2, 3, 4, 5, 6, 7},
                              std::initializer_list<std::size_t>{31, 33, 1'000'003},
                              std::initializer_list<std::size_t>{32, 64, 4'000'000}}) {
        if (streamed_lanes(splits) != lanes4_mixed(bytes)) {
            die("StreamLanes disagrees with lanes4_mixed, which it is supposed to stream");
        }
    }

    // The threaded version must agree with the sequential one it parallelises, or the
    // comparison below is between two different functions.
    const std::span<const std::byte> sample = bytes.subspan(0, 1024 * 1024);
    const std::uint64_t sequential = blocked(sample, 64 * 1024);
    for (const unsigned threads : {1U, 2U, 3U, 8U}) {
        if (blocked_threads(sample, 64 * 1024, threads) != sequential) {
            die(std::format("blocked_threads disagrees with blocked at {} threads", threads));
        }
    }

    // The engine adopted lanes4_mixed in M8, so its hash must still be exactly that. This is
    // what stops the engine and the measurement drifting apart: change the engine's algorithm
    // and the benchmark that justified it stops agreeing, loudly.
    if (atlas::hash_bytes(bytes) != lanes4_mixed(bytes)) {
        die("atlas::hash_bytes is no longer the candidate this benchmark measured");
    }
    // And identifiers must still be FNV-1a, which is the half of the decision that did not
    // change. "abc" as bytes, hashed by the local copy of the old algorithm.
    constexpr std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    if (atlas::hash_string("abc") != fnv1a(abc)) {
        die("atlas::hash_string is no longer FNV-1a, which identifiers depend on");
    }
}

std::vector<Result> run() {
    // Deterministic pseudo-random bytes: a hash over zeroes would be a hash over nothing.
    std::vector<std::byte> buffer(kWorldBytes);
    std::uint64_t state = 0x243F'6A88'85A3'08D3ULL;
    for (std::size_t at = 0; at + 8 <= buffer.size(); at += 8) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        std::memcpy(buffer.data() + at, &state, sizeof(state));
    }
    const std::span<const std::byte> bytes{buffer};
    verify(bytes);

    // Written to by every candidate and read once at the end, so that none of them can be
    // optimised away as a computation whose result nobody wants.
    volatile std::uint64_t sink = 0;
    const auto parameters = std::format("bytes={}", kWorldBytes);
    const auto with_bytes = [](Result result) {
        result.units_per_iteration = kWorldBytes;
        result.unit_name = "bytes";
        return result;
    };

    std::vector<Result> results;
    results.push_back(with_bytes(
        atlas::bench::measure("hash/fnv1a", parameters, 20, 3, [&] { sink = fnv1a(bytes); })));
    results.push_back(with_bytes(atlas::bench::measure("hash/fnv_words", parameters, 50, 5,
                                                       [&] { sink = fnv_words(bytes); })));
    results.push_back(with_bytes(
        atlas::bench::measure("hash/lanes4", parameters, 50, 5, [&] { sink = lanes4(bytes); })));
    results.push_back(with_bytes(atlas::bench::measure("hash/lanes4_mixed", parameters, 50, 5,
                                                       [&] { sink = lanes4_mixed(bytes); })));
    // What the engine actually runs, through its own header, including the partial-block
    // buffering the one-shot candidate above does not have to do.
    results.push_back(with_bytes(atlas::bench::measure("hash/atlas_hash_bytes", parameters, 50, 5,
                                                       [&] { sink = atlas::hash_bytes(bytes); })));
    for (const std::size_t block : {std::size_t{16} * 1024, std::size_t{256} * 1024}) {
        results.push_back(with_bytes(atlas::bench::measure(
            "hash/blocked", std::format("{} block={}KiB", parameters, block / 1024), 50, 5,
            [&] { sink = blocked(bytes, block); })));
    }
    // Streaming as the tables actually use it: a row count, then one span per column.
    results.push_back(with_bytes(atlas::bench::measure(
        "hash/stream_words", std::format("{} spans=4", parameters), 50, 5, [&] {
            StreamHash hash;
            hash.add(bytes.subspan(0, 8));
            hash.add(bytes.subspan(8, 4 * 1024 * 1024));
            hash.add(bytes.subspan(8 + (4 * 1024 * 1024), 4 * 1024 * 1024));
            hash.add(bytes.subspan(8 + (8 * 1024 * 1024)));
            sink = hash.value();
        })));
    // And the case the buffering exists for: spans that are not multiples of eight.
    results.push_back(with_bytes(atlas::bench::measure(
        "hash/stream_words", std::format("{} spans=unaligned", parameters), 50, 5, [&] {
            StreamHash hash;
            std::size_t at = 0;
            while (at < bytes.size()) {
                const std::size_t span = std::min<std::size_t>(1'000'003, bytes.size() - at);
                hash.add(bytes.subspan(at, span));
                at += span;
            }
            sink = hash.value();
        })));

    results.push_back(with_bytes(atlas::bench::measure(
        "hash/stream_lanes4", std::format("{} spans=unaligned", parameters), 50, 5, [&] {
            StreamLanes hash;
            std::size_t at = 0;
            while (at < bytes.size()) {
                const std::size_t span = std::min<std::size_t>(1'000'003, bytes.size() - at);
                hash.add(bytes.subspan(at, span));
                at += span;
            }
            sink = hash.value();
        })));

    const unsigned available = std::max(1U, std::thread::hardware_concurrency());
    for (const unsigned threads : {2U, 4U, 8U}) {
        if (threads > available) {
            continue;
        }
        results.push_back(with_bytes(atlas::bench::measure(
            "hash/blocked_threads", std::format("{} threads={}", parameters, threads), 30, 5,
            [&] { sink = blocked_threads(bytes, 256 * 1024, threads); })));
    }
    // Quality, on the same footing as speed. One kibibyte, every bit flipped in turn.
    std::vector<std::byte> scratch(buffer.begin(), buffer.begin() + 1024);
    const auto blocked_64k_mixed = [](std::span<const std::byte> in) {
        return blocked_mixed(in, 64 * 1024);
    };

    const struct {
        const char* name;
        std::uint64_t (*hash)(std::span<const std::byte>);
    } quality[] = {
        {"fnv1a", fnv1a},
        {"fnv_words", fnv_words},
        {"lanes4", lanes4},
        {"fnv_words_mixed", fnv_words_mixed},
        {"lanes4_mixed", lanes4_mixed},
        {"blocked_mixed", +blocked_64k_mixed},
    };

    for (const auto& candidate : quality) {
        Result result = atlas::bench::measure_reported(
            "hash/avalanche", std::format("algorithm={} bytes=1024", candidate.name), 3, 1,
            [&] { return avalanche(candidate.hash, scratch); });
        result.metric = atlas::bench::Metric::Count;
        result.count_name = "milli-bits of 64000 ideal 32000";
        results.push_back(result);
    }

    if (sink == 0) {
        die("every candidate returned zero, which no hash of this input does");
    }
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("hash", run);

}  // namespace
