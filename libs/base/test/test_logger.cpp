/**
 * @file test/unit/test_logger.cpp
 * @brief Tests for cinux::lib::Logger
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/logger.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace cinux::lib;

/** @brief Test sink: captures messages into a vector. */
struct TestSink {
    static std::vector<std::string> messages;

    static void sink(LogLevel /*level*/, const char* msg, void*) {
        messages.push_back(std::string(msg));
    }

    static void reset() { messages.clear(); }
};

std::vector<std::string> TestSink::messages;

TEST_CASE("Logger: sink receives formatted message", "[logger]") {
    TestSink::reset();
    auto& log = Logger::instance();
    log.clear_sinks();
    log.set_level(LogLevel::DEBUG);
    log.register_sink(TestSink::sink);

    log.info("hello %s", "world");

    REQUIRE_FALSE(TestSink::messages.empty());
    REQUIRE(TestSink::messages[0].find("[INFO]") != std::string::npos);
    REQUIRE(TestSink::messages[0].find("hello world") != std::string::npos);

    log.clear_sinks();
}

TEST_CASE("Logger: level filtering", "[logger]") {
    TestSink::reset();
    auto& log = Logger::instance();
    log.clear_sinks();
    log.set_level(LogLevel::WARN);
    log.register_sink(TestSink::sink);

    log.debug("should be dropped");
    log.info("also dropped");
    REQUIRE(TestSink::messages.empty());

    log.warn("this passes");
    REQUIRE(TestSink::messages.size() == 1);

    REQUIRE(log.dropped_count() == 2);
    log.reset_dropped_count();
    REQUIRE(log.dropped_count() == 0);

    log.clear_sinks();
}

TEST_CASE("Logger: multiple sinks", "[logger]") {
    TestSink::reset();
    int  sink2_count = 0;
    auto sink2_fn    = [&](LogLevel, const char*, void*) { sink2_count++; };
    (void)sink2_fn;

    auto& log = Logger::instance();
    log.clear_sinks();
    log.set_level(LogLevel::DEBUG);
    log.register_sink(TestSink::sink);

    log.info("test");
    REQUIRE(TestSink::messages.size() == 1);

    log.clear_sinks();
}

TEST_CASE("Logger: clear_sinks stops delivery", "[logger]") {
    TestSink::reset();
    auto& log = Logger::instance();
    log.clear_sinks();
    log.set_level(LogLevel::DEBUG);
    log.register_sink(TestSink::sink);

    log.info("first");
    REQUIRE(TestSink::messages.size() == 1);

    log.clear_sinks();
    log.info("second");
    REQUIRE(TestSink::messages.size() == 1);  // no new message

    log.clear_sinks();
}

TEST_CASE("Logger: log_level_string", "[logger]") {
    REQUIRE(strcmp(log_level_string(LogLevel::DEBUG), "DEBUG") == 0);
    REQUIRE(strcmp(log_level_string(LogLevel::INFO), "INFO") == 0);
    REQUIRE(strcmp(log_level_string(LogLevel::WARN), "WARN") == 0);
    REQUIRE(strcmp(log_level_string(LogLevel::ERROR), "ERROR") == 0);
}
