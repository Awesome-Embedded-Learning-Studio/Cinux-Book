/**
 * @file    test_result.cpp
 * @brief   Behaviour tests for Result<T>, Result<void> and ErrorString().
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_result
 */

#include <cinux/result.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "../framework/framework.hpp"

using cinux::base::ErrorString;
using cinux::base::KernelError;
using cinux::base::Result;
using cinux::test::RunAll;

static_assert(std::string_view(ErrorString(KernelError::kOk)) == "Ok");

TEST("result: error_string covers all entries") {
    ASSERT_STREQ(ErrorString(KernelError::kOk), "Ok");
    ASSERT_STREQ(ErrorString(KernelError::kOutOfMemory), "OutOfMemory");
    ASSERT_STREQ(ErrorString(KernelError::kInvalidArgument), "InvalidArgument");
    ASSERT_STREQ(ErrorString(KernelError::kNotFound), "NotFound");
    ASSERT_STREQ(ErrorString(KernelError::kIOError), "IOError");
    ASSERT_STREQ(ErrorString(KernelError::kWouldBlock), "WouldBlock");
}

TEST("result: success path") {
    const Result<int> kProbe = 42;
    ASSERT_TRUE(kProbe.ok());
    ASSERT_TRUE(static_cast<bool>(kProbe));
    ASSERT_EQ(kProbe.value(), 42);
    ASSERT_EQ(*kProbe, 42);
}

TEST("result: error path") {
    const Result<int> kProbe = KernelError::kNotFound;
    ASSERT_FALSE(kProbe.ok());
    ASSERT_FALSE(static_cast<bool>(kProbe));
    ASSERT_EQ(kProbe.error(), KernelError::kNotFound);
}

TEST("result: void specialization success") {
    const Result<void> kProbe{};
    ASSERT_TRUE(kProbe.ok());
    ASSERT_EQ(kProbe.error(), KernelError::kOk);
}

TEST("result: void specialization error") {
    const Result<void> kProbe = KernelError::kInvalidArgument;
    ASSERT_FALSE(kProbe.ok());
    ASSERT_EQ(kProbe.error(), KernelError::kInvalidArgument);
}

TEST("result: copy preserves both paths") {
    const Result<std::string> kSource = std::string("hello");
    const Result<std::string> kCopy   =  // NOLINT(performance-unnecessary-copy-initialization)
        kSource;
    ASSERT_TRUE(kCopy.ok());
    ASSERT_EQ(*kCopy, std::string("hello"));

    const Result<std::string> kErrorCase = KernelError::kIOError;
    const Result<std::string> kErrorCopy =  // NOLINT(performance-unnecessary-copy-initialization)
        kErrorCase;
    ASSERT_FALSE(kErrorCopy.ok());
    ASSERT_EQ(kErrorCopy.error(), kErrorCase.error());
}

TEST("result: move transfers ownership") {
    Result<std::string>       source = std::string("payload");
    const Result<std::string> kMoved = std::move(source);
    ASSERT_TRUE(kMoved.ok());
    ASSERT_EQ(*kMoved, std::string("payload"));
}

TEST("result: assignment destroys before reconstruct") {
    Result<std::string> probe = std::string("first");
    probe                     = std::string("second");
    ASSERT_TRUE(probe.ok());
    ASSERT_EQ(*probe, std::string("second"));

    probe = KernelError::kIOError;
    ASSERT_FALSE(probe.ok());
    ASSERT_EQ(probe.error(), KernelError::kIOError);
}

TEST("result: arrow reaches members") {
    struct Point {
        int x;
        int y;
    };
    const Result<Point> kPoint = Point{.x = 3, .y = 4};
    ASSERT_EQ(kPoint->x, 3);
    ASSERT_EQ(kPoint->y, 4);
}

TEST("result: custom error enum") {
    enum class ProbeError : std::uint8_t {
        kOk = 0,
        kNope
    };
    const Result<int, ProbeError> kProbe = ProbeError::kNope;
    ASSERT_FALSE(kProbe.ok());
    ASSERT_EQ(kProbe.error(), ProbeError::kNope);
}

int main() {
    return RunAll();
}
