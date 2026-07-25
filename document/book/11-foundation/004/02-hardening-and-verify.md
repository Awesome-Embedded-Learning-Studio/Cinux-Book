---
title: 02 · 一批"看不见但扛事"的加固,与验证
---

# 一批"看不见但扛事"的加固,与验证

## 一批"看不见但扛事"的加固

这一章还收了几样地基活,平时没存在感、出事才知道值:

- **freestanding 头的门禁**:内核没标准库,某些标准头在 freestanding + 特定编译器下有坑,门禁挡住误用;
- **编译零警告**:收紧警告标志到零。警告不是"能过就行"——一堆警告里藏的真 bug 会被淹没;
- **`kprintf` 加 format 属性**:让编译器检查格式串和参数类型匹配(揪出一批 `%` 不匹配);
- **`static_assert` 锁结构体布局**:关键结构体的成员偏移加编译期断言(还记得 046 提的 `offsetof(CpuContext, fs_base)==80` 吗?就是这来的——改了布局,编译就挡)。

## 验证

```bash
grep -n 'CINUX_LOCKDEP\|lockdep_held_depth' kernel/CMakeLists.txt kernel/proc/scheduler.cpp
grep -rn 'NotNull' kernel/proc/scheduler.hpp kernel/proc/scheduler.cpp | head
# 打开 lockdep 构建跑测试(开发期检查)
cmake -B build -S . -DCINUX_LOCKDEP=ON && cmake --build build -j$(nproc) > /tmp/bl.log 2>&1; echo "build=$?"
```

想体会 lockdep 的价值:打开 `CINUX_LOCKDEP=ON` 跑测试,如果代码里有持锁跨调度的地方,会 panic 暴露;全绿 = 锁纪律干净。这正是多核前的"保险"——多核下这类 bug 极难复现,lockdep 把它变成开发期必炸。
