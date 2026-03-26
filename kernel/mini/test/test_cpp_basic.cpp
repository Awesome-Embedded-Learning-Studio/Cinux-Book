/* ==============================================================
 * Cinux Mini Kernel - C++ Runtime Tests
 * ============================================================== */

#include "../lib/kprintf.h"

using cinux::mini::lib::kprintf;

// ============================================================
// Test Framework Adapter (Kernel Mode)
// ============================================================
namespace test {
    static int tests_passed = 0;
    static int tests_failed = 0;
}

// Test macros (must be outside namespace to work from other namespaces)
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kprintf("[FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            test::tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b) TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NE(a, b) TEST_ASSERT((a) != (b))
#define TEST_ASSERT_NULL(ptr) TEST_ASSERT((ptr) == nullptr)
#define TEST_ASSERT_NOT_NULL(ptr) TEST_ASSERT((ptr) != nullptr)

#define RUN_TEST(fn) \
    do { \
        kprintf("[RUN] %s\n", #fn); \
        fn(); \
        if (test::tests_failed == 0) { \
            test::tests_passed++; \
            kprintf("[PASS] %s\n", #fn); \
        } \
    } while(0)

#define TEST_SUMMARY() \
    do { \
        kprintf("\n=== Tests: %d passed, %d failed ===\n", test::tests_passed, test::tests_failed); \
    } while(0)

// ============================================================
// Test 1: Simple Class with Constructor/Destructor
// ============================================================
namespace test1 {
    static int constructor_calls = 0;
    static int destructor_calls = 0;

    class SimpleClass {
    private:
        int value;
        char marker;
    public:
        SimpleClass(int v) : value(v), marker('S') {
            constructor_calls++;
        }
        ~SimpleClass() {
            destructor_calls++;
        }
        int getValue() const { return value; }
        char getMarker() const { return marker; }
    };

    void test_simple_class() {
        test1::constructor_calls = 0;
        test1::destructor_calls = 0;

        {
            SimpleClass obj(1);
            TEST_ASSERT_EQ(obj.getValue(), 1);
            TEST_ASSERT_EQ(obj.getMarker(), 'S');
            TEST_ASSERT_EQ(constructor_calls, 1);
            TEST_ASSERT_EQ(destructor_calls, 0);
        }

        TEST_ASSERT_EQ(constructor_calls, 1);
        TEST_ASSERT_EQ(destructor_calls, 1);
    }
}

// ============================================================
// Test 2: Virtual Functions
// ============================================================
namespace test2 {
    class Base {
    public:
        virtual char getName() = 0;
        virtual int compute() = 0;
        virtual ~Base() {}
    };

    class Derived : public Base {
    private:
        int multiplier;
    public:
        Derived(int m) : multiplier(m) {}
        virtual char getName() override { return 'D'; }
        virtual int compute() override { return multiplier * 2; }
        virtual ~Derived() override {}
    };

    void test_virtual_functions() {
        Derived derived(5);
        Base* base = &derived;

        TEST_ASSERT_EQ(base->getName(), 'D');
        TEST_ASSERT_EQ(base->compute(), 10);
    }
}

// ============================================================
// Test 3: Global Object Construction
// ============================================================
namespace test3 {
    static int global_construction_count = 0;

    class GlobalCounter {
    public:
        GlobalCounter() {
            global_construction_count = 42;
        }
        int getCount() const { return global_construction_count; }
    };

    // 全局对象 - 构造函数应该在 main 之前被调用
    static GlobalCounter global_counter;

    void test_global_construction() {
        TEST_ASSERT_EQ(global_counter.getCount(), 42);
    }
}

// ============================================================
// Test 4: Multiple Inheritance
// ============================================================
namespace test4 {
    class Base1 {
    public:
        virtual int f1() { return 1; }
        virtual ~Base1() = default;
    };

    class Base2 {
    public:
        virtual int f2() { return 2; }
        virtual ~Base2() = default;
    };

    class Multi : public Base1, public Base2 {
    public:
        virtual int f1() override { return 11; }
        virtual int f2() override { return 22; }
    };

    void test_multiple_inheritance() {
        Multi m;
        Base1* b1 = &m;
        Base2* b2 = &m;

        TEST_ASSERT_EQ(b1->f1(), 11);
        TEST_ASSERT_EQ(b2->f2(), 22);
    }
}

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_cpp_tests() {
    kprintf("\n=== C++ Runtime Tests ===\n\n");

    RUN_TEST(test1::test_simple_class);
    RUN_TEST(test2::test_virtual_functions);
    RUN_TEST(test3::test_global_construction);
    RUN_TEST(test4::test_multiple_inheritance);

    TEST_SUMMARY();
}
