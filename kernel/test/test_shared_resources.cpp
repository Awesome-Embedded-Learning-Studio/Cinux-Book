/**
 * @file kernel/test/test_shared_resources.cpp
 * @brief QEMU in-kernel tests for refcounted shared resources (F3-M2 batch 3)
 *
 * Exercises the reference-counted wrappers that let CLONE_SIGHAND / CLONE_FS /
 * CLONE_FILES threads share process state:
 *   - SharedSigActions: create / create_copy / acquire / release + copy independence
 *   - SharedCwd:        create / create_copy / acquire / release + copy independence
 *   - FDTable:          acquire / release refcount bookkeeping
 *
 * No scheduler needed -- these are pure heap/refcount checks.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/fs/file.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/signal.hpp"

using cinux::fs::FDTable;
using cinux::proc::HandlerType;
using cinux::proc::SharedCwd;
using cinux::proc::SharedSigActions;
using cinux::proc::Signal;

namespace test_shared_sig {

void test_create_refcount() {
    auto* p = SharedSigActions::create();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ(p->refcount, 1u);

    p->acquire();
    TEST_ASSERT_EQ(p->refcount, 2u);

    p->release();  // back to 1, still alive
    TEST_ASSERT_EQ(p->refcount, 1u);

    p->release();  // -> 0, freed; do not touch p afterwards
}

void test_create_copy_is_independent() {
    auto* parent                                             = SharedSigActions::create();
    parent->actions[static_cast<int>(Signal::kSigterm)].type = HandlerType::kIgnore;

    auto* child = SharedSigActions::create_copy(parent);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_NE(static_cast<void*>(child), static_cast<void*>(parent));  // distinct object
    // Copy inherits the disposition...
    TEST_ASSERT_EQ(child->actions[static_cast<int>(Signal::kSigterm)].type, HandlerType::kIgnore);

    // ...and mutating the copy leaves the parent untouched (fork copy semantics).
    child->actions[static_cast<int>(Signal::kSigterm)].type = HandlerType::kCustom;
    TEST_ASSERT_EQ(parent->actions[static_cast<int>(Signal::kSigterm)].type, HandlerType::kIgnore);

    parent->release();
    child->release();
}

void test_shared_mutation_propagates() {
    // CLONE_SIGHAND model: two holders of the SAME object see each other's writes.
    auto* shared = SharedSigActions::create();
    shared->acquire();  // now "two" holders
    auto* same = shared;

    same->actions[static_cast<int>(Signal::kSigint)].type = HandlerType::kIgnore;
    TEST_ASSERT_EQ(shared->actions[static_cast<int>(Signal::kSigint)].type, HandlerType::kIgnore);

    shared->release();
    same->release();
}

}  // namespace test_shared_sig

namespace test_shared_cwd {

void test_create_default_root() {
    auto* c = SharedCwd::create();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQ(c->refcount, 1u);
    TEST_ASSERT_EQ(c->path[0], '/');
    TEST_ASSERT_EQ(c->path[1], '\0');
    c->release();
}

void test_create_copy_independent() {
    auto* parent = SharedCwd::create("/usr/bin");
    auto* child  = SharedCwd::create_copy(parent);
    TEST_ASSERT_NE(static_cast<void*>(child), static_cast<void*>(parent));

    // Same initial content...
    TEST_ASSERT_TRUE(parent->path[0] != '\0');
    bool same = true;
    for (uint32_t i = 0; i < SharedCwd::kPathMax; ++i) {
        if (parent->path[i] != child->path[i]) {
            same = false;
            break;
        }
    }
    TEST_ASSERT_TRUE(same);

    // ...but mutating the child does not affect the parent.
    child->path[0] = 'X';
    child->path[1] = '\0';
    TEST_ASSERT_EQ(parent->path[0], '/');

    parent->release();
    child->release();
}

void test_acquire_release() {
    auto* c = SharedCwd::create("/var");
    c->acquire();
    TEST_ASSERT_EQ(c->refcount, 2u);
    c->release();
    TEST_ASSERT_EQ(c->refcount, 1u);
    c->release();  // freed
}

}  // namespace test_shared_cwd

namespace test_fdtable_refcount {

void test_acquire_release_refcount() {
    auto* t = new FDTable;
    TEST_ASSERT_EQ(t->refcount(), 1u);

    t->acquire();  // simulate CLONE_FILES sharing
    TEST_ASSERT_EQ(t->refcount(), 2u);

    t->release();  // one holder drops
    TEST_ASSERT_EQ(t->refcount(), 1u);

    t->release();  // last holder -> table frees itself (closes any open fds)
}

}  // namespace test_fdtable_refcount

extern "C" void run_shared_resources_tests() {
    TEST_SECTION("Shared Resources Tests (F3-M2-3)");

    RUN_TEST(test_shared_sig::test_create_refcount);
    RUN_TEST(test_shared_sig::test_create_copy_is_independent);
    RUN_TEST(test_shared_sig::test_shared_mutation_propagates);

    RUN_TEST(test_shared_cwd::test_create_default_root);
    RUN_TEST(test_shared_cwd::test_create_copy_independent);
    RUN_TEST(test_shared_cwd::test_acquire_release);

    RUN_TEST(test_fdtable_refcount::test_acquire_release_refcount);

    TEST_SUMMARY();
}
