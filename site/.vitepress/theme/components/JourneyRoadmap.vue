<script setup lang="ts">
import { withBase } from 'vitepress'

interface Stage {
  id: string
  eyebrow: string
  title: string
  desc: string
  status: 'open' | 'next' | 'planned'
  href?: string
  trail: string[]
}

const stages: Stage[] = [
  {
    id: '00',
    eyebrow: 'NOW · 已开放',
    title: '武器库',
    desc: '还没碰内核，先把 Result、格式引擎、断言与测试框架握在手里。',
    status: 'open',
    href: '/journey/00-armory/',
    trail: ['Result', 'format', 'assert', 'test'],
  },
  {
    id: '01',
    eyebrow: 'NEXT · 下一站',
    title: '点亮机器',
    desc: '从 512 字节的引导扇区出发，让处理器越过实模式，真正进入 64 位世界。',
    status: 'next',
    trail: ['0x7C00', 'GDT', 'paging', 'long mode'],
  },
  {
    id: '02',
    eyebrow: 'ACT I · 内核开始呼吸',
    title: '让它运行起来',
    desc: '接住中断、管理内存、调度进程，再把用户态与文件系统一点点接进来。',
    status: 'planned',
    trail: ['interrupt', 'memory', 'process', 'filesystem'],
  },
  {
    id: '03',
    eyebrow: 'ACT II · 长成系统',
    title: '让它走向真实世界',
    desc: '显示、输入、存储、多核与网络在这里汇合，最终长成一套可以交互的系统。',
    status: 'planned',
    trail: ['GUI', 'storage', 'SMP', 'network'],
  },
]

const currentStage = stages[0]!
const upcomingStages = stages.slice(1)
const currentHref = withBase(currentStage.href!)
</script>

<template>
  <section class="journey-map" aria-labelledby="journey-map-title">
    <header class="journey-map__head">
      <div>
        <span class="journey-map__kicker">CINUX / BUILD LOG</span>
        <h3 id="journey-map-title">沿着真实开发史，一站一站往前走</h3>
      </div>
      <p><i aria-hidden="true" /> 当前开放 1 站</p>
    </header>

    <div class="journey-map__layout">
      <a class="journey-current" :href="currentHref">
        <header class="journey-current__head">
          <span class="journey-current__node">{{ currentStage.id }}</span>
          <span class="journey-current__eyebrow">{{ currentStage.eyebrow }}</span>
        </header>

        <div class="journey-current__body">
          <span class="journey-current__label">CURRENT STATION</span>
          <strong>{{ currentStage.title }}</strong>
          <span class="journey-current__desc">{{ currentStage.desc }}</span>
          <span class="journey-current__trail">
            <span v-for="item in currentStage.trail" :key="item">{{ item }}</span>
          </span>
        </div>

        <span class="journey-current__go">进入这一站 <b aria-hidden="true">→</b></span>
      </a>

      <div class="journey-future">
        <div class="journey-future__title">
          <span>ROUTE / 接下来</span>
          <i aria-hidden="true" />
        </div>

        <ol>
          <li
            v-for="stage in upcomingStages"
            :key="stage.id"
            class="journey-next"
            :class="`is-${stage.status}`"
          >
            <span class="journey-next__node" aria-hidden="true">{{ stage.id }}</span>
            <div class="journey-next__copy">
              <span>{{ stage.eyebrow }}</span>
              <strong>{{ stage.title }}</strong>
              <small>{{ stage.desc }}</small>
            </div>
            <span class="journey-next__state">{{ stage.status === 'next' ? '正在装填' : '路线预告' }}</span>
          </li>
        </ol>
      </div>
    </div>
  </section>
</template>

<!-- BEM 类名已由 journey-* 命名空间隔离；保持全局可避免开发态重建组件后遗留旧 scoped hash。 -->
<style>
.journey-map {
  position: relative;
  margin: 1.55em 0 2.1em;
  padding: 22px;
  overflow: hidden;
  border: 1px solid var(--vp-c-divider);
  border-radius: 18px;
  background:
    radial-gradient(circle at 4% 0, color-mix(in srgb, var(--vp-c-brand-soft) 68%, transparent), transparent 30%),
    var(--vp-c-bg-soft);
}

.journey-map__head {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 18px;
  margin-bottom: 20px;
}

.journey-map__kicker {
  color: var(--vp-c-brand-1);
  font: 700 10px/1 var(--vp-font-family-mono);
  letter-spacing: .12em;
}

.journey-map__head h3 {
  margin: 7px 0 0;
  padding: 0;
  color: var(--vp-c-text-1);
  font-size: 19px;
  line-height: 1.4;
}

.journey-map__head h3::before { display: none; }

.journey-map__head p {
  display: flex;
  align-items: center;
  gap: 7px;
  margin: 0;
  color: var(--vp-c-text-3);
  font-size: 11.5px;
  white-space: nowrap;
}

.journey-map__head p i {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--vp-c-brand-1);
  box-shadow: 0 0 0 5px color-mix(in srgb, var(--vp-c-brand-1) 11%, transparent);
}

.journey-map__layout {
  display: grid;
  grid-template-columns: minmax(0, .9fr) minmax(0, 1.45fr);
  gap: 14px;
}

.journey-current {
  position: relative;
  display: flex;
  min-height: 300px;
  flex-direction: column;
  padding: 17px 18px 16px;
  overflow: hidden;
  border: 1px solid color-mix(in srgb, var(--vp-c-brand-1) 48%, var(--vp-c-divider));
  border-radius: 14px;
  background:
    linear-gradient(145deg, color-mix(in srgb, var(--vp-c-brand-soft) 58%, transparent), transparent 44%),
    color-mix(in srgb, var(--vp-c-bg) 94%, transparent);
  color: inherit;
  text-decoration: none !important;
  box-shadow: 0 12px 28px -22px color-mix(in srgb, var(--vp-c-brand-1) 60%, transparent);
  transition: border-color .2s ease, box-shadow .2s ease, transform .2s ease;
}

.journey-current::after {
  position: absolute;
  right: -30px;
  bottom: -40px;
  width: 126px;
  height: 126px;
  border: 1px solid color-mix(in srgb, var(--vp-c-brand-1) 10%, transparent);
  border-radius: 50%;
  box-shadow:
    0 0 0 18px color-mix(in srgb, var(--vp-c-brand-1) 4%, transparent),
    0 0 0 38px color-mix(in srgb, var(--vp-c-brand-1) 3%, transparent);
  content: '';
}

.journey-current__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.journey-current__node {
  display: grid;
  width: 38px;
  height: 38px;
  place-items: center;
  border-radius: 50%;
  background: var(--vp-c-brand-1);
  box-shadow: 0 0 0 6px color-mix(in srgb, var(--vp-c-brand-1) 11%, transparent);
  color: var(--vp-c-bg);
  font: 700 10px/1 var(--vp-font-family-mono);
}

.journey-current__eyebrow,
.journey-current__label,
.journey-future__title span,
.journey-next__copy > span {
  color: var(--vp-c-brand-1);
  font: 700 9px/1.3 var(--vp-font-family-mono);
  letter-spacing: .1em;
}

.journey-current__body {
  display: flex;
  flex-direction: column;
  margin-top: 25px;
}

.journey-current__label {
  color: var(--vp-c-text-3);
  font-size: 8px;
}

.journey-current__body > strong {
  margin-top: 7px;
  color: var(--vp-c-text-1);
  font-size: 20px;
  line-height: 1.35;
}

.journey-current__desc {
  margin-top: 8px;
  color: var(--vp-c-text-2);
  font-size: 12px;
  line-height: 1.7;
}

.journey-current__trail {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
  margin-top: 13px;
}

.journey-current__trail span {
  padding: 2px 6px;
  border-radius: 4px;
  background: var(--vp-c-bg-alt);
  color: var(--vp-c-text-3);
  font: 500 9px/1.5 var(--vp-font-family-mono);
}

.journey-current__go {
  z-index: 1;
  margin-top: auto;
  padding-top: 16px;
  color: var(--vp-c-brand-1);
  font-size: 11.5px;
  font-weight: 650;
}

.journey-current__go b {
  display: inline-block;
  margin-left: 4px;
  transition: transform .2s ease;
}

.journey-future {
  padding: 15px 16px 12px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px;
  background: color-mix(in srgb, var(--vp-c-bg) 84%, transparent);
}

.journey-future__title {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 3px;
}

.journey-future__title i {
  height: 1px;
  flex: 1;
  background: linear-gradient(90deg, var(--vp-c-divider), transparent);
}

.journey-future ol {
  margin: 0;
  padding: 0;
  border: 0;
  background: none;
  box-shadow: none;
  list-style: none;
}

.journey-next {
  position: relative;
  display: grid;
  grid-template-columns: 36px minmax(0, 1fr) auto;
  align-items: center;
  gap: 12px;
  min-height: 78px;
  margin: 0;
  padding: 11px 2px;
}

.journey-next + .journey-next {
  border-top: 1px solid var(--vp-c-divider);
}

.journey-next:not(:last-child)::after {
  position: absolute;
  bottom: -7px;
  left: 17px;
  z-index: 1;
  color: var(--vp-c-text-3);
  content: '↓';
  font: 500 10px/1 var(--vp-font-family-mono);
}

.journey-next__node {
  display: grid;
  width: 34px;
  height: 34px;
  place-items: center;
  border: 1.5px solid color-mix(in srgb, var(--vp-c-brand-1) 25%, var(--vp-c-divider));
  border-radius: 50%;
  color: var(--vp-c-text-3);
  font: 700 9px/1 var(--vp-font-family-mono);
}

.journey-next.is-next .journey-next__node {
  border-color: var(--vp-c-brand-1);
  border-style: dashed;
  color: var(--vp-c-brand-1);
}

.journey-next__copy {
  display: grid;
  min-width: 0;
  gap: 3px;
}

.journey-next__copy strong {
  color: var(--vp-c-text-1);
  font-size: 13px;
  line-height: 1.35;
}

.journey-next__copy small {
  overflow: hidden;
  color: var(--vp-c-text-3);
  font-size: 10.5px;
  line-height: 1.45;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.journey-next__state {
  padding: 3px 7px;
  border-radius: 99px;
  background: var(--vp-c-bg-alt);
  color: var(--vp-c-text-3);
  font-size: 9px;
  white-space: nowrap;
}

.journey-next.is-next .journey-next__state {
  background: var(--vp-c-brand-soft);
  color: var(--vp-c-brand-1);
}

.journey-next.is-planned { opacity: .7; }

@media (hover: hover) and (pointer: fine) and (prefers-reduced-motion: no-preference) {
  .journey-current:hover {
    border-color: var(--vp-c-brand-1);
    box-shadow: 0 18px 34px -24px color-mix(in srgb, var(--vp-c-brand-1) 70%, transparent);
    transform: translateY(-2px);
  }

  .journey-current:hover .journey-current__go b { transform: translateX(3px); }
}

@media (max-width: 767px) {
  .journey-map { padding: 18px 15px; }
  .journey-map__head { align-items: flex-start; }
  .journey-map__head p { display: none; }
  .journey-map__layout { grid-template-columns: 1fr; }
  .journey-current { min-height: 252px; }
  .journey-next { grid-template-columns: 34px minmax(0, 1fr); }
  .journey-next__state { display: none; }
}

@media (max-width: 420px) {
  .journey-next__copy small {
    display: -webkit-box;
    overflow: hidden;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    white-space: normal;
  }
}

@media print {
  .journey-map { background: none; }
  .journey-current { box-shadow: none; }
}
</style>
