<script setup lang="ts">
/**
 * 首页「系统构建路线」双视图区块。
 *
 * 左 Tab = HomePathGraph（按真实开发史展开的构建时间线，默认页）；
 * 右 Tab = HomeFeatureGrid（按用途分组的内容板块）。两页也保留 scroll-snap，
 * 移动端可横滑；Tab 使用标准 tablist/tab/tabpanel 语义与方向键交互。
 *
 * 手势分工：pager 在外层、panzoom 在视图二内部——图内横滑归图（panzoom 平移），
 * 翻页靠 Tab / 卡片视图区滑动。inert 屏蔽非活动页的焦点，但只在
 * 客户端 mounted 后设置（SSR HTML 里两页都可点，保证无 JS / 首屏可访问性）。
 */
import { computed, onMounted, ref } from 'vue'
import { useData, withBase } from 'vitepress'
import HomePathGraph from './HomePathGraph.vue'
import HomeFeatureGrid from './HomeFeatureGrid.vue'

const { lang } = useData()
const isEn = computed(() => lang.value.startsWith('en'))

const t = computed(() =>
  isEn.value
    ? {
        title: 'Explore Cinux',
        timelineTab: 'Build timeline',
        sectionsTab: 'Sections',
        timelineNote: 'Drag nodes · Hover to trace · Click to jump',
        sectionsNote: 'Pick a section and enter directly',
        timelineLabel: 'Cinux interactive build timeline',
        sectionsLabel: 'Cinux content sections',
        cta: 'Enter the journey →',
        ctaLink: '/journey/',
      }
    : {
        title: '探索 Cinux',
        timelineTab: '构建时间线',
        sectionsTab: '内容板块',
        timelineNote: '节点能拖 · 画布能缩放 · 悬停追溯来路 · 点击直达',
        sectionsNote: '按用途选一扇舱门，直接进入对应板块',
        timelineLabel: 'Cinux 交互式构建时间线',
        sectionsLabel: 'Cinux 内容板块',
        cta: '进入完整旅程 →',
        ctaLink: '/journey/',
      },
)

/* ── pager ↔ Tabs 状态同步；时间线是更能表达 Cinux 叙事的默认页 ── */
const active = ref(0)
const mounted = ref(false)
const pagerEl = ref<HTMLElement | null>(null)
const tablistEl = ref<HTMLElement | null>(null)
let rafPending = false

function onScroll() {
  if (rafPending || !pagerEl.value) return
  rafPending = true
  requestAnimationFrame(() => {
    rafPending = false
    const el = pagerEl.value
    if (!el || !el.clientWidth) return
    const i = Math.round(el.scrollLeft / el.clientWidth)
    if (i !== active.value) active.value = i
  })
}

function select(i: number) {
  const el = pagerEl.value
  if (!el) return
  /* Tab 点击时短暂关闭 snap，直接落到目标页；否则 Chromium 会在程序化反向滚动
     越过中点前吸回原页。下一帧恢复，手指横滑仍保留原生滚动与 snap 动效。 */
  el.style.scrollSnapType = 'none'
  el.style.scrollBehavior = 'auto'
  el.scrollLeft = i * el.clientWidth
  active.value = i
  requestAnimationFrame(() => {
    el.style.removeProperty('scroll-snap-type')
    el.style.removeProperty('scroll-behavior')
  })
}

function onTabKeydown(ev: KeyboardEvent) {
  let next: number | null = null
  if (ev.key === 'ArrowLeft' || ev.key === 'Home') next = 0
  if (ev.key === 'ArrowRight' || ev.key === 'End') next = 1
  if (next === null) return
  ev.preventDefault()
  select(next)
  requestAnimationFrame(() => {
    tablistEl.value?.querySelectorAll<HTMLButtonElement>('[role="tab"]')[next!]?.focus()
  })
}

onMounted(() => {
  /* mounted 后才设 inert：SSR HTML 保持两页可交互（无 JS / 爬虫可达） */
  mounted.value = true
})
</script>

<template>
  <section id="roadmap" class="home-path">
    <header class="hp-head">
      <h2 class="hp-title">🧭 {{ t.title }}</h2>
      <p class="hp-note">{{ active === 0 ? t.timelineNote : t.sectionsNote }}</p>

      <a class="hp-cta" :href="withBase(t.ctaLink)">{{ t.cta }}</a>
    </header>

    <div
      ref="tablistEl"
      class="hp-tabs"
      role="tablist"
      :aria-label="t.title"
      @keydown="onTabKeydown"
    >
      <button
        id="hp-tab-timeline"
        type="button"
        role="tab"
        class="hp-tab"
        :class="{ 'is-active': active === 0 }"
        aria-controls="hp-panel-timeline"
        :aria-selected="active === 0"
        :tabindex="active === 0 ? 0 : -1"
        @click="select(0)"
      >
        <span class="hp-tab__mark" aria-hidden="true">01</span>
        <span>{{ t.timelineTab }}</span>
      </button>
      <button
        id="hp-tab-sections"
        type="button"
        role="tab"
        class="hp-tab"
        :class="{ 'is-active': active === 1 }"
        aria-controls="hp-panel-sections"
        :aria-selected="active === 1"
        :tabindex="active === 1 ? 0 : -1"
        @click="select(1)"
      >
        <span class="hp-tab__mark" aria-hidden="true">02</span>
        <span>{{ t.sectionsTab }}</span>
      </button>
    </div>

    <div ref="pagerEl" class="hp-pager" @scroll.passive="onScroll">
      <div
        id="hp-panel-timeline"
        role="tabpanel"
        aria-labelledby="hp-tab-timeline"
        :aria-label="t.timelineLabel"
        :aria-hidden="mounted && active !== 0"
        class="hp-slide"
        :inert="mounted && active !== 0"
      >
        <HomePathGraph />
      </div>
      <div
        id="hp-panel-sections"
        role="tabpanel"
        aria-labelledby="hp-tab-sections"
        :aria-label="t.sectionsLabel"
        :aria-hidden="mounted && active !== 1"
        class="hp-slide"
        :inert="mounted && active !== 1"
      >
        <HomeFeatureGrid />
      </div>
    </div>
  </section>
</template>

<style scoped>
.home-path {
  max-width: 1152px;
  margin: 40px auto 56px;
  padding: 0 24px;
  scroll-margin-top: 80px;
  animation: hp-fade-up 0.7s cubic-bezier(0.25, 0.46, 0.45, 0.94) both;
}

.hp-head {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 10px 16px;
  margin-bottom: 16px;
}

.hp-title {
  margin: 0;
  font-size: 20px;
  font-weight: 700;
  line-height: 1.4;
  color: var(--vp-c-text-1);
}

.hp-note {
  margin: 0;
  font-size: 13px;
  color: var(--vp-c-text-3);
}

/* 两个同权视图：tab 本身就是标题栏，不再用单向 Banner 暗示层级。 */
.hp-tabs {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 4px;
  margin: 0 0 12px;
  padding: 4px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 12px;
  background: var(--vp-c-bg-soft);
}

.hp-tab {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 9px;
  min-height: 42px;
  padding: 8px 14px;
  border: 1px solid transparent;
  border-radius: 8px;
  background: transparent;
  color: var(--vp-c-text-2);
  font-family: var(--vp-font-family);
  font-size: 13.5px;
  font-weight: 600;
  line-height: 1.4;
  cursor: pointer;
  transition: border-color 0.2s ease, color 0.2s ease, background 0.2s ease, box-shadow 0.2s ease;
}

.hp-tab:hover {
  color: var(--vp-c-brand-1);
  background: color-mix(in srgb, var(--vp-c-brand-soft) 58%, transparent);
}

.hp-tab.is-active {
  color: var(--vp-c-brand-1);
  background: var(--vp-c-bg);
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 32%, var(--vp-c-divider));
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.06);
}

.hp-tab:focus-visible {
  outline: 2px solid var(--vp-c-brand-1);
  outline-offset: 2px;
}

.hp-tab__mark {
  color: var(--vp-c-text-3);
  font: 700 9px/1 var(--vp-font-family-mono);
  letter-spacing: 0.08em;
}

.hp-tab.is-active .hp-tab__mark {
  color: var(--vp-c-brand-1);
}

.hp-cta {
  margin-left: auto;
  font-size: 13px;
  font-weight: 600;
  color: var(--vp-c-brand-1);
  text-decoration: none;
  white-space: nowrap;
}

.hp-cta:hover {
  text-decoration: underline;
}

/* 双页 pager：scroll-snap 原生横滑翻页，Tab 同步活动页。 */
.hp-pager {
  display: flex;
  overflow-x: auto;
  scroll-snap-type: x mandatory;
  overscroll-behavior-x: contain;
  scrollbar-width: none;
  -webkit-overflow-scrolling: touch;
}

.hp-pager::-webkit-scrollbar {
  display: none;
}

.hp-slide {
  flex: 0 0 100%;
  min-width: 100%;
  scroll-snap-align: center;
}

@keyframes hp-fade-up {
  from {
    opacity: 0;
    transform: translateY(18px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

@media (prefers-reduced-motion: reduce) {
  .home-path {
    animation: none !important;
  }

  .hp-tab {
    transition: none;
  }
}

@media (max-width: 639px) {
  .home-path {
    padding: 0 16px;
    margin: 28px auto 36px;
  }

  .hp-note {
    display: none;
  }

  .hp-tab {
    min-height: 40px;
    padding: 8px 10px;
    font-size: 13px;
  }

  .hp-cta {
    margin-left: auto;
  }
}
</style>
