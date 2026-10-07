#include <memory>
#include <type_traits>
#include <utility>

#include "cinux/container/static_stack.hpp"
#include "framework.hpp"
#include "test_assert.hpp"

namespace {
using cinux::base::container::StaticStack;

constexpr bool kConstantStack = [] {
    StaticStack<unsigned int, 2> stack;
    if (!stack.push(10) || !stack.push(20) || stack.push(30)) {
        return false;
    }
    if (*stack.top() != 20 || stack.view()[0] != 10 || !stack.pop()) {
        return false;
    }
    return *stack.top() == 10 && stack.pop() && stack.empty() && !stack.pop();
}();
static_assert(kConstantStack);
static_assert(std::is_trivially_destructible_v<StaticStack<unsigned int, 16>>);
constinit StaticStack<unsigned int, 2> g_constant_stack;

TEST("static stack: overflow preserves residents and removal reuses capacity") {
    StaticStack<unsigned int, 2> stack;
    ASSERT_TRUE(stack.push(10));
    ASSERT_TRUE(stack.push(20));
    ASSERT_FALSE(stack.push(30));
    ASSERT_EQ(stack.size(), 2U);
    ASSERT_EQ(*stack.top(), 20U);
    ASSERT_TRUE(stack.pop());
    ASSERT_TRUE(stack.push(30));
    ASSERT_EQ(*stack.top(), 30U);
}

TEST("static stack: empty and zero-capacity stacks have no top") {
    StaticStack<unsigned int, 0> stack;
    ASSERT_TRUE(stack.empty());
    ASSERT_TRUE(stack.full());
    ASSERT_TRUE(stack.top() == nullptr);
    ASSERT_TRUE(stack.view().empty());
    ASSERT_FALSE(stack.push(10));
    ASSERT_FALSE(stack.pop());
    ASSERT_EQ(g_constant_stack.size(), 0U);
}

TEST("static stack: const view exposes only residents in bottom-to-top order") {
    StaticStack<unsigned int, 3> stack;
    ASSERT_TRUE(stack.push(10));
    ASSERT_TRUE(stack.push(20));
    const auto& const_stack = stack;
    ASSERT_EQ(*const_stack.top(), 20U);
    unsigned int sum = 0;
    for (const auto kValue : const_stack.view()) {
        sum += kValue;
    }
    ASSERT_EQ(sum, 30U);
    ASSERT_EQ(const_stack.view().size(), 2UL);
}

TEST("static stack: pop releases ownership before the stack is destroyed") {
    StaticStack<std::shared_ptr<unsigned int>, 2> stack;
    auto                                          owner     = std::make_shared<unsigned int>(10);
    const std::weak_ptr<unsigned int>             kObserver = owner;
    ASSERT_TRUE(stack.push(std::move(owner)));
    ASSERT_FALSE(kObserver.expired());
    ASSERT_TRUE(stack.pop());
    ASSERT_TRUE(kObserver.expired());
}
}  // namespace

TEST("static stack: pop transfers ownership and empty pop keeps the destination") {
    StaticStack<std::unique_ptr<unsigned int>, 1> stack;
    ASSERT_TRUE(stack.push(std::make_unique<unsigned int>(10)));
    std::unique_ptr<unsigned int> destination;
    ASSERT_TRUE(stack.pop(destination));
    ASSERT_EQ(*destination, 10U);
    ASSERT_TRUE(stack.empty());
    ASSERT_FALSE(stack.pop(destination));
    ASSERT_EQ(*destination, 10U);
}

int main() {
    return cinux::test::RunAll();
}
