#include "../framework/framework.hpp"
#include "cinux/container/self_list.hpp"
#include "test_assert.hpp"

namespace {

struct Item : cinux::base::container::SelfNode<Item> {
    unsigned int value;
};

Item make(unsigned int value) {
    Item item{};
    item.value = value;
    return item;
}

TEST("self list: push_back keeps arrival order through pop_head") {
    cinux::base::container::SelfList<Item> list;
    Item                                   first  = make(1);
    Item                                   second = make(2);
    Item                                   third  = make(3);
    list.push_back(first);
    list.push_back(second);
    list.push_back(third);

    ASSERT_TRUE(list.head() == &first);
    ASSERT_TRUE(list.pop_head() == &first);
    ASSERT_TRUE(list.pop_head() == &second);
    ASSERT_TRUE(list.pop_head() == &third);
    ASSERT_TRUE(list.pop_head() == nullptr);
    ASSERT_TRUE(list.empty());
}

TEST("self list: push_front jumps the queue") {
    cinux::base::container::SelfList<Item> list;
    Item                                   first  = make(1);
    Item                                   second = make(2);
    list.push_back(first);
    list.push_front(second);

    ASSERT_TRUE(list.pop_head() == &second);
    ASSERT_TRUE(list.pop_head() == &first);
}

TEST("self list: remove unlinks head, middle and tail") {
    cinux::base::container::SelfList<Item> list;
    Item                                   first  = make(1);
    Item                                   middle = make(2);
    Item                                   tail   = make(3);
    list.push_back(first);
    list.push_back(middle);
    list.push_back(tail);

    ASSERT_TRUE(list.remove(middle));
    ASSERT_TRUE(list.pop_head() == &first);
    ASSERT_TRUE(list.pop_head() == &tail);
    ASSERT_TRUE(list.empty());

    list.push_back(first);
    list.push_back(middle);
    ASSERT_TRUE(list.remove(first));
    ASSERT_TRUE(list.head() == &middle);
    ASSERT_TRUE(list.remove(middle));
    ASSERT_TRUE(list.empty());
}

TEST("self list: remove of an unlinked object reports false") {
    cinux::base::container::SelfList<Item> list;
    Item                                   lone   = make(9);
    Item                                   member = make(1);
    list.push_back(member);

    ASSERT_TRUE(!list.remove(lone));
    ASSERT_TRUE(list.remove(member));
    ASSERT_TRUE(!list.remove(member));
}

TEST("self list: pop_head detaches for re-insertion") {
    cinux::base::container::SelfList<Item> list;
    Item                                   first  = make(1);
    Item                                   second = make(2);
    list.push_back(first);
    list.push_back(second);

    Item* popped = list.pop_head();
    ASSERT_TRUE(popped == &first);
    list.push_back(*popped);
    ASSERT_TRUE(list.pop_head() == &second);
    ASSERT_TRUE(list.pop_head() == &first);
}

TEST("self list: tail survives head removals and keeps append O(1) shape") {
    cinux::base::container::SelfList<Item> list;
    Item                                   first = make(1);
    Item                                   next  = make(2);
    list.push_back(first);
    ASSERT_TRUE(list.pop_head() == &first);
    ASSERT_TRUE(list.empty());
    list.push_back(next);
    ASSERT_TRUE(list.pop_head() == &next);
}

static_assert(sizeof(cinux::base::container::SelfList<Item>) == 2 * sizeof(Item*));

}  // namespace

int main() {
    return cinux::test::RunAll();
}
