#include <type_traits>

#include "cinux/ref_count.hpp"
#include "framework.hpp"
#include "test/mock/ref_object.hpp"
#include "test_assert.hpp"

namespace {
using cinux::test::RefObject;
static_assert(!std::is_copy_constructible_v<RefObject>);
static_assert(!std::is_move_constructible_v<RefObject>);

TEST("ref counted: initial owner alone destroys the object at release") {
    unsigned int destroyed = 0;
    auto* const  kObject   = new RefObject(destroyed);
    ASSERT_EQ(kObject->use_count(), 1UL);
    kObject->release_ref();
    ASSERT_EQ(destroyed, 1U);
}

TEST("ref counted: separate owners keep an object alive until the last release") {
    unsigned int destroyed = 0;
    auto* const  kObject   = new RefObject(destroyed);
    kObject->add_ref();
    kObject->add_ref();
    ASSERT_EQ(kObject->use_count(), 3UL);
    kObject->release_ref();
    kObject->release_ref();
    ASSERT_EQ(destroyed, 0U);
    ASSERT_EQ(kObject->use_count(), 1UL);
    kObject->release_ref();
    ASSERT_EQ(destroyed, 1U);
}
}  // namespace

TEST("ref count: narrow storage reaches its limit and reports only the final release") {
    cinux::base::RefCount<unsigned char> references;
    for (unsigned int owner = 1; owner < 255; ++owner) {
        references.retain();
    }
    ASSERT_EQ(references.use_count(), 255U);
    for (unsigned int owner = 255; owner > 1; --owner) {
        ASSERT_FALSE(references.release());
    }
    ASSERT_TRUE(references.release());
    ASSERT_EQ(references.use_count(), 0U);
}

int main() {
    return cinux::test::RunAll();
}
