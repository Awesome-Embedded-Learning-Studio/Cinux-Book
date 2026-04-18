# =============================================================================
# test/CMakeLists.txt
# @brief Cinux 测试配置
#
# 此文件配置 Cinux 的 host 端单元测试。
# 测试在宿主机（Linux）上运行，使用 -DCINUX_HOST_TEST 编译标志。
#
# 测试文件位置：
#   - test/unit/test_*.cpp : 单元测试
#   - test/integration/*.cpp : 集成测试（后续）
#
# 测试框架：
#   - test/framework/test_framework.h : 自研轻量测试框架
# =============================================================================

# ============================================================
# 测试编译选项
# ============================================================

# 设置测试专用编译定义
# 添加 CINUX_HOST_TEST 宏，告诉代码在 host 端运行
set(CINUX_HOST_TEST ON)

# 设置测试包含目录
# 添加测试框架和内核头文件目录
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/kernel
)

# ============================================================
# CTest 配置
# ============================================================

# 启用 CMake 的 CTest 功能
enable_testing()

# ============================================================
# 测试可执行文件：test_smoke
# ============================================================

# 创建冒烟测试可执行文件
# 这是最基础的测试，验证测试框架本身工作正常
add_executable(test_smoke
    unit/test_smoke.cpp
)

# 添加编译定义：CINUX_HOST_TEST
target_compile_definitions(test_smoke PRIVATE CINUX_HOST_TEST)

# 设置包含目录
target_include_directories(test_smoke PRIVATE ${TEST_INCLUDE_DIRS})

# 注册测试到 CTest
add_test(NAME smoke COMMAND test_smoke)
set_tests_properties(smoke PROPERTIES LABELS "smoke")

# ============================================================
# 测试可执行文件：test_kprintf_format
# ============================================================
add_executable(test_kprintf_format
    unit/test_kprintf_format.cpp
    ${CMAKE_SOURCE_DIR}/kernel/mini/lib/private/format.cpp
)

target_compile_definitions(test_kprintf_format PRIVATE CINUX_HOST_TEST)
target_include_directories(test_kprintf_format PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME kprintf_format COMMAND test_kprintf_format)
set_tests_properties(kprintf_format PROPERTIES LABELS "kprintf_format")

# ============================================================
# 测试可执行文件：test_gdt_idt
# ============================================================
# GDT/IDT 数据结构与编码逻辑的 host 端单元测试
# 测试 GDT 描述符编码、IDT 条目编码、结构体布局、段选择子常量
add_executable(test_gdt_idt
    unit/test_gdt_idt.cpp
)

target_compile_definitions(test_gdt_idt PRIVATE CINUX_HOST_TEST)
target_include_directories(test_gdt_idt PRIVATE ${TEST_INCLUDE_DIRS})
add_test(NAME gdt_idt COMMAND test_gdt_idt)
set_tests_properties(gdt_idt PROPERTIES LABELS "gdt_idt")

# ============================================================
# 测试辅助目标
# ============================================================

# Host 端单元测试
add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)

# 统一测试目标：先运行 host 测试，再运行内核测试（自动退出）
# 使用脚本包装，确保 USES_TERMINAL 能正确传递给 QEMU
configure_file(
    ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh.in
    ${CMAKE_BINARY_DIR}/run_all_tests.sh
    @ONLY
)

add_custom_target(test
    COMMAND sh ${CMAKE_BINARY_DIR}/run_all_tests.sh
    DEPENDS test_smoke test_kprintf_format test_gdt_idt test-image
    USES_TERMINAL
    COMMENT "Running ALL tests (host then kernel)..."
)

# 详细模式运行 host 测试
add_custom_target(test_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure
    DEPENDS test_smoke test_kprintf_format test_gdt_idt
    COMMENT "Running host tests in verbose mode..."
)

# 单个测试快捷方式
add_custom_target(test_smoke_run
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R smoke
    DEPENDS test_smoke
    COMMENT "Running smoke test..."
)
