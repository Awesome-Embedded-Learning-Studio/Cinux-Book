#include <span>
#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/text.hpp"
#include "test_assert.hpp"

TEST("text: truncation preserves surrounding storage and terminates") {
    char storage[] = "?xxxx!";
    ASSERT_TRUE(cinux::base::CopyText(std::span<char>(storage + 1, 4), "abcdef") == 3);
    ASSERT_TRUE(std::string_view(storage, 6) == std::string_view("?abc\0!", 6));
}

TEST("text: empty destination and empty source") {
    ASSERT_TRUE(cinux::base::CopyText({}, "abc") == 0);
    char storage[] = "xy";
    ASSERT_TRUE(cinux::base::CopyText(storage, {}) == 0);
    ASSERT_TRUE(storage[0] == '\0');
    ASSERT_TRUE(storage[1] == 'y');
}

int main() {
    return cinux::test::RunAll();
}
