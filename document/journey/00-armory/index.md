---
title: 00 · 武器库:在写第一行内核代码之前
description: "这一站一行内核代码都不写,造的是四件趁手工具:错误处理、格式引擎、断言、测试框架。"
chapter: 0
order: 0
platform: host
difficulty: beginner
cpp_standard: 23
tags:
  - host
  - beginner
---

# 00 · 武器库:在写第一行内核代码之前

<StationPanel tag="r00_armory" build="cmake -B build && cmake --build build --target test_host" />

这一站不写内核;四件武器一件件开箱,每件都从它自己的现场讲起,再立自己的做法。

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-now" desc="无声重启的事故现场,和『第一敌人不是不会写,是不知道哪错了』">为什么是现在</ChapterLink>
  <ChapterLink num="2" href="02-rules" desc="拿 vector 按内核姿势做实验,数清七个头的允许清单与五面警告旗">七个头,五面旗</ChapterLink>
  <ChapterLink num="3" href="03-result" desc="从读不出的 -1 到有名字的错误,再与 std::expected 做四组对照">失败也是一种值</ChapterLink>
  <ChapterLink num="4" href="04-format" desc="排版与送信拆开;二十一个用例与 INT64_MIN 的边界现场">先把话排好</ChapterLink>
  <ChapterLink num="5" href="05-assert" desc="拆掉 &lt;cassert&gt; 的链接期地雷,让两个世界各交一份实现">断言:这回不借了</ChapterLink>
  <ChapterLink num="6" href="06-test-framework" desc="一行登记、快照计数、框架考自己;三十五个用例一条命令跑完">测试框架:三十五个用例,一条命令</ChapterLink>
</ChapterNav>

下一站,就是那 512 字节的引导扇区。机器上什么都没有——但咱们手里,已经有武器了。
