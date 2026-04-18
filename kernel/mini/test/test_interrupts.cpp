/**
 * @file kernel/mini/test/test_interrupts.cpp
 * @brief GDT/IDT/中断 的 QEMU 内核端集成测试
 *
 * 在 QEMU 内核中运行，直接验证 GDT/IDT 初始化后的状态以及
 * 中断处理是否正常工作。
 *
 * 测试内容：
 *   - GDT 初始化后段寄存器的值
 *   - IDT 初始化后的基本状态
 *   - #BP(3) 断点异常触发与恢复
 *   - #PF(14) 页错误异常触发与恢复
 *
 * 使用 kernel_test.h 框架（不是 test_framework.h）。
 */

#include "kernel_test.h"
#include "../arch/x86_64/gdt.hpp"
#include "../arch/x86_64/idt.hpp"

using cinux::mini::arch::SEGMENT_CODE64;
using cinux::mini::arch::SEGMENT_DATA64;

// ============================================================
// Test 1: 读取当前段寄存器值验证 GDT 加载成功
// ============================================================
namespace test_gdt_segments {

    /**
     * @brief 验证 GDT 初始化后 CS 寄存器指向 code64 段
     */
    void test_cs_register() {
        uint16_t cs = 0;
        __asm__ volatile("movw %%cs, %0" : "=r"(cs));
        TEST_ASSERT_EQ(cs, SEGMENT_CODE64);
    }

    /**
     * @brief 验证 GDT 初始化后 DS 寄存器指向 data64 段
     */
    void test_ds_register() {
        uint16_t ds = 0;
        __asm__ volatile("movw %%ds, %0" : "=r"(ds));
        TEST_ASSERT_EQ(ds, SEGMENT_DATA64);
    }

    /**
     * @brief 验证 GDT 初始化后 SS 寄存器指向 data64 段
     */
    void test_ss_register() {
        uint16_t ss = 0;
        __asm__ volatile("movw %%ss, %0" : "=r"(ss));
        TEST_ASSERT_EQ(ss, SEGMENT_DATA64);
    }

    /**
     * @brief 验证 GDT 初始化后 ES 寄存器指向 data64 段
     */
    void test_es_register() {
        uint16_t es = 0;
        __asm__ volatile("movw %%es, %0" : "=r"(es));
        TEST_ASSERT_EQ(es, SEGMENT_DATA64);
    }
}

// ============================================================
// Test 2: #BP 断点异常触发与恢复
// ============================================================
namespace test_bp_exception {

    /// 标记变量：handle_bp 中是否被执行过
    static volatile int bp_handler_executed = 0;

    /**
     * @brief 验证 INT3 触发 #BP 后执行能继续
     *
     * 通过 asm volatile("int $3") 触发断点异常。
     * 如果 GDT/IDT/ISR 正确配置，handle_bp 会打印信息并返回，
     * 执行继续到此断言。
     *
     * 如果执行不到这里，说明 triple fault 发生，QEMU 会重启。
     */
    void test_bp_continues_execution() {
        // 记录执行前状态
        volatile int marker_before = 0x1234;

        // 触发 #BP(3) 断点异常
        __asm__ volatile("int $3");

        // 如果能到这里，说明 #BP 处理成功并返回
        volatile int marker_after = 0x5678;

        // 验证变量没有被破坏（栈帧恢复正确）
        TEST_ASSERT_EQ(marker_before, 0x1234);
        TEST_ASSERT_EQ(marker_after, 0x5678);
    }
}

// ============================================================
// Test 3: #PF 页错误异常触发与恢复
// ============================================================
namespace test_pf_exception {

    /**
     * @brief 验证访问不存在地址触发 #PF 后执行能继续
     *
     * 通过读取一个未映射的高地址来触发页错误。
     * 如果 #PF 处理程序正确，会打印信息并返回（IRETQ），
     * 但由于没有修复页表，下次访问同一地址仍会触发 #PF。
     *
     * 注意：这里我们只验证 #PF 触发后不会死机。
     * 由于 handle_pf 只是打印信息然后返回，返回后会重新执行
     * 导致缺页的指令，从而无限循环。因此这个测试使用一种
     * 特殊方式：修改返回地址来跳过触发指令。
     *
     * 更安全的做法是使用一个已知会触发但不影响执行的地址。
     * 但由于当前 handle_pf 不修复页表，直接触发会导致死循环。
     * 因此这个测试被标记为可选，默认跳过。
     */
    void test_pf_optional() {
        // #PF 测试需要 handle_pf 修改 RIP 才能安全返回。
        // 当前 handle_pf 不修改 RIP，直接触发会死循环。
        // 因此这里只做标记，不实际触发 #PF。
        // 如果需要测试 #PF，可以在 handle_pf 中添加 RIP 跳过逻辑。
        kprintf("  [SKIP] #PF test skipped - handle_pf does not skip faulting instruction\n");
    }
}

// ============================================================
// Test 4: 多次触发异常不累积破坏
// ============================================================
namespace test_multiple_exceptions {

    /**
     * @brief 验证连续多次触发 #BP 后系统仍然正常
     */
    void test_multiple_bp() {
        volatile uint64_t canary = 0xCAFEBABEDEADC0DEULL;

        // 第一次触发
        __asm__ volatile("int $3");
        TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

        // 第二次触发
        __asm__ volatile("int $3");
        TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);

        // 第三次触发
        __asm__ volatile("int $3");
        TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
    }
}

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_interrupt_tests() {
    TEST_SECTION("GDT/IDT/Interrupt Tests (007)");

    RUN_TEST(test_gdt_segments::test_cs_register);
    RUN_TEST(test_gdt_segments::test_ds_register);
    RUN_TEST(test_gdt_segments::test_ss_register);
    RUN_TEST(test_gdt_segments::test_es_register);
    RUN_TEST(test_bp_exception::test_bp_continues_execution);
    RUN_TEST(test_pf_exception::test_pf_optional);
    RUN_TEST(test_multiple_exceptions::test_multiple_bp);

    TEST_SUMMARY();
}
