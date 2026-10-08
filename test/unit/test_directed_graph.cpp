#include "cinux/container/directed_graph.hpp"
#include "framework.hpp"
#include "test_assert.hpp"

namespace {
TEST("directed graph: paths follow direction and tolerate cycles") {
    cinux::base::DirectedGraph<4> graph;
    graph.connect(0, 1);
    graph.connect(1, 2);
    ASSERT_TRUE(graph.reaches(0, 2));
    ASSERT_FALSE(graph.reaches(2, 0));
    ASSERT_FALSE(graph.reaches(0, 3));
    graph.connect(2, 0);
    ASSERT_TRUE(graph.reaches(2, 1));
    ASSERT_FALSE(graph.reaches(2, 3));
}

TEST("directed graph: erasing a vertex removes incoming and outgoing paths") {
    cinux::base::DirectedGraph<64> graph;
    graph.connect(0, 63);
    graph.connect(63, 1);
    graph.connect(2, 3);
    ASSERT_TRUE(graph.reaches(0, 1));
    graph.erase(63);
    ASSERT_FALSE(graph.reaches(0, 63));
    ASSERT_FALSE(graph.reaches(63, 1));
    ASSERT_TRUE(graph.reaches(2, 3));
    graph.connect(63, 0);
    ASSERT_TRUE(graph.reaches(63, 0));
}

constexpr bool kCompileTimePath = [] {
    cinux::base::DirectedGraph<2> graph;
    graph.connect(0, 1);
    return graph.reaches(0, 1) && !graph.reaches(1, 0);
}();
static_assert(kCompileTimePath);
}  // namespace

int main() {
    return cinux::test::RunAll();
}
