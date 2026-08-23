# 2026-07-01 · P2-a · Scene 数据模型(host-neutral 场景描述,Compositor 的输入)

> P2 渲染收敛核心第一批。给三个 probe host(offscreen/replay/fbdev)手抄的「画窗口+标题栏+文字+光标」提供一个 host-neutral 的**数据**收敛点——`Scene`(纯 POD)。本批只立数据模型 + 几何 helper + 文本拷贝;Compositor 读 Scene 画像素是 P2-b。

## 背景

P0/P1 收尾时审三个 host 的 `render_frame`,撞见两个收敛信号(详见 [PLAN.md](../ai/PLAN.md) P2 提案):

1. **场景画法抄了 3 遍**:`offscreen_host_main.cpp:98`、`replay_host_main.cpp:159`、`linux_fbdev_main.cpp:94` 各自手写同一套 `paint_scene`(bg+window+titlebar+border+text+cursor),连 `draw_rect_outline`/`draw_text` 都一字不差地抄(replay 注释自承「mirror offscreen_host_main; not shared to keep b3 local」)。
2. **dirty 只省 flush 不省 composite**:`replay_host_main.cpp:237` 每帧整场景 swraster 重画(`paint whole staging (correct); flush only dirty`)。

P2 的活:把「画一帧场景」从每个 host 抽出来,变成 host-neutral 的 **Scene(数据)+ Compositor(渲染)**。本批先立 Scene 数据层。

## 目标

1. **纯数据 Scene 模型**:背景色 + 有序 Window 栈(数组序 = z 序,后盖前)+ Cursor。零像素、零 raster 调用——只是 host 填、Compositor(P2-b)读。
2. **守 core 铁律**:`core/` 只 `stdint`/`stddef`,**禁 `<vector>`/`<string>`** → 定长 Window 数组 + NUL 终止 char 数组,跟 event ABI 的定长哲学一致。
3. **几何 helper**:`window_rect`/`cursor_rect` 把 (x,y,w,h) 升为半开 `Rect`,直接喂 P2-c 的 region 代数。
4. **零 Host ABI 改动**:Scene 是 host 链接的 host-neutral 库,不进 `host.hpp` 表 → CinuxOS 不 bump pin。

## 设计

### `core/scene.hpp` —— POD 三件套

- `Window`{x,y,w,h, 配色 5 色, titlebar_height, title[N], body[N]}:聚合体,类内全初始化。几何用 (x,y,w,h) 跟 swraster 原语签名对齐(不是半开 Rect);`titlebar_height==0` = 无标题带。
- `Cursor`{x,y,w,h,color}:小实心矩形(当前探针光标)。
- `Scene`{bg_color, windows[kSceneMaxWindows], window_count, cursor}:z 栈,Compositor 按 index 升序画(bg→windows→cursor 顶层)。
- 容量常量 `kWindowTitleLen=32` / `kWindowBodyLen=128` / `kSceneMaxWindows=16`(满则 drop,P3/P4 不够再加)。

### `core/scene.cpp` —— 容量栈 + 手写 byte copy

- `scene_add_window`:可见性检查(w>0&&h>0)→ 容量检查 → 拷入栈顶。退化/满都 return false(count 不变)。
- `window_set_title`/`window_set_body`:**手写循环**拷到 cap-1,末尾强制 NUL。nullptr 清空。
- `window_rect`/`cursor_rect`/`window_visible`:`constexpr`,留头里(region 代数同款,零运行期开销)。
- `scene_clear`:`inline`,一行重置 window_count。

### 不引 `<cstring>` 的拷贝

`while (i+1 < cap && text[i]) { dst[i]=text[i]; i++; } dst[i]='\0';`——32/128 字节,手写循环零依赖争议,保 `core/` 纯 stdint/stddef(`memcpy` 虽可用但 CODING-TASTE §1 的禁用清单是保守口径,不引最稳)。

## 决策

- **放 `core/` 而非新目录**(用户拍板决策点 A):host-neutral 库本位;DIRECTIVES A「GuiCore 对场景无知」限定的是 **pump 驱动器**,非禁 `core/` 有 Scene 类型。Scene/Compositor 零 host include 即合规。
- **定长数组 + char[] 而非 vector/string**:`core/` 禁动态容器(CODING-TASTE §1);定长跟 event ABI 同构,跨帧零分配。
- **文本只 title/body 两段**:当前三 host 场景就两段文字;多段/富文本是 P3 控件/P5 字体的事,不过早泛化。
- **几何用 (x,y,w,h) 不用 Rect**:跟 swraster 五原语签名一致(`fill_rect(s,x,y,w,h)`),`window_rect()` 在需要 region 代数时再升半开——两种表示各得其所。

## 陷阱

- **`.clang-format` 缺失(pre-existing debt,本批顺补)**:p0-a 笔记第 53 行已登记——CODING-TASTE §0/§4 把它列为机械风格权威第 1,但文件不存在,`clang-format` 退回 LLVM 默认(2 空格/指针右贴),会把新代码格式化得跟全仓(4 空格/左贴)冲突。**本批补 `.clang-format`**(Google 基底 + 4 空格 + 列 100 + 左贴 + `AccessModifierOffset:-4` 顶格 access + 连续声明对齐),用现有 `core/` 代码校准到 `region.{cpp,hpp}`/`host.hpp` **0 diff**,证明配置如实描述现状。剩 `font.cpp`(5)/`swraster.*`(10/14) 等零星手写残留**不顺手改**(守 L3 不批量重写),后续触碰时各自 format。
- **测试断言字符串长度算错**:初版 `test_scene.cpp` 写 `body[14]=='\0'`,但 `"Hello\nCinux-GUI"` 是 **15** 字符(H e l l o \n C i n u x - G U I),NUL 在 [15]。是测试 bug 非实现 bug,ctest 当场拦下,改回 15 绿。

## 验证

- **standalone ctest 全绿**(Release):5/5——含新增 `scene-test`(8 段:empty/add/几何半开/容量上限/退化拒绝/文本拷贝截断/clear/z 序)。
- **ASAN 干净**:`-fsanitize=address` 构建 `scene-test`+`evdev-test` 直接运行,exit 0,无 leak/越界。
- **clang-format 自洽**:`core/scene.{hpp,cpp}` format 后 self-diff = 0(与现有风格一致)。

## 后续

- **P2-b(🔜)**:Compositor——`compose(staging, scene)` 用 swraster 把 Scene 画进 staging,收敛三 host 重复 paint。像素结构断言对齐旧 offscreen 验零回归。
- **P2-c**:Compositor 持 prev Scene,只 composite 变化区(省 CPU 不止省 flush),`window_rect` 进 region 代数。
- **P2-d**:三 host 切 Compositor + fbdev QEMU 冒烟。
- **Debt**:`font.cpp`/`swraster.*`/`gui_core.*` 的零星 format 残留,后续触碰时顺手 format。
- CinuxOS 侧零改动(Host ABI 没动,pin 不 bump)。
