# P7-a — Radio + RadioGroup + Dropdown

> P7 第一批。控件扩展:P6-a 有 CheckBox(toggle),补 Radio(单选组)+ Dropdown(下拉选)。

## 背景

CheckBox 是独立 toggle。Radio 要互斥(组内单选)。Dropdown 要弹出列表选。P7-a 补这两个常见控件。

## 目标

Radio + RadioGroup(单选管理,非 Widget)+ Dropdown(下拉,rect 全列表高)。Host ABI 零改动。

## 设计

- **RadioGroup(非 Widget 管理器)**:`radios_[]` + `select(Radio*)`(该 checked,其余 clear)+ `selected()`。不参布局/绘制 —— Radios 各自在 HBox/VBox,group 指针接互斥。
- **Radio : Widget**:`group_` + `checked_`。`on_pointer` down → `group_->select(this)`。paint:16px 圆(fill_round_rect radius 8 外环 + hollow + checked 时 primary 内点)。`set_checked`(group 调,invalidate on change)。
- **Dropdown : Widget**:`options_[]` + `opt_count_` + `selected_` + `expanded_`。**rect 全列表高**(`opt_count × kRowH`)—— 不做超 rect 弹出(Widget 模型 rect 内 hit/paint,弹出要破 clip/hit 复杂)。closed 画顶行(selected)+ "v" 标记 + 其余 surface bg;expanded 画全列表 + selected 行 primary 高亮。`on_pointer` down:未展开 → 开;已展开 + 点行 → 选+关;点外 → 关。

## 决策

1. **RadioGroup 非 Widget**:Radio 互斥是逻辑(非布局/绘制)。RadioGroup 作纯管理器,Radios 在任意布局容器,group 指针接。避免 RadioGroup : Container 的布局耦合。
2. **Dropdown rect 全列表高(非弹出)**:Widget 模型 rect 内 hit/paint。真弹出(超 rect)要破 clip + hit 超 rect(复杂)。简化:rect = opt_count × kRowH,closed 画顶行 + bg,expanded 全。视觉 closed 顶行 + 下 bg(不完美但可验,hit 简单)。
3. **Radio 圆 = fill_round_rect radius 8**:swraster 无 circle 原语。fill_round_rect(16×16 radius 8)≈ 圆。P5-d per-corner 不用(全圆)。
4. **Dropdown "v" 标记**:PSF 无 ▼ triangle,用 "v" 字符。够辨识。

## 陷阱

- **override 访问**(P4-a/P6-a 老坑):Radio/Dropdown 的 on_pointer/hit_test 放 public(基类 public override 不降级),paint_to_list protected。
- **Dropdown closed rect 全高**:closed 视觉有空 bg(列表区未展开)。接受(简化,不弹)。

## 验证

- 新 `test_radio.cpp`:RadioGroup select 互斥(r1 选 → r2 clear,反之)+ paint。`test_dropdown.cpp`:open + select-row(Cherry)+ close + selected_text + paint。
- standalone ctest **21/21** + ASAN 干净。
- Host ABI 零改动;core 仍 host-neutral。

## CinuxOS 侧

零改动。

## 下一步

P7-b sdl-host 键盘 demo(TextBox/Radio/Dropdown 端到端眼检)。
