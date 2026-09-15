// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/args.hpp>

#include <charconv>
#include <format>
#include <string_view>

namespace atlas {

Result<Args> Args::parse(int argc, const char* const* argv) {
    Args args;

    if (argc > 0 && argv[0] != nullptr) {
        args.m_program = argv[0];
    }

    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string_view token{argv[i]};

        if (!token.starts_with("--")) {
            return std::unexpected(Error(
                ErrorCode::InvalidArgument,
                std::format("unexpected positional argument '{}'; options start with --", token)));
        }

        const std::string_view body = token.substr(2);
        if (body.empty()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "empty option '--'"));
        }

        // --option=value
        if (const auto eq = body.find('='); eq != std::string_view::npos) {
            const std::string_view name = body.substr(0, eq);
            if (name.empty()) {
                return std::unexpected(
                    Error(ErrorCode::InvalidArgument, std::format("malformed option '{}'", token)));
            }
            args.m_entries.push_back(
                Entry{.name = std::string{name}, .value = std::string{body.substr(eq + 1)}});
            continue;
        }

        // --option value, where the next token is not itself an option.
        const bool next_is_value = (i + 1 < argc) && argv[i + 1] != nullptr &&
                                   !std::string_view{argv[i + 1]}.starts_with("--");

        if (next_is_value) {
            args.m_entries.push_back(
                Entry{.name = std::string{body}, .value = std::string{argv[i + 1]}});
            ++i;
        } else {
            args.m_entries.push_back(
                Entry{.name = std::string{body}, .value = {}, .is_flag = true});
        }
    }

    return args;
}

const Args::Entry* Args::find(std::string_view name) const {
    for (const auto& entry : m_entries) {
        if (entry.name == name) {
            entry.queried = true;
            return &entry;
        }
    }
    return nullptr;
}

bool Args::has(std::string_view name) const {
    return find(name) != nullptr;
}

Result<std::string_view> Args::value(std::string_view name) const {
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("missing required option --{}", name)));
    }
    if (entry->is_flag) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, std::format("option --{} needs a value", name)));
    }
    return std::string_view{entry->value};
}

std::string_view Args::value_or(std::string_view name, std::string_view fallback) const {
    const Entry* entry = find(name);
    if (entry == nullptr || entry->is_flag) {
        return fallback;
    }
    return entry->value;
}

Result<std::uint64_t> Args::value_or(std::string_view name, std::uint64_t fallback) const {
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return fallback;
    }
    if (entry->is_flag) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("option --{} needs a numeric value", name)));
    }

    std::uint64_t parsed = 0;
    const char* first = entry->value.data();
    const char* last = first + entry->value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);

    if (ec != std::errc{} || ptr != last) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("option --{} expects a non-negative integer, got '{}'", name,
                              entry->value)));
    }
    return parsed;
}

Status Args::reject_unknown() const {
    for (const auto& entry : m_entries) {
        if (!entry.queried) {
            return std::unexpected(
                Error(ErrorCode::InvalidArgument, std::format("unknown option --{}", entry.name)));
        }
    }
    return ok();
}

}  // namespace atlas
