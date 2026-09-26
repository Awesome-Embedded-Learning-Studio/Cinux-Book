<script setup lang="ts">
/**
 * 首页入口区。
 *
 * 这里刻意没有「小标题 + 大标题」的区块头：它紧接 hero，是 hero 里那两个
 * 按钮的展开，不是一个新话题。再加一道标题会让首屏下方连着出现三层抬头。
 * 视觉重量差异本身就是信息：主入口占两倍宽，次入口是并列的窄条。
 */
import { withBase } from 'vitepress'

const secondary = [
  { title: '排错笔记', note: '真实内核故障的定位与收敛过程', href: '/debug-notes/' },
  { title: '参考手册', note: '寄存器、接口与边界速查', href: '/reference/' },
  { title: '开发流程', note: 'CI 矩阵与工程约定', href: '/ci/' },
]
</script>

<template>
  <section class="entry">
    <a class="entry__main" :href="withBase('/primer/')">
      <span class="entry__main-top">
        <span class="entry__tag">前置卷</span>
        <span class="entry__count">工具链 · 编译 · 调试</span>
      </span>
      <strong>先把武器磨好，再一起从零重走</strong>
      <p>主线正在重写：跟着真实的开发史把一台机器从头建起来。动工之前，先在前置卷把工具链与调试环境备齐。</p>
      <span class="entry__go">开始阅读<i aria-hidden="true">→</i></span>
    </a>

    <div class="entry__side">
      <a v-for="item in secondary" :key="item.title" class="entry__item" :href="withBase(item.href)">
        <span class="entry__item-head">
          <strong>{{ item.title }}</strong>
          <i aria-hidden="true">→</i>
        </span>
        <span class="entry__item-note">{{ item.note }}</span>
      </a>
    </div>

    <a class="entry__journey" :href="withBase('/journey/00-armory/')">
      <span class="entry__main-top">
        <span class="entry__tag">教程</span>
        <span class="entry__count">第一站 · 武器库</span>
      </span>
      <strong>在写第一行内核代码之前,先把武器备齐</strong>
      <p>错误处理、格式引擎、断言、测试框架,第一站咱们把这四件趁手工具造出来,后面照着真实的开发史一站一站往下走。</p>
      <span class="entry__go">开始读<i aria-hidden="true">→</i></span>
    </a>
  </section>
</template>

<style scoped>
.entry {
  display: grid;
  grid-template-columns: minmax(0, 1.25fr) minmax(280px, 0.75fr);
  gap: 16px;
  max-width: 1152px;
  margin: 0 auto;
  padding: 64px 24px 0;
}

/* ── 主入口 ── */
.entry__main {
  display: flex;
  flex-direction: column;
  min-width: 0;
  padding: 30px 32px 26px;
  color: inherit;
  background: var(--vp-c-bg-alt);
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px;
  text-decoration: none;
  transition: border-color 0.18s ease, background 0.18s ease;
}
.entry__main:hover {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 50%, var(--vp-c-divider));
  background: var(--vp-c-bg-elv);
}
.entry__main-top { display: flex; align-items: center; gap: 11px; }
.entry__tag {
  padding: 3px 9px;
  color: var(--vp-c-bg);
  background: var(--vp-c-brand-1);
  border-radius: 5px;
  font-size: 11px;
  font-weight: 700;
}
.entry__count {
  color: var(--vp-c-text-3);
  font: 500 11.5px/1 var(--vp-font-family-mono);
  letter-spacing: 0.04em;
}
.entry__main strong {
  margin: 20px 0 0;
  max-width: 19em;
  color: var(--vp-c-text-1);
  font-size: clamp(22px, 2.5vw, 31px);
  font-weight: 700;
  line-height: 1.28;
  letter-spacing: -0.035em;
}
.entry__main p {
  max-width: 34em;
  margin: 12px 0 26px;
  color: var(--vp-c-text-2);
  font-size: 13.5px;
  line-height: 1.8;
}
.entry__go {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  margin-top: auto;
  color: var(--vp-c-brand-1);
  font-size: 13.5px;
  font-weight: 650;
}
.entry__go i { transition: transform 0.18s ease; }
.entry__main:hover .entry__go i { transform: translateX(4px); }

/* ── 次入口:并列窄条,靠密度与主入口区分 ── */
.entry__side {
  display: grid;
  grid-template-rows: repeat(3, minmax(0, 1fr));
  gap: 8px;
  min-width: 0;
}
.entry__item {
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: 5px;
  min-width: 0;
  padding: 16px 18px;
  color: inherit;
  border: 1px solid var(--vp-c-divider);
  border-radius: 10px;
  text-decoration: none;
  transition: border-color 0.18s ease, background 0.18s ease;
}
.entry__item:hover {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 50%, var(--vp-c-divider));
  background: var(--vp-c-bg-alt);
}
.entry__item-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}
.entry__item-head strong { color: var(--vp-c-text-1); font-size: 14.5px; font-weight: 650; }
.entry__item-head i { color: var(--vp-c-text-3); font-size: 12px; transition: color 0.18s ease, transform 0.18s ease; }
.entry__item:hover .entry__item-head i { color: var(--vp-c-brand-1); transform: translateX(3px); }
.entry__item-note {
  overflow: hidden;
  color: var(--vp-c-text-3);
  font-size: 12px;
  line-height: 1.5;
  text-overflow: ellipsis;
}

.entry__journey {
  grid-column: 1 / -1;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 10px 24px;
  padding: 18px 26px;
  color: inherit;
  background: var(--vp-c-bg-alt);
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px;
  text-decoration: none;
  transition: border-color 0.18s ease, background 0.18s ease;
}
.entry__journey:hover {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 50%, var(--vp-c-divider));
  background: var(--vp-c-bg-elv);
}
.entry__journey .entry__main-top { flex-shrink: 0; }
.entry__journey strong {
  flex-shrink: 0;
  margin: 0;
  color: var(--vp-c-text-1);
  font-size: 18px;
  font-weight: 700;
  letter-spacing: -0.02em;
}
.entry__journey p {
  flex: 1;
  min-width: 240px;
  margin: 0;
  color: var(--vp-c-text-2);
  font-size: 13px;
  line-height: 1.7;
}
.entry__journey .entry__go { flex-shrink: 0; margin-top: 0; }
.entry__journey:hover .entry__go i { transform: translateX(4px); }

@media (max-width: 900px) {
  .entry { grid-template-columns: 1fr; padding-top: 48px; }
  .entry__side { grid-template-rows: none; grid-template-columns: repeat(3, 1fr); }
  .entry__item { gap: 6px; }
}
@media (max-width: 639px) {
  .entry { padding: 40px 18px 0; gap: 10px; }
  .entry__main { padding: 24px 20px 22px; }
  .entry__main strong { font-size: 22px; }
  .entry__side { grid-template-columns: 1fr; }
  .entry__item { padding: 14px 16px; }
}
</style>
