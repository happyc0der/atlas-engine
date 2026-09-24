// SPDX-License-Identifier: GPL-3.0-or-later
// Reading HOST:PORT. The rest of the library opens sockets and is covered by the integration
// cases that run two processes; this is the part that can be wrong with no network at all.
#include <atlas/app/socket_session.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::app::parse_connect;

TEST_CASE("a host and port are read, with the port after the last colon", "[app][socket]") {
    const auto plain = parse_connect("127.0.0.1:7777");
    REQUIRE(plain.has_value());
    CHECK(plain->host == "127.0.0.1");
    CHECK(plain->port == 7777);
    CHECK_FALSE(plain->listen);

    // An IPv6 literal carries colons of its own.
    const auto six = parse_connect("::1:9000");
    REQUIRE(six.has_value());
    CHECK(six->host == "::1");
    CHECK(six->port == 9000);
}

TEST_CASE("what is not HOST:PORT is refused", "[app][socket]") {
    for (const char* text :
         {"localhost", ":7777", "host:", "host:0", "host:65536", "host:77x", "host:-1"}) {
        INFO(text);
        CHECK_FALSE(parse_connect(text).has_value());
    }
    CHECK(parse_connect("host:65535").has_value());
}
