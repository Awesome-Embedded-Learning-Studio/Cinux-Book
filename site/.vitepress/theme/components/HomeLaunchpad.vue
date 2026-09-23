<script setup lang="ts">
import { withBase } from 'vitepress'

const routes = [
  {
    id: '01', kind: 'primary', eyebrow: 'MAIN BOOK', title: '从第一条指令开始',
    note: '顺着 17 卷主线，把启动、内存、进程、文件系统、桌面与网络连成一台完整的机器。',
    action: '进入主书', href: '/book/', signal: 'BOOT → KERNEL → USERLAND',
  },
  {
    id: '02', kind: 'lab', eyebrow: 'LABS', title: '亲手验证',
    note: '手算地址、补全接口、复现实验。', action: '打开实验册', href: '/labs/', signal: 'BUILD / RUN / CHECK',
  },
  {
    id: '03', kind: 'debug', eyebrow: 'DEBUG LOG', title: '走进故障现场',
    note: '从症状到根因，看真实内核问题如何收敛。', action: '阅读排错笔记', href: '/debug-notes/', signal: 'TRACE / ISOLATE / FIX',
  },
  {
    id: '04', kind: 'reference', eyebrow: 'REFERENCE', title: '快速查参数',
    note: '寄存器、接口、边界与源码索引。', action: '查阅参考手册', href: '/reference/', signal: 'LOOKUP / VERIFY',
  },
]
</script>

<template>
  <section class="launchpad">
    <header class="launchpad__head">
      <span>CHOOSE YOUR ENTRY POINT</span>
      <h2>第一次来，从主线开始；卡住时，切到实验和排错</h2>
    </header>
    <div class="launchpad__grid">
      <a
        v-for="route in routes"
        :key="route.id"
        :href="withBase(route.href)"
        class="launch-card"
        :class="`launch-card--${route.kind}`"
      >
        <span class="launch-card__id">{{ route.id }}</span>
        <span class="launch-card__eyebrow">{{ route.eyebrow }}</span>
        <strong>{{ route.title }}</strong>
        <p>{{ route.note }}</p>
        <span class="launch-card__signal">{{ route.signal }}</span>
        <span class="launch-card__action">{{ route.action }} <i>↗</i></span>
      </a>
    </div>
  </section>
</template>

<style scoped>
.launchpad { max-width: 1152px; margin: 0 auto; padding: 12px 24px 68px; }
.launchpad__head { display: grid; grid-template-columns: 230px minmax(0, 1fr); gap: 28px; align-items: start; margin-bottom: 24px; }
.launchpad__head > span { padding-top: 7px; color: var(--vp-c-brand-1); font: 700 11px/1 var(--vp-font-family-mono); letter-spacing: 0.13em; }
.launchpad__head h2 { max-width: 720px; margin: 0; color: var(--vp-c-text-1); font-size: clamp(24px, 3vw, 36px); line-height: 1.22; letter-spacing: -0.035em; }
.launchpad__grid { display: grid; grid-template-columns: 1.5fr 1fr 1fr; grid-template-rows: repeat(2, minmax(155px, auto)); gap: 10px; }
.launch-card {
  --card-accent: var(--vp-c-brand-1);
  position: relative;
  display: flex;
  min-width: 0;
  flex-direction: column;
  padding: 22px;
  overflow: hidden;
  color: inherit;
  background: var(--vp-c-bg-soft);
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px 4px 14px 4px;
  text-decoration: none;
  transition: transform 0.2s ease, border-color 0.2s ease, box-shadow 0.2s ease;
}
.launch-card::before { position: absolute; top: 0; right: 0; width: 54px; height: 3px; background: var(--card-accent); content: ''; }
.launch-card:hover { transform: translateY(-3px); border-color: color-mix(in srgb, var(--card-accent) 45%, var(--vp-c-divider)); box-shadow: 0 15px 36px rgba(14, 56, 47, 0.1); }
.launch-card--primary { grid-row: 1 / 3; padding: 28px; color: #dffff7; background: linear-gradient(145deg, #071713, #0b3028); border-color: rgba(69, 211, 181, 0.28); }
.launch-card--primary::after { position: absolute; right: -50px; bottom: -65px; width: 230px; height: 230px; border: 1px solid rgba(81, 223, 193, 0.16); border-radius: 50%; box-shadow: inset 0 0 0 24px rgba(81, 223, 193, 0.025), inset 0 0 0 52px rgba(81, 223, 193, 0.025); content: ''; }
.launch-card--debug { --card-accent: #b56b20; }
.launch-card--reference { --card-accent: #7659af; }
.launch-card__id { position: absolute; top: 18px; right: 17px; color: var(--vp-c-text-3); font: 600 10px/1 var(--vp-font-family-mono); }
.launch-card--primary .launch-card__id { color: rgba(201, 247, 237, 0.45); }
.launch-card__eyebrow { color: var(--card-accent); font: 700 10px/1 var(--vp-font-family-mono); letter-spacing: 0.13em; }
.launch-card--primary .launch-card__eyebrow { color: #55dfc1; }
.launch-card strong { margin-top: 18px; color: var(--vp-c-text-1); font-size: 19px; line-height: 1.3; }
.launch-card--primary strong { max-width: 330px; margin-top: 48px; color: #f1fffc; font-size: clamp(26px, 3vw, 39px); line-height: 1.12; letter-spacing: -0.04em; }
.launch-card p { max-width: 400px; margin: 8px 0 20px; color: var(--vp-c-text-2); font-size: 13px; line-height: 1.7; }
.launch-card--primary p { color: #9dc8bd; font-size: 14px; }
.launch-card__signal { margin-top: auto; color: var(--vp-c-text-3); font: 600 9px/1 var(--vp-font-family-mono); letter-spacing: 0.1em; }
.launch-card--primary .launch-card__signal { color: #55867b; }
.launch-card__action { display: flex; align-items: center; justify-content: space-between; margin-top: 18px; color: var(--card-accent); font-size: 12px; font-weight: 700; }
.launch-card--primary .launch-card__action { position: relative; z-index: 1; color: #59e1c3; }
.launch-card__action i { font-style: normal; transition: transform 0.18s ease; }
.launch-card:hover .launch-card__action i { transform: translate(2px, -2px); }

@media (max-width: 900px) {
  .launchpad__grid { grid-template-columns: 1fr 1fr; grid-template-rows: auto; }
  .launch-card--primary { grid-column: 1 / -1; grid-row: auto; min-height: 270px; }
  .launch-card--reference { grid-column: 1 / -1; }
}
@media (max-width: 639px) {
  .launchpad { padding: 8px 18px 48px; }
  .launchpad__head { grid-template-columns: 1fr; gap: 10px; }
  .launchpad__head h2 { font-size: 26px; }
  .launchpad__grid { grid-template-columns: 1fr; }
  .launch-card--primary,
  .launch-card--reference { grid-column: auto; min-height: 235px; }
}
</style>
