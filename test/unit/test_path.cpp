#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/path.hpp"
#include "test_assert.hpp"

using cinux::base::FoldOntoPath;

namespace {

bool folds_to(const char* base, const char* addend, const char* wanted) {
    char out[96] = {};
    if (!FoldOntoPath(base, addend, out, sizeof(out)).ok) {
        return false;
    }
    return std::string_view(out) == std::string_view(wanted);
}

}  // namespace

TEST("path: a relative addend lands under the base") {
    ASSERT_TRUE(folds_to("/etc", "motd", "/etc/motd"));
    ASSERT_TRUE(folds_to("/", "etc", "/etc"));
}

TEST("path: an absolute addend folds alone") {
    ASSERT_TRUE(folds_to("/etc", "/hello.txt", "/hello.txt"));
}

TEST("path: dots fold away") {
    ASSERT_TRUE(folds_to("/etc", "./motd", "/etc/motd"));
    ASSERT_TRUE(folds_to("/etc", ".", "/etc"));
}

TEST("path: dot-dot pops one level") {
    ASSERT_TRUE(folds_to("/etc", "..", "/"));
    ASSERT_TRUE(folds_to("/etc/log", "../motd", "/etc/motd"));
}

TEST("path: dot-dot never climbs past the root") {
    ASSERT_TRUE(folds_to("/", "..", "/"));
    ASSERT_TRUE(folds_to("/", "../../etc", "/etc"));
}

TEST("path: repeated slashes are tolerated") {
    ASSERT_TRUE(folds_to("/etc", "a//b", "/etc/a/b"));
    ASSERT_TRUE(folds_to("//etc//", "motd", "/etc/motd"));
}

TEST("path: an empty addend keeps the base") {
    ASSERT_TRUE(folds_to("/etc", "", "/etc"));
    ASSERT_TRUE(folds_to("/", "", "/"));
}

TEST("path: an overflowing fold is refused") {
    char out[8] = {};
    ASSERT_TRUE(!FoldOntoPath("/", "averylongname", out, sizeof(out)).ok);
}

TEST("path: a fold too small for the root alone is refused") {
    char out[1] = {};
    ASSERT_TRUE(!FoldOntoPath("/", "..", out, sizeof(out)).ok);
}

TEST("path: the fold reports its length") {
    char out[96] = {};
    ASSERT_TRUE(FoldOntoPath("/etc", "motd", out, sizeof(out)).length == 9UL);
    ASSERT_TRUE(FoldOntoPath("/", "..", out, sizeof(out)).length == 1UL);
}

TEST("path: absolute means leading slash") {
    ASSERT_TRUE(cinux::base::IsAbsolute("/etc"));
    ASSERT_TRUE(!cinux::base::IsAbsolute("etc"));
    ASSERT_TRUE(!cinux::base::IsAbsolute(""));
}

TEST("path: file names take the last component") {
    ASSERT_TRUE(cinux::base::FileName("/etc/motd") == std::string_view("motd"));
    ASSERT_TRUE(cinux::base::FileName("motd") == std::string_view("motd"));
    ASSERT_TRUE(cinux::base::FileName("/") == std::string_view(""));
}

TEST("path: parent paths stop one directory up") {
    ASSERT_TRUE(cinux::base::ParentPath("/etc/motd") == std::string_view("/etc"));
    ASSERT_TRUE(cinux::base::ParentPath("/etc") == std::string_view("/"));
    ASSERT_TRUE(cinux::base::ParentPath("/") == std::string_view(""));
    ASSERT_TRUE(cinux::base::ParentPath("motd") == std::string_view(""));
}

TEST("path: stems drop the last extension") {
    ASSERT_TRUE(cinux::base::Stem("/a/b.tar.gz") == std::string_view("b.tar"));
    ASSERT_TRUE(cinux::base::Stem("/a/b.txt") == std::string_view("b"));
    ASSERT_TRUE(cinux::base::Stem("/a/.hidden") == std::string_view(".hidden"));
    ASSERT_TRUE(cinux::base::Stem("/a/Makefile") == std::string_view("Makefile"));
}

TEST("path: extensions carry the dot") {
    ASSERT_TRUE(cinux::base::Extension("/a/b.tar.gz") == std::string_view(".gz"));
    ASSERT_TRUE(cinux::base::Extension("/a/b.txt") == std::string_view(".txt"));
    ASSERT_TRUE(cinux::base::Extension("/a/.hidden") == std::string_view(""));
    ASSERT_TRUE(cinux::base::Extension("/a/Makefile") == std::string_view(""));
}

int main() {
    return cinux::test::RunAll();
}
