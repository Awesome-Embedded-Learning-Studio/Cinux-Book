# Cinux-GUI Coding Taste

> 代码风格的**单一权威**。`DIRECTIVES.md` B 与 `CLAUDE.md`/`AGENTS.md` 指向本文。
> **铁律**：机械风格（缩进/换行/对齐/指针位置/include 排序）以 `.clang-format` 为准——**用 clang-format 格式化，不手调**；本文只管 clang-format 管不到的：命名、惯用法、注释、结构。
> 现实优先：本文扎根真实代码与 `.clang-format`，是与 CinuxOS 内核共享通用风格、裁掉内核专属项后的版本。

## 0. 权威层级
1. `.clang-format`（Google 基底）— 机械风格，跑工具。
2. 本文档 — 命名/注释/惯用法/结构。
3. `DIRECTIVES.md` A — 语言级铁律（C++17、无异常、无 RTTI、Host ABI 契约、core host-neutral 边界）。

## 1. 语言约束（见 DIRECTIVES A）
C++17（**不用 C++20**，不用 concepts/ranges）。禁异常、禁 RTTI。`core/` 只 `stdint`/`stddef`，**禁** `<vector> <string> <memory> <iostream> <algorithm>`；`host/` 可用更宽的标准库头（host 侧无跨进程 ABI 约束）。错误用返回码/简单错误处理（**不引 `ErrorOr`**——本仓当前不用）。

## 2. 命名

**文件**：`snake_case.hpp` / `snake_case.cpp`；host 适配用 `host_<name>.cpp`（如 `host_cinux.cpp`、`fake_host_main.cpp`）；**纯单测** `test/test_<module>.cpp`（放 `test/` 目录，不填 Host 表的单元测试）；host harness main（填 Host 表驱动 pump 的，含 fake/offscreen/replay/linux-fbdev）是 host 适配器形态，留 `host/`。

| 元素 | 规则 | 示例 |
|------|------|------|
| 类/结构/联合/枚举类 | PascalCase | `Region`、`SwRaster`、`PixelFormat` |
| 函数/变量/参数 | snake_case | `flush_rects()`、`dirty_count` |
| 私有成员 | snake_case + 尾随 `_`（**必须**） | `staging_buf_`、`rect_count_` |
| 公开结构体字段 | snake_case，无后缀 | `rects`、`stride`、`format` |
| 命名空间 | 小写简短 | `cinux::gui` |

**常量与枚举值（目标态 = k 前缀）**：
- `constexpr`/`const` 常量 → `kPascalCase`：`kFrameHeaderSize`、`kQ8One`。
- `enum class` 值 → `kPascalCase`：`kXrgb8888`、`kArgb8888`。
- 宏（`#define`）→ `UPPER_SNAKE`：`MAX_RECTS`、`ROUND_UP(x, align)`。
- **新代码一律 k 前缀**；**大幅触碰某 legacy 常量/枚举时顺手迁移**；**不擅自批量重写**（守 DIRECTIVES L3）。

## 3. 注释（一律英文）
- **文件头**：Doxygen `/** @file path @brief ... */`，说明模块职责。
- **公开 API**：Doxygen `@brief`/`@param`/`@return`/`@note`。
- **行内**：`//` 英文，解释**为什么**而非是什么。
- **TODO/FIXME/XXX**：**必须带实现思路**（不只是 `// TODO`），例：
  ```cpp
  // TODO: 实现 flush 脏 rect 合并：1) 收集本帧 dirty rect 2) 按 x 排序 3) 相邻同高合并 4) 调 host->flush
  ```

## 4. 格式化（clang-format 权威）
`.clang-format` 要点（不要与之对抗）：
- 缩进 4 空格、K&R 大括号（左括号同行）、`ColumnLimit: 100`。
- **命名空间内容不缩进**（`NamespaceIndentation: None`）。
- **指针/引用左贴**：`T* p`、`const char* p`、`void* x`（**唯一**风格，`PointerAlignment: Left`）。
- 连续赋值/声明/宏**对齐**（工具自动）。
- 成员初始化列表：单行 `: head_(0), tail_(0), count_(0)`；超长才换行缩进。
- **include 分块重排**（`IncludeBlocks: Regroup`）：先对应 `.hpp`（.cpp 里）→ C 系统 `<...>` → C++ `<...>` → 项目 `"..."`；`SortIncludes: CaseSensitive`。
- 条件 include 用 `#ifdef CINUX_USB / #    include "..." / #endif`（`#`后缩进）。
- **提交前跑 clang-format**。

## 5. 错误处理惯用法（真实模式）
本仓**不用 `ErrorOr`**。错误经返回码或简单状态传递——别把 `-1` 之类裸值当"成功"语义偷渡；适配层须把底层失败显式翻译成上层能判的返回值。
```cpp
// 成功：返回有效指针/true；失败：返回 nullptr/false 或错误码
Rect* Region::allocate();
if (rect == nullptr) {
    return false;  // 容量耗尽 → 调用方处理
}
```
- **边界**：`core/` 不做 I/O、不抛错；host 适配把 host 侧的失败（设备打开失败、映射失败）翻译成 core 能理解的返回值/默认行为，别让 host 错误形态泄漏进 core。

## 6. 类 / 结构布局
- `struct` = POD/聚合（公开字段，如 `Rect`、`Frame`、`EventHeader`）；`class` = 有封装（私有状态 + 方法）。有非平凡不变量/私有态用 `class`。
- 顺序：`public` 类型与方法在前，`private` 成员在后；访问修饰符之间 1 空行。
- 成员优先**类内初始化**：`uint32_t count_ = 0;`、`void* staging_{nullptr};`。
- `[[nodiscard]]` 标分配/工厂。
- 用 `using` 别名简化（`PixelSpan = Span<uint32_t>`）。

## 7. 头文件
- **`core/` 头**：`#pragma once`。
- **`host/` 头**：亦用 `#pragma once`（本仓无 Cinux-Base 子模块的 legacy `#ifndef` 守卫约束）。
- 用前向声明（`struct HostOps;`）削减编译依赖。
- **`core/` 零 host include**：`core/` 文件**只** `#include <stdint.h>` / `<stddef.h>` 及本仓 `core/*.hpp`，**绝不** include 任何 host 头（`<SDL.h>`、`<linux/fb.h>`、`"host_*.hpp"`）。core 对 host 一无所知——所有 host 能力走 `core/host.hpp` 的函数指针表（**Host ABI = 唯一硬缝**）。

## 7b. 文件行数（500 行软上限）
- 源文件（`.cpp`/`.hpp`）**软上限 500 行**——PR review 会标红超限。一个文件一个聚焦职责。
- 写/改完一个文件后 `wc -l` 看一眼；超 500 **及时按职责拆**（如 `pump.cpp` 拆出 `flush_loop.cpp`、`region.hpp` 拆出 `rect.hpp` 或把 span/迭代声明挪到独立头）。不要堆到 PR 被打回再拆。
- 测试文件可适当放宽（聚合器性质），但亦尽量按子系统拆。

## 8. 断言 / 失败处理
- **编译期**：`static_assert`（结构体布局不变量，如 `static_assert(sizeof(EventHeader) == 8)`、`static_assert(sizeof(Rect) == 16)`——见 `core/abi_check.cpp`）。
- **运行期**：`assert()`（host 单测/standalone 上下文可用 libc）或返回错误码；**core 运行期不 `abort`**——失败要么 static_assert 拦在编译期，要么返回可恢复的错误码。

## 9. 测试
框架 = standalone ctest（GoogleTest 风格 / 自写断言宏均可，跟随既有 host 单测约定）：
```cpp
namespace test_region {
void test_collapse_to_envelope() {
    TEST_ASSERT_TRUE(region_overflow_collapses_to_bbox());
}
}  // namespace test_region
```
- 断言宏：`TEST_ASSERT_EQ / NE / TRUE / FALSE / GT / LT`。
- 测试函数按主题入 `namespace test_<topic> {}`。
- **验证命令**（standalone，不是 CinuxOS 的 `run-kernel-test`）：
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
- host 单测 push 前开 ASAN 自验：`-DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`。
- Probe-1 真 fbdev 路径：QEMU-Linux 手动冒烟（非 CI，结果记笔记）。

## 10. 汇编
本仓一般不写汇编（纯用户态库）。若 host 侧有热路径需要手写（罕见），用 AT&T：源/目顺序 `mov src, dst`；寄存器 `%rax`、立即数 `$0x1000`、寻址 `offset(base, index, scale)`。每条指令右侧给语义注释；函数头用 `# ===` 块注明职责/输入(`%rdi`)/输出/影响寄存器。

## 11. CMake
变量/宏大写下划线（`CMAKE_CXX_STANDARD`、`CINUX_BUILD_TESTS`），目标小写下划线（`cinux_gui_core`、host 目标）。区域标题用 `# ===` 块；`# TODO` 带意图。

## 12. 提交前 checklist
- [ ] 跑过 clang-format；命名合规（PascalCase/snake_case/`_`/`k`前缀目标）
- [ ] 注释英文；新公开 API 有 Doxygen；TODO 带思路
- [ ] 无异常/RTTI；`core/` 无禁用标准库头、**零 host include**
- [ ] host ABI 边界未泄漏 host 形态进 `core/`
- [ ] 头 `#pragma once`；include 分块正确
- [ ] 编译 + standalone ctest 全绿（host 单测 push 前开 ASAN 自验）
- [ ] commit message 纯描述（无 Co-Auth、无里程碑标签）

## 13. 分层边界 + 最简表达（两类典型反例，易在赶进度时犯）

> 来源：CinuxOS F5-M5 HID 鼠标（2026-06-23）「梭哈」写快了被当面纠正。GUI 这种 host/core 分层的长期项目，**解耦边界 + 最简表达不能为赶进度让步**——后面拆/改费劲。两条都进了反面教材。

### 13a. 通用/传输类不得混入应用语义

一个**按职责命名的类**只承担它名字说的那一层。**传输层**（「host 怎么把像素/事件送出去」）绝不能长出**应用层**的知识（「这是个按钮 / 这个事件怎么解释成点击」）。判定法：把这个类换一个 host/用途，它的字段/方法还说得通吗？说不通的就是泄漏。

**反例**（来自 CinuxOS 同族教训）：传输类（USB 设备槽）一度被塞进「鼠标报告缓冲 / poll_mouse_report」——一个传输类不该知道「鼠标」。换个键盘这些字段就没意义 → 说明是应用语义泄漏。

**正确分层**：host 传输只给原语；应用语义收进 `core/` 的专门类。
```
host/           = 纯传输/适配：host_cinux / host_sdl（fb 映射、事件采集、flush 把像素推到设备，返回原始事件字节）
core/           = host-neutral 应用语义：Region（脏 rect 代数）、SwRaster（绘制）、Pump（事件分发 + flush 循环），用 Host ABI 表做传输
```
依赖**单向**：`core/` → Host ABI（函数指针表），**绝不反向**——host 知道 core，core 不知道 host。文件也按层收拢（`host/` 装所有 host 适配，`core/` 装 host-neutral 逻辑，别把 host 代码散进 `core/`）。

### 13b. 位布局重合就用 bitmask，别写「算恒等」的条件链

写「把 A 的位映射到 B」之前，**先核两边位布局是否一致**；一致就直接 bitmask，不要套逐位三元链模板——那种链既啰嗦又经常在算恒等（读者还要逐位验证才发现是 no-op）。

**反例**（来自 CinuxOS 同族教训）：HID boot mouse byte0（bit0/1/2 = left/right/middle）与 PS/2 Packet0（`LEFT=0x01/RIGHT=0x02/MIDDLE=0x04`）**位完全重合**，却写了：
```cpp
// 反例：三行算的是恒等映射（每项 == buttons & 那一位）
uint8_t mapped = ((buttons & 0x01) ? LEFT : 0) |
                 ((buttons & 0x02) ? RIGHT : 0) |
                 ((buttons & 0x04) ? MIDDLE : 0);
```
**正确**：核过重合 → 一行，且加注释说明为何不用映射：
```cpp
// HID boot-mouse byte0 uses the SAME button bit layout as PS/2 Packet0
// (bit0=left / 1=right / 2=middle) -- no per-bit remapping needed.
uint8_t mapped = buttons & (LEFT_BTN | RIGHT_BTN | MIDDLE_BTN);
```
推而广之：位运算期望值写进测试前，自己用常量重算一遍（别凭「2<<10」直觉——`1<<10=0x400`、`2<<10=0x800`，搞反过）。

## 14. 编译期开关（`#ifdef`）——开关归 CMake，源码不读半截路

> 来源：CinuxOS 的 `#ifdef CINUX_GUI` 曾渗透进 init/PIT/keyboard/main 四处（2026-06-25 审查），读代码读到一半撞 `#else` 要脑补两路。GUI 这种长期项目，**别让 `#ifdef` 长在别人函数的肚子里**——后面读/改都费劲。

**核心**：编不编某功能，是 CMake 的事；源码读起来该是「全编进去也读得通」，不该让审查者为了读个函数去猜哪段编不编。

**四条规矩**：
1. **可选功能整个搬进它自己的文件**，CMake 决定编不编（`if(CINUX_BUILD_TESTS) target_sources(...)`）。别处要调它就一句普通调用；没编时给个**空壳文件**顶上（同函数名，啥也不干），让链接器选一份 → 调用处零 `#ifdef`。
2. **小诊断开关**（要不要加点检查）包成一个宏，调用处就一行，看不见 `#ifdef`。
3. `#ifdef` 只用在两个地方：头守卫（`#pragma once` 那种）、真是两个程序（不同 host target）。
4. **最忌讳**：函数读到一半 `#ifdef` 分叉两条路（带 `#else`）。能消就消——收进独立文件 + 空壳，或重构让分支消失。

**正例**（host 选择，文件级 gate）：
```cmake
# host/CMakeLists.txt —— 文件级 gate
if(CINUX_HOST_SDL) target_sources(host_sdl.cpp) else target_sources(host_cinux.cpp) endif()
```
```cpp
// core/ —— 一句普通调用，不知道当前 host 是谁
host->flush(area, pixels, stride, fmt);   // 表填充在 host_*.cpp，core 零 #ifdef
```
调用处零 `#ifdef`，读起来就是一条直线。

**一句话**：开关的归属在 CMake，不在源码。可选的东西要么整文件 + 空壳，要么宏包起来；别让 `#ifdef` 长在别人函数的肚子里。
