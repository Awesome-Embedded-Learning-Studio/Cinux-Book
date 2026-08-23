# 2026-07-01 · P3-a · 控件模型 + PaintList + 事件路由(P3 地基)

> P3 控件工具箱第一批,打地基。CinuxOS `kernel/gui/` 只有 WM/window/terminal,**无控件可参考** → 本仓从零设计控件层。本批立:Widget 基类(套娃树 + 虚 paint/hit/event)+ PaintList(绘制原语序列,取代 P2 Scene 作场景源)+ Desktop(dispatch + render)+ Compositor::execute(PaintList)。Label/Button/Slider 在 P3-c/d。

## 背景

P2 把「画一帧场景」收敛进 Compositor(读 Scene 数据),但 Scene 是「窗口级」固定结构(背景+窗口栈+光标),不能表达「窗口里套按钮、按钮点了一下变色」这种动态控件。P3 要在 Compositor 之上加一层「控件树」:控件有状态、能收事件、能告诉渲染器怎么画自己。P3-a 是这层的骨架。

## 目标

1. **PaintList 作新场景源**:控件树每帧 flatten 成一张绘制清单(fill_rect / fill_round_rect / text / clip push-pop),Compositor 照清单画 staging。**取代 P2 Scene**(决策 A①),但 P3-a 共存(Scene 退役在 P3-d,先不破 P2 host)。
2. **Widget 基类 + 套娃树**:rect + children(定长数组)+ 三个虚 hook(paint_to_list / hit_test / on_pointer)。框架管 clip + 递归,子类只填画法和事件。
3. **事件路由**:Desktop hit-test(顶层优先,点谁谁处理,不冒泡 —— 决策 C)+ 交付。
4. **clip 收敛**:flatten 时每个 widget push 自己 rect,execute 维护 clip 栈(与父 intersect)→ 控件**画不出祖先矩形外**(防溢出)。

## 设计

### `core/paint_list.{hpp,cpp}` —— 绘制清单

- `PaintCmd` = `CmdKind`(kFillRect/kFillRoundRect/kText/kClipPush/kClipPop)+ 匿名 union(各 cmd POD 载荷)。
- `PaintList`:定长 `cmds_[256]` + count,溢出 drop(不 abort,守 DIRECTIVES)。TextCmd 存 `const char*`(借指,当帧有效,控件 text 成员稳定)。
- push 接口:fill_rect / fill_round_rect / text / clip_push / clip_pop。

### `core/widget.{hpp,cpp}` —— 控件 + Desktop

- `Widget`:virtual ~Widget + set_rect/rect/visible + add_child(定长 `children_[16]`)+ `flatten(list)`(非虚,框架:clip push → paint_to_list 虚 → 递归 child → clip pop)+ virtual `hit_test`(children last-to-first 顶层优先,命中则返)+ virtual `on_pointer`/`paint_to_list`(默认 noop)。
- `Desktop`:set_root + `dispatch_pointer`(root->hit_test → on_pointer,不冒泡)+ `render(staging, font, dirty)`(root->flatten → execute;P3-a 全屏 dirty,per-widget dirty 后批)。
- 前向声明 `Surface`/`PsfFont`(widget.hpp 不 include swraster/font,.cpp 才 include)。

### `core/compositor.*::execute(PaintList)` —— 清单执行

遍历 cmds,每个调对应 swraster 原语(fill_rect / draw_text)+ **clip 栈**(`kClipPush` 与栈顶 intersect,`kClipPop` 出栈;top=-1 时 nullptr = surface bounds)。kFillRoundRect 暂 fallback 成 fill_rect(P3-b 加 fill_rounded_rect 后转真圆角)。

## 决策

- **控件树取代 Scene(共存在 P3-a)**:execute(PaintList) 是新路径,compose(Scene) 保留(P2 host 还在用)。P3-d 三 host 切控件树后 Scene 退役。共存避免 P3-a 一次动太多。
- **虚函数而非函数指针表**:Widget 用 virtual(C++17 允许,不是 RTTI —— dynamic_cast 才禁)。控件是「对象 + 行为」,class 层次自然;不像 Scene 那样纯 POD。
- **定长数组(禁 `<vector>`)**:children[16] / cmds[256],跟 event ABI / Scene 同哲学。溢出 drop + 记笔记(raise if hit)。
- **clip 在 flatten(推) + execute(栈 intersect)**:控件 paint_to_list 不用管 clip(框架已 push 自己 rect);execute 的栈保证祖先裁剪。双层防御:控件溢出画不出去。
- **hit-test 顶层优先 + 不冒泡**:children last-to-first(后画在上),命中即返;on_pointer 不往父传。简单,够 P3;冒泡后批。

## 陷阱

- **clang-format 对齐 vs Edit 精确匹配**:P2-c 时 clang-format 把 `prev_  = scene`(两空格,对齐 `first_`)。我 Edit old 写三空格 → 不匹配。**改前看一眼实际文本**(`git diff` 或 Read),别凭记忆。
- **IDE clangd 滞后(复发)**:execute 声明加了 PaintList 参数,clangd 报「paint_list.hpp not used directly」—— 它没重索引 execute。`cmake --build` 通过、ctest 绿。p0-a 起每批都遇,**以实际编译为准**。

## 验证

- **standalone ctest 全绿**(Release):7/7 —— 新 `widget-test` 4 段:
  - hit-test(嵌套 child 命中 / parent-only 命中 / 外部 miss / hidden 跳过)
  - dispatch(命中交付 / 未命中不交付)
  - flatten→execute 像素(root 全屏 + child 盖顶)
  - clip 收敛(child 超 parent rect 的部分被裁,画不出去)
- **ASAN 干净**:`widget-test`/`compositor-test` exit 0(虚析构链 + new[]/delete[] 干净)。
- **clang-format 自洽**:paint_list/widget.{hpp,cpp} self-diff = 0。

## 后续

- **P3-b(🔜)**:swraster `fill_rounded_rect`(整数角 mask)+ `core/theme.*`(Material 配色 primary/surface/on-surface + 圆角半径 + 8dp 网格)。execute 的 kFillRoundRect 转真圆角。
- **P3-c**:Label/Button/Container + HBox/VBox。
- **P3-d**:Slider + host demo + P2 Scene 退役。
- CinuxOS 侧零改动(Host ABI 没动)。
