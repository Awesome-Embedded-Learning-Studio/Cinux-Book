---
title: Lab 016 · ext2 SMP-safe 验证:block_buf_ 替换痕迹 + 双参重载
---

# Lab 016 · ext2 SMP-safe 验证:block_buf_ 替换痕迹 + 双参重载

> 对应 `document/book/08-filesystem/016/`。验证档 **B 档**:race 修复是内部重写、这次没搭 host 上的 TSAN 确定性回归,lab 靠「跑 run-kernel-test 全绿 + grep 验收替换痕迹 + 读双参重载注释」三步,亲手确认「`block_buf_` 在 SMP 路径上已被 per-call `KmBuf` 替换干净,且整体没回归」。不改代码——把「block_buf_ 还该出现在哪儿」的判断内化成能自己说出来的纪律。

## 目标

确认四件事:

1. **整体没回归**:跑 `run-kernel-test`,确认全绿(给命令、给预期数字,不给答案);
2. **`block_buf_` 不再出现在 SMP 路径**:在 `libs/ext2/` 下 `grep block_buf_`,预期只在「成员声明 + init 期使用 + 几处 NOT SMP-safe 注释」里出现,真正并发路径的文件(`ext2_inode.cpp`、`ext2_metadata.cpp`、`ext2_directory.cpp`、`ext2_dirops.cpp`、`ext2_extent.cpp` 的调用方)里**不应有裸 `block_buf_` 读写**,只应有 `KmBuf` 或传入的 scratch 指针;
3. **双参重载 + 单参留 init**:打开 `ext2.hpp` 找 `read_block`/`write_block`/`zero_and_write_block` 的单参+双参对,读注释确认单参版被明确标注「NOT SMP-safe; SMP uses the dst/src overload」,单参版的实现委托 `block_buf_`;
4. **能说出「block_buf_ 还该出现在哪些地方才安全」**:把上面 grep 到的「该出现的 3-5 处」和「不该出现的」各列出来,这是把迁移纪律内化的练习。

## 步骤

### 1. 编 + 跑 run-kernel-test,确认全绿

```bash
cmake --build build -j$(nproc) 2>&1 | grep -iE 'ext2|Built target big_kernel_test' | head
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'EXT2|Tests:' | tail -20
```

应看到 `libs/ext2/` 下的文件都编了,机制测里 ext2 相关用例 `[PASS]`,末尾 `Tests:` 那行是「全 passed, 0 failed」(具体数字看实际输出,别照抄章节——那是验证证据不是固定战绩,跑出来是多少就是多少)。

> 这一章没搭 host 上的 TSAN 回归(见章节边界),所以 lab **不跑 TSAN**,race 的确定性回归是缺的——全绿只证明「没回归」,不证明「race 被确定性根治」。这是真实的测试缺口,要心里有数。

### 2. grep 验收 `block_buf_` 替换痕迹

```bash
grep -rn 'block_buf_' libs/ext2/
```

**先自己判一遍**,再对照下面的预期。该出现的地方(约这么几类):

- **成员声明**:`ext2.hpp` 里 `uint8_t block_buf_[4096];` 那一行(章节主线一已贴);
- **init 期使用**:`ext2_init.cpp` 里 `read_block(block_num, block_buf_)`(单参版委托)、`mount()` 里读 superblock / BGDT 那几处直写 `block_buf_`(章节主线三已贴,挂载时单线程);
- **注释**:`ext2.hpp` 头文件里 `NOT SMP-safe` 注释、`ext2_common.hpp` 的 `KmBuf` 文档、`ext2_common.cpp` / `ext2_inode.cpp` 里解释「为什么不用 block_buf_」的注释。

**不该出现**的地方:SMP 路径文件里**裸的 `block_buf_` 读写**。具体说:

```bash
grep -nE 'block_buf_\b' libs/ext2/ext2_inode.cpp libs/ext2/ext2_metadata.cpp \
                   libs/ext2/ext2_directory.cpp libs/ext2/ext2_block.cpp \
                   libs/ext2/ext2_dirops.cpp libs/ext2/ext2_links.cpp
```

(也可以直接 `grep -rn 'block_buf_\b' libs/ext2/` 一次性扫整个库,覆盖更全。)预期:这几个文件里 `block_buf_` 只出现在**注释**里(解释为什么不用),不应出现在「`read_block(x, block_buf_)` / `memcpy(..., block_buf_, ...)`」这种真读写里。真读写应该是 `KmBuf` 对象的 `.get()` / `.data()`,或者函数参数传进来的 `scratch` / `dst` / `src` 指针。

### 3. 把「该出现」和「不该出现」各列出来

拿张纸(或者文本文件),把第 2 步 grep 到的每一处 `block_buf_` 归类:

- **该出现(安全)**:写明它是「成员声明 / init 期单线程 / 纯注释」中的哪一类,为什么安全;
- **不该出现(残留 race)**:如果有任何一处是 SMP 路径文件里的裸读写,记下来——那就是一个漏迁的调用点,一个还留着的 race。

如果判下来「该出现的」是 3-5 类、「不该出现的」是 0 处,就跟章节的迁移结论一致。这一步的目的是形成肌肉记忆:**看到 `block_buf_`,第一反应是问「这是 init 期还是 SMP 路径?」**——这是把迁移纪律从「章节里读到的」变成「自己能执行的」的关键一步。

### 4. 读双参重载,确认单参版被明确标注 + 委托 block_buf_

打开 [ext2.hpp](libs/ext2/ext2.hpp) 找这三对:

- `read_block(uint32_t)` 单参 + `read_block(uint32_t, void* dst)` 双参(约 `ext2.hpp:120-123`);
- `write_block(uint32_t)` 单参 + `write_block(uint32_t, void* src)` 双参(约 `ext2.hpp:129-139`);
- `zero_and_write_block(uint32_t)` 单参 + `zero_and_write_block(uint32_t, void* src)` 双参(约 `ext2.hpp:141-144`)。

逐对确认两件事:

- 单参版的**注释**里写了 `NOT SMP-safe; SMP uses the dst/src overload`(或等价表述)——这是「写死边界」的信号,告诉后来的读者这块只能单线程用;
- 单参版的**实现**(在 [ext2_init.cpp](libs/ext2/ext2_init.cpp) 约 `47-89` 行)是 `return read_block(block_num, block_buf_);` 这种**委托 `block_buf_`** 的形态——也就是说单参版就是「用共享 buffer 的那版」,init 期之外没人该再调它。

然后回答一个问题(**写下来,这是 lab 的交付物之一**):

> **为什么单参版不直接删掉?** 提示:章节主线三讲了两条理由——一条是「init 期还在用、用得合理」,另一条是「大调用面 race 修复的标准姿势」。把这两条用自己的话写一遍,再想一个第三条(比如:删掉会怎样、留下加注释又怎样)。

### 5.(可选)看一个 SMP 路径的 KmBuf 用法,确认「出作用域自动 kfree」

挑一个调用方,比如 `Ext2FileOps::read` 里的 `KmBuf scratch`:

```bash
grep -nA3 'KmBuf scratch' libs/ext2/ext2_common.cpp | head
```

应看到 `KmBuf scratch(4096);` 后面紧跟 `if (!scratch) return ...IOError;`——这就是 `operator bool` 查 OOM 的用法(`kmalloc` 可能失败,不能假设成功)。再翻到函数结尾,确认**没有手写的 `kfree`**——`KmBuf` 的析构在出函数作用域时由编译器自动调。这是 RAII 的核心收益:消除共享 + 不用记着 free,两件事一次解决。

## 范围与边界

- **不搭 host TSAN**:这次没搭 host 上的确定性竞态回归(host test 基建有预存债,`test_ext2_ops` 这个 target 因为 `#include "fs/ext2/ext2_types.hpp"` 还指向搬家前的旧路径而编不过),这是章节诚实边界里写明的测试缺口。lab 只验「逻辑上 `block_buf_` 已替换干净 + run-kernel-test 没回归」,不验「race 被确定性根治」。
- **不要求改代码**:B 档验证型。lab 的体力活在「grep + 判断 + 写归类」,不在「动代码」——迁移已经做完,要做的是**验收**。
- **「该出现的 3-5 处」是范围不是精确数字**:具体几处取决于怎么归类(比如 init 期的 superblock 读和 BGDT 读算一处还是两处)。判对了「为什么这里安全」比数对了几处更重要。
- **truncate shrink-only leak 不在本 lab**:那是已知 hobby-os 式 leak(read 不超过 `i_size` 所以非正确性问题),跟 race 修复是两件事,lab 不验。
