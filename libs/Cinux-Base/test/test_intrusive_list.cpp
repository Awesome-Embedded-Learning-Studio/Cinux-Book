/**
 * @file test/unit/test_intrusive_list.cpp
 * @brief Tests for cinux::lib::IntrusiveList
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/intrusive_list.hpp>

using namespace cinux::lib;

struct Item {
    int                     value;
    IntrusiveListNode<Item> list_node;
};

TEST_CASE("IntrusiveList: push_back order", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}}, c{3, {}};

    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    REQUIRE(list.size() == 3);
    REQUIRE(list.front()->value == 1);
    REQUIRE(list.back()->value == 3);
}

TEST_CASE("IntrusiveList: push_front order", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}}, c{3, {}};

    list.push_front(a);
    list.push_front(b);
    list.push_front(c);

    REQUIRE(list.front()->value == 3);
    REQUIRE(list.back()->value == 1);
}

TEST_CASE("IntrusiveList: manual iteration via next", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{10, {}}, b{20, {}}, c{30, {}};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    int sum = 0;
    for (auto* cur = list.front(); cur != nullptr;
         cur       = IntrusiveListTraits<Item>::node(*cur).next) {
        sum += cur->value;
    }
    REQUIRE(sum == 60);
}

TEST_CASE("IntrusiveList: remove middle", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}}, c{3, {}};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    list.remove(b);
    REQUIRE(list.size() == 2);
    REQUIRE(list.front()->value == 1);
    REQUIRE(list.back()->value == 3);
}

TEST_CASE("IntrusiveList: remove head", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}};
    list.push_back(a);
    list.push_back(b);

    list.remove(a);
    REQUIRE(list.size() == 1);
    REQUIRE(list.front()->value == 2);
}

TEST_CASE("IntrusiveList: remove tail", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}};
    list.push_back(a);
    list.push_back(b);

    list.remove(b);
    REQUIRE(list.size() == 1);
    REQUIRE(list.back()->value == 1);
}

TEST_CASE("IntrusiveList: pop_front / pop_back", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}}, c{3, {}};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    REQUIRE(list.pop_front()->value == 1);
    REQUIRE(list.pop_back()->value == 3);
    REQUIRE(list.size() == 1);
}

TEST_CASE("IntrusiveList: clear", "[intrusive_list]") {
    IntrusiveList<Item> list;
    Item                a{1, {}}, b{2, {}};
    list.push_back(a);
    list.push_back(b);
    list.clear();

    REQUIRE(list.empty());
    REQUIRE(list.size() == 0);
    REQUIRE(list.front() == nullptr);
    REQUIRE(list.back() == nullptr);
}

TEST_CASE("IntrusiveList: empty begin == end", "[intrusive_list]") {
    IntrusiveList<Item> list;
    REQUIRE(list.begin() == list.end());
}

TEST_CASE("IntrusiveList: same element in multiple lists", "[intrusive_list]") {
    struct DualNode {
        int                         v;
        IntrusiveListNode<DualNode> list_a;
        IntrusiveListNode<DualNode> list_b;
    };

    struct TraitsA {
        static constexpr IntrusiveListNode<DualNode>& node(DualNode& o) { return o.list_a; }
    };
    struct TraitsB {
        static constexpr IntrusiveListNode<DualNode>& node(DualNode& o) { return o.list_b; }
    };

    DualNode x{42, {}, {}};

    IntrusiveList<DualNode, TraitsA> la;
    IntrusiveList<DualNode, TraitsB> lb;

    la.push_back(x);
    lb.push_back(x);

    REQUIRE(la.size() == 1);
    REQUIRE(lb.size() == 1);
    REQUIRE(la.front()->v == 42);
    REQUIRE(lb.front()->v == 42);
}
