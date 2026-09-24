<script setup lang="ts">
import { useData } from 'vitepress'
import { computed } from 'vue'

const { page, theme, site, frontmatter } = useData()
const base = site.value.base

interface FlatPage {
  text: string
  link: string
}

function flattenSidebar(items: any[], result: FlatPage[] = []): FlatPage[] {
  for (const item of items) {
    if (item.link) {
      result.push({ text: item.text, link: item.link })
    }
    if (item.items) {
      flattenSidebar(item.items, result)
    }
  }
  return result
}

function normalize(link: string): string {
  return link.replace(/\.md$/, '').replace(/\/index$/, '/').replace(/\/$/, '')
}

const prefixLink = (p: FlatPage) => ({
  text: p.text,
  link: (base + p.link.replace(/^\//, '')).replace(/\/\//g, '/'),
  raw: p.link,
})

// 构建期注入的邻居(config/shared.ts attachDocNav):SSR 与 hydration 拿到
// 同一份数据,分卷构建的跨卷边界页也有卡片。注入缺失(异常页)时回退到
// 从 theme.sidebar 摊平推导。
const navInfo = computed(() => {
  const fm = frontmatter.value.docNav as
    | { prev: FlatPage | null; next: FlatPage | null }
    | undefined

  if (fm) {
    return {
      prev: fm.prev ? prefixLink(fm.prev) : null,
      next: fm.next ? prefixLink(fm.next) : null,
    }
  }

  const sidebar = theme.value.sidebar
  if (!sidebar) return null

  let allPages: FlatPage[] = []
  for (const key of Object.keys(sidebar)) {
    const group = sidebar[key]
    if (Array.isArray(group)) {
      flattenSidebar(group, allPages)
    }
  }

  if (allPages.length === 0) return null

  // Try multiple path formats to find current page
  const relPath = page.value.relativePath // e.g. "tutorial/uboot/07_network_porting.md"
  const candidates = [
    normalize('/' + relPath),              // /tutorial/uboot/07_network_porting
    normalize('/' + relPath.replace(/\.md$/, '') + '/'), // for index pages
  ]

  let idx = -1
  for (const candidate of candidates) {
    idx = allPages.findIndex(p => normalize(p.link) === candidate)
    if (idx >= 0) break
  }

  if (idx < 0) {
    // Fallback: try matching the end of the path
    const endPath = relPath.replace(/\.md$/, '').replace(/\/index$/, '')
    idx = allPages.findIndex(p => normalize(p.link).endsWith(endPath))
  }

  if (idx < 0) return null

  return {
    prev: idx > 0 ? prefixLink(allPages[idx - 1]) : null,
    next: idx < allPages.length - 1 ? prefixLink(allPages[idx + 1]) : null
  }
})

// 跨册(primer ↔ book ↔ labs ↔ …)时标签换成「上/下一册」,
// 读者从卷末翻到下一卷时能明确感知换了册,而不是误以为还是同一章。
// 卷归属用注入的原始链接(不带 base)首段判断,与页面 relativePath 同一坐标系。
function volOf(raw: string | null | undefined): string {
  return raw ? raw.replace(/^\//, '').split('/')[0] : ''
}
const curVol = computed(() => page.value.relativePath.split('/')[0])
const prevLabel = computed(() =>
  navInfo.value?.prev && volOf((navInfo.value.prev as any).raw) !== curVol.value ? '← 上一册' : '← 上一章',
)
const nextLabel = computed(() =>
  navInfo.value?.next && volOf((navInfo.value.next as any).raw) !== curVol.value ? '下一册 →' : '下一章 →',
)
</script>

<template>
  <div v-if="navInfo && (navInfo.prev || navInfo.next)" class="doc-nav-cards">
    <a v-if="navInfo.prev" :href="navInfo.prev.link" class="doc-nav-card doc-nav-card--prev">
      <span class="doc-nav-card-label">{{ prevLabel }}</span>
      <span class="doc-nav-card-title">{{ navInfo.prev.text }}</span>
    </a>
    <span v-else class="doc-nav-card doc-nav-card--placeholder" />
    <a v-if="navInfo.next" :href="navInfo.next.link" class="doc-nav-card doc-nav-card--next">
      <span class="doc-nav-card-label">{{ nextLabel }}</span>
      <span class="doc-nav-card-title">{{ navInfo.next.text }}</span>
    </a>
  </div>
</template>

<style scoped>
.doc-nav-cards {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
  margin-top: 2.2em;
  padding-top: 1.4em;
  border-top: 1px solid var(--vp-c-divider);
}

/* 章节流转卡是页尾的"下一步",不做投影与抬起——
   与正文其余元素保持同一套安静的语言。 */
.doc-nav-card {
  display: flex;
  flex-direction: column;
  gap: 5px;
  padding: 14px 16px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 9px;
  background-color: var(--vp-c-bg-alt);
  box-shadow: none;
  text-decoration: none !important;
  color: inherit;
  transition: border-color 0.18s ease, background-color 0.18s ease;
}

.doc-nav-card:hover {
  border-color: var(--vp-c-brand-1);
  background-color: var(--vp-c-brand-soft);
}

.doc-nav-card--next {
  text-align: right;
}

.doc-nav-card--placeholder {
  visibility: hidden;
}

.doc-nav-card-label {
  font-size: 12px;
  font-weight: 500;
  color: var(--vp-c-brand-1);
  line-height: 1.4;
}

.doc-nav-card-title {
  font-size: 14px;
  font-weight: 500;
  color: var(--vp-c-text-1);
  line-height: 1.5;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
  transition: color 0.35s ease;
}

.doc-nav-card:hover .doc-nav-card-title {
  color: var(--vp-c-brand-1);
}

.dark .doc-nav-card {
  border-color: var(--vp-c-border);
}

@media (max-width: 639px) {
  .doc-nav-cards {
    grid-template-columns: 1fr;
  }

  .doc-nav-card--next {
    text-align: left;
  }

  .doc-nav-card--placeholder {
    display: none;
  }
}
</style>
