# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☐` 打完后打 tag，触发 prompts/ 工作流

---

## 附录 · AI Checkpoint 工作流

```
☑ 所有 checkbox 完成
  → git tag 编号_大主题_小阶段
  → 复制本 milestone 块 → prompts/03_code_review.md {{code_snippet}}
  → 复制本 milestone 块 → prompts/04_test_generation.md {{interface_snippet}}
  → 测试全绿
  → 复制本 milestone 块 → prompts/01_tutorial_hands_on.md
  → 复制本 milestone 块 → prompts/02_tutorial_readthrough.md（附完整代码）
  → 更新本文件 ☐→☑，更新 README.md 进度表
```

### 占位符速填

| 占位符 | 来源 |
|--------|------|
| `{{current_tag}}` | 刚打的 git tag |
| `{{prev_tag}}` | `git tag` 列表倒数第二 |
| `{{phase_title}}` | milestone `###` 标题 |
| `{{milestone_goal}}` | 本节「效果」一行 |
| `{{key_files}}` | 本节所有「涉及文件」 |
| `{{checklist_items}}` | 本节所有 `☐` 条目 |
| `{{code_snippet}}` | 你写完的实际代码 |

---

## Phase 9 · GUI 桌面环境

### `035_multi_terminal`
**效果**：每个终端绑定独立 shell 进程，支持多终端并发交互

- ☐ `kernel/gui/gui_init.cpp` `create_shell_terminal()` 重构：每次调用动态创建新 pipe 对（`sys_pipe`）→ `fork()` → 子进程 `execve("/bin/sh")` → 父进程将 pipe fd 绑定到新 Terminal → add_window
- ☐ `kernel/gui/terminal.cpp` 析构函数恢复 pipe 清理：终端销毁时 close pipe writer/reader → `waitpid` 回收 shell 子进程 → 防止 zombie
- ☐ `kernel/proc/init.cpp`：移除全局 pipe 创建逻辑，pipe 创建责任下沉到 `create_shell_terminal()`
- ☐ `kernel/gui/gui_init.cpp`：移除 `set_shell_pipes()` 接口（不再需要全局 pipe）
- ☐ `kernel/gui/gui_init.cpp` `gui_tick_callback`：遍历所有窗口，对每个 `is_terminal()` 的窗口执行 `poll_output()` + `render_to_canvas()`（而非只 poll focused）
- ☐ Host 单元测试 `test/unit/test_multi_terminal.cpp`：动态 pipe 创建、多终端独立 pipe 绑定、终端销毁后 pipe 清理、多 shell 并发不干扰
- ☐ Kernel 测试 `kernel/test/test_multi_terminal.cpp`：创建两个终端 → 各自独立 shell 输出 → 关闭一个不影响另一个

### `036_gui_calculator`
**效果**：点击 Calculator 图标弹出计算器窗口，支持 `+−×÷` 基本运算

- ☐ `kernel/gui/calculator.hpp/cpp`（新建）：Calculator 继承 Window
  - 常量：`DISPLAY_H=32`、`BTN_SIZE=36`、`BTN_GAP=4`，GRID 4×5（`0-9`、`+−×÷`、`=`、`C`）
  - 状态：`accumulator_`、`current_`、`pending_op_`、`new_input_`、`display_text_[32]`
  - `on_paint()`：绘制显示区（深色背景 + 白色数字）+ 按钮网格（灰色按钮 + 标签文字）
  - `on_mouse_click(mx, my)`：坐标→按钮映射 + `handle_button()`
  - `handle_button()`：数字输入、运算符、等号计算、C 清零；除零显示 `"Error"`
- ☐ `kernel/gui/window.hpp` 新增 `virtual void on_mouse_click(int32_t mx, int32_t my) { (void)mx; (void)my; }`
- ☐ `kernel/gui/window_manager.hpp/cpp` `handle_mouse()`：content area 点击时转发 `focused_->on_mouse_click(ev.mouse.x, ev.mouse.y)`
- ☐ `kernel/gui/gui_init.cpp`：`OpenCalculator` 时 `new Calculator(x, y)` + add_window
- ☐ Host 单元测试 `test/unit/test_calculator.cpp`：数字输入、四则运算、等号、清零、除零、on_mouse_click 坐标映射
- ☐ Kernel 测试 `kernel/test/test_calculator.cpp`：模拟按键 + 串口验证 display