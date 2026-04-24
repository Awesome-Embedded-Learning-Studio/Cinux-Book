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

### `033_gui_desktop`
**效果**：启动后显示桌面背景 + 两个可点击图标（Shell、Calculator）；点击 Shell 图标弹出终端窗口

**Shell 启动时序设计**：Shell 进程仍在 boot 时启动（`launch_first_user` 不变），pipe 在 `init.cpp` 创建并绑定 fd 0/1；Terminal 窗口延迟到用户点击 Shell 图标时才创建，pipe 指针存入 `gui_init` 模块变量；Shell 输出暂存 pipe buffer，Terminal 连接后通过 `poll_output` 取回

- ☐ `kernel/gui/window_manager.hpp/cpp`：
  - 新增 `DesktopIcon icons_[16]` + `icon_count_` + `pending_icon_action_`
  - 新增 `add_desktop_icon()`、`hit_test_icon()`、`consume_pending_icon_action()`
  - `composite()` 修改：clear 后、blit 窗口前，调用 `draw_desktop_icons()` 绘制图标位图 + 居中标签文字
  - `handle_mouse()` 修改：MouseDown 且无窗口命中时，检查 `hit_test_icon()`，命中则设 `pending_icon_action_`
- ☐ `kernel/gui/window.hpp` 新增 `virtual bool is_terminal() const { return false; }`
- ☐ `kernel/gui/terminal.hpp` 新增 `bool is_terminal() const override { return true; }`
- ☐ `kernel/gui/gui_init.hpp/cpp`：
  - `gui_start()` 返回类型改为 `void`，不再创建 Terminal
  - 新增 `set_shell_pipes(stdin_pipe*, stdout_pipe*)` 存储管道指针
  - 新增内部 `create_shell_terminal()` helper：创建 Terminal + 连接管道 + add_window
  - `gui_tick_callback` 修改：检查 `consume_pending_icon_action()`，`OpenShell` 时调 `create_shell_terminal()`；terminal poll 用 `is_terminal()` 替代 `static_cast`
  - `gui_start()` 中注册两个 DesktopIcon（Shell @ (40,40)、Calculator @ (40,120)）
- ☐ `kernel/proc/init.cpp`：pipe 创建后调 `set_shell_pipes()`，删除 `term->set_stdin_pipe/set_stdout_pipe`，`gui_start()` 不再捕获返回值
- ☐ Host 单元测试 `test/unit/test_desktop.cpp`：add_desktop_icon、hit_test_icon、consume_pending_icon_action、图标点击设 action、桌面空白点击清 focus
- ☐ Kernel 测试 `kernel/test/test_desktop.cpp`：WM 初始化 + 图标 hit test + composite 不崩溃

---

### `034_gui_calculator`
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