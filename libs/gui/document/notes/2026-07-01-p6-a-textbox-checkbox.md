# P6-a — TextBox + CheckBox + 键盘路由

> P6 第一批。桌面 GUI 缺文本输入控件（terminal 是 shell；GUI 应用要 TextBox/CheckBox）。给 Widget 加键盘路由（on_key + Desktop dispatch_key/focus），立 TextBox（键盘输入 + 光标）+ CheckBox（toggle）。

## 背景

P0–P5 控件层有 Label/Button/Slider/Window/Terminal，但**没键盘路由 + 无文本输入控件**。TextBox 要键盘：Widget 只有 on_pointer。P6-a 立键盘路由基建 + 两控件。

## 目标

Widget `on_key` + Desktop `dispatch_key`（click 设焦点）+ TextBox（ascii 插入/`\b` 删 + 光标块）+ CheckBox（press toggle）。Host ABI 零改动。

## 设计

- **Widget `on_key(const KeycodePayload&)`** virtual 默认 noop（同 on_pointer 模式）。
- **Desktop `dispatch_key`**：投给 `focus_`；`focus_` 在 `dispatch_pointer` down 时设（hit target = 焦点，键盘焦点跟随点击）。
- **TextBox**：`text_[kMaxLen]` + `len_` + `cursor_`。`on_key`：ascii ≥ 0x20 → 在 cursor 处插入（尾右移）+ cursor++；`'\b'` → 删 cursor 前（尾左移）+ cursor--。paint：surface bg + text + 2px cursor 块（cursor×8px 列）。
- **CheckBox**：`checked_`。`on_pointer` down → toggle。paint：16px 外框 outline + hollow 内 + checked 时 primary 内填。

## 决策

1. **键盘焦点跟随点击**：Desktop `focus_` = pointer down 的 hit target。简单（点谁谁获焦点），符合直觉（点 TextBox 才能输入）。无 Tab 焦点遍历（后续）。
2. **TextBox 用 ascii（非 scancode）**：KeycodePayload.ascii 对可打印填 char，`'\b'` 填 backspace。host（sdl-host）SDL_TEXTINPUT → ascii，SDL_KEYDOWN BACKSPACE → ascii='\b'。scancode 跨 host 不一致，ascii 简单。
3. **cursor 块 2px 宽**（非反色 cell）：简单可见，不依赖 cell 内容。
4. **CheckBox hollow + 内填**：outline 外框 + surface hollow + checked primary 内填。比"×"字符简单（无 glyph 依赖）。

## 陷阱

- **override 降级访问**（P4-a 同款老坑再现）：TextBox `on_key` / CheckBox `on_pointer` 首版放 protected → test 直接调报 protected within context。基类 on_pointer/on_key 是 public，override 不能窄化。修复：移回 public（只 paint_to_list protected）。**教训：写新控件 override on_key/on_pointer/hit_test/layout 时，全放 public，paint_to_list 单独 protected。**
- **sdl-host T 键冲突**（未解，留 demo）：P5-b T 键切主题（KEYDOWN）与 TextBox 输入 T（TEXTINPUT）同时触发。sdl-host 键盘 demo 留眼检（移除 T 切主题 或 focus 时禁切）。

## 验证

- 新 `test_textbox.cpp`：Desktop focus（click）→ dispatch_key('A'/'B'/'C'）→ text/cursor；`\b` 删；flatten 含 text + fill。`test_checkbox.cpp`：press toggle ×2 + paint。
- standalone ctest **21/21** + ASAN 干净。
- Host ABI 零改动；core 仍 host-neutral；纯整数 + 定长（text_ 固定缓冲，无 <string>）。

## CinuxOS 侧

零改动。

## 下一步

P6-b 窗口 resize（边框 handle 拖调大小）+ 最大化/最小化/还原。sdl-host 键盘 demo 回头补（P6-a 收尾 或 P6 末）。
