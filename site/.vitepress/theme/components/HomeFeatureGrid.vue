<script setup lang="ts">
/**
 * 首页路线卡片。骨架来自 TAMCPP 的 HomeFeatureGrid，内容改为 Cinux 当前
 * 可以真实抵达的四扇「舱门」，避免尚在重写的站点产生空链接。
 */
import { withBase } from 'vitepress'

interface Feature {
  eyebrow: string
  icon: string
  title: string
  details: string
  link: string
  linkText: string
  tone: 'boot' | 'core' | 'lab' | 'signal'
}

const features: Feature[] = [
  {
    eyebrow: 'MAIN / 00', icon: '⌁', title: '第一站 · 武器库',
    details: 'Result、格式引擎、断言和测试框架。先造四件后面会反复使用的工具。',
    link: '/journey/00-armory/', linkText: '从第一站出发', tone: 'core',
  },
  {
    eyebrow: 'PREP / 01—03', icon: '⌘', title: '前置卷',
    details: '工具链、汇编与受约束的 C++。哪块心里没底，就从那条支线补给。',
    link: '/primer/', linkText: '检查行装', tone: 'boot',
  },
  {
    eyebrow: 'FORENSICS', icon: '⌕', title: '排错现场',
    details: '不只给答案：顺着真实故障留下的痕迹，看一次问题如何被定位和收敛。',
    link: '/debug-notes/', linkText: '进入故障现场', tone: 'lab',
  },
  {
    eyebrow: 'SIGNAL / INDEX', icon: '⋮', title: '参考与索引',
    details: '需要寄存器、边界或接口的精确答案时，从参考手册和标签索引横切进去。',
    link: '/reference/', linkText: '打开参考台', tone: 'signal',
  },
]
</script>

<template>
  <div class="VPFeatures home-cards">
    <div class="container">
      <div class="items">
        <div
          v-for="f in features"
          :key="f.title"
          class="item"
          :class="[`tone-${f.tone}`]"
        >
          <component
            :is="'a'"
            class="VPFeature link"
            :href="withBase(f.link)"
          >
            <article class="box">
              <div class="card-top">
                <span class="eyebrow">{{ f.eyebrow }}</span>
                <span class="icon" aria-hidden="true">{{ f.icon }}</span>
              </div>
              <h2 class="title">{{ f.title }}</h2>
              <p class="details">{{ f.details }}</p>
              <div class="link-text">
                <p class="link-text-value">
                  {{ f.linkText }}
                  <svg
                    class="link-text-icon"
                    xmlns="http://www.w3.org/2000/svg"
                    width="14"
                    height="14"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  ><path d="M5 12h14M12 5l7 7-7 7" /></svg>
                </p>
              </div>
            </article>
          </component>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.VPFeatures {
  position: relative;
  padding: 0;
}

.container {
  margin: 0 auto;
  max-width: 1152px;
}

.items {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}

.item {
  min-width: 0;
}

.VPFeature {
  --card-tone: var(--vp-c-brand-1);
  position: relative;
  display: block;
  height: 100%;
  overflow: hidden;
  color: inherit;
  background: var(--vp-c-bg);
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px;
  text-decoration: none;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04);
  transition: transform 0.24s ease, border-color 0.24s ease, box-shadow 0.24s ease;
}

.VPFeature::before {
  position: absolute;
  inset: 0 auto 0 0;
  width: 4px;
  background: var(--card-tone);
  opacity: 0.65;
  content: '';
}

.tone-boot .VPFeature { --card-tone: #b06a1f; }
.tone-core .VPFeature { --card-tone: var(--vp-c-brand-1); }
.tone-lab .VPFeature { --card-tone: #b34f62; }
.tone-signal .VPFeature { --card-tone: #7659af; }

.VPFeature:hover {
  border-color: var(--card-tone);
  box-shadow: 0 12px 28px rgba(0, 0, 0, 0.08);
  transform: translateY(-3px);
}

.box {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 194px;
  padding: 22px 24px 20px 27px;
}

.card-top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 17px;
}

.eyebrow {
  color: var(--card-tone);
  font: 700 10.5px/1 var(--vp-font-family-mono);
  letter-spacing: 0.1em;
}

.icon {
  display: grid;
  width: 34px;
  height: 34px;
  place-items: center;
  color: var(--card-tone);
  background: color-mix(in srgb, var(--card-tone) 10%, transparent);
  border: 1px solid color-mix(in srgb, var(--card-tone) 24%, transparent);
  border-radius: 10px 3px 10px 3px;
  font: 700 18px/1 var(--vp-font-family-mono);
}

.title {
  line-height: 24px;
  font-size: 16px;
  font-weight: 600;
}

.details {
  flex-grow: 1;
  padding-top: 8px;
  color: var(--vp-c-text-2);
  font-size: 13px;
  line-height: 1.7;
  font-weight: 400;
}

.link-text {
  padding-top: 8px;
}

.link-text-value {
  display: flex;
  align-items: center;
  font-size: 14px;
  font-weight: 500;
  color: var(--card-tone);
}

.link-text-icon {
  margin-left: 6px;
  flex-shrink: 0;
  transition: transform 0.2s ease;
}

.VPFeature:hover .link-text-icon {
  transform: translateX(4px);
}

@media (max-width: 639px) {
  .items { grid-template-columns: 1fr; }
  .box { min-height: 0; padding: 19px 19px 18px 22px; }
}

@media (prefers-reduced-motion: reduce) {
  .VPFeature,
  .link-text-icon { transition: none; }
}
</style>
