import { nextTick, onMounted, onUnmounted } from 'vue'
import { subscribeAfterRouteChange } from './router-hooks'
import { attachZoomButton, resetZoomButton } from './figure-zoom'

// mermaid 的 API 形状(只取我们用到的两个方法),避免依赖整包类型
type MermaidApi = {
  initialize: (config: Record<string, unknown>) => void
  render: (id: string, text: string) => Promise<{ svg: string; bindFunctions?: (el: Element) => void }>
}

// 模块级缓存:mermaid 包本身只 import 一次(主题切换时重新 initialize 即可)。
let mermaidPromise: Promise<MermaidApi> | null = null
// 当前已应用的配色('light' | 'dark'),用于判断是否需要重新 initialize + 重渲。
let appliedScheme: string | null = null

function currentScheme(): 'light' | 'dark' {
  return document.documentElement.classList.contains('dark') ? 'dark' : 'light'
}

/**
 * 从已生效的 CSS 变量取色,让流程图跟着「暖纸 · 冰川青」走。
 *
 * 直接读计算样式而不是在 TS 里写死色值:配色以后只在 custom.css 改一处,
 * 流程图自动跟随,不会再出现「站点换了暖纸、图还是冷白」的割裂。
 */
function paletteFromCss() {
  const s = getComputedStyle(document.documentElement)
  const v = (name: string, fallback: string) => s.getPropertyValue(name).trim() || fallback
  const dark = currentScheme() === 'dark'

  return {
    // 节点:纸面稍抬一层,描边用品牌青
    surface: v('--vp-c-bg-elv', dark ? '#272119' : '#FEFBF4'),
    surfaceAlt: v('--vp-c-bg-alt', dark ? '#201B14' : '#F1EBDD'),
    brand: v('--vp-c-brand-1', dark ? '#3AD6B8' : '#0E8A76'),
    ink: v('--vp-c-text-1', dark ? '#EDE6D6' : '#2C2620'),
    inkSoft: v('--vp-c-text-2', dark ? '#B5AB96' : '#57503F'),
    line: v('--vp-c-border', dark ? '#3B3428' : '#E4DBC7'),
    amber: v('--vp-accent-amber', dark ? '#E8A33D' : '#B96A0B'),
    dark,
  }
}

function mermaidThemeVariables() {
  const p = paletteFromCss()
  return {
    fontSize: '15px',
    fontFamily: 'var(--vp-font-family-base)',

    // 主体节点
    background: 'transparent',
    primaryColor: p.surfaceAlt,
    primaryTextColor: p.ink,
    primaryBorderColor: p.brand,
    secondaryColor: p.surface,
    secondaryTextColor: p.ink,
    secondaryBorderColor: p.line,
    tertiaryColor: p.surface,
    tertiaryTextColor: p.inkSoft,
    tertiaryBorderColor: p.line,

    // 连线与标签
    lineColor: p.dark ? p.inkSoft : p.brand,
    textColor: p.ink,
    edgeLabelBackground: p.surface,

    // 时序图 / 状态图常用项
    noteBkgColor: p.dark ? 'rgba(232,163,61,0.14)' : 'rgba(185,106,11,0.10)',
    noteTextColor: p.ink,
    noteBorderColor: p.amber,
    actorBkg: p.surfaceAlt,
    actorBorder: p.brand,
    actorTextColor: p.ink,
    labelBoxBkgColor: p.surfaceAlt,
    labelBoxBorderColor: p.line,
    labelTextColor: p.ink,
    clusterBkg: p.dark ? 'rgba(255,255,255,0.03)' : 'rgba(64,53,36,0.035)',
    clusterBorder: p.line,
    nodeBorder: p.brand,
    mainBkg: p.surfaceAlt,
  }
}

function loadMermaid(): Promise<MermaidApi> {
  if (typeof window === 'undefined') return Promise.reject(new Error('SSR 环境不加载 mermaid'))
  if (!mermaidPromise) {
    // 动态 import → Vite 把 mermaid 打进客户端异步 chunk(同源 + hash 缓存,离线/被墙都能渲染);
    // config 里的 ssr.external: ['mermaid'] 让 SSR build 不求值 mermaid,运行时 onMounted 才取。
    mermaidPromise = import('mermaid').then((mod) =>
      ((mod as { default?: MermaidApi }).default ?? (mod as unknown as MermaidApi))
    )
  }
  return mermaidPromise
}

/** 按当前明暗模式配置 mermaid;配色未变则跳过。返回是否发生了切换。 */
function applyTheme(mermaid: MermaidApi): boolean {
  const scheme = currentScheme()
  if (appliedScheme === scheme) return false

  mermaid.initialize({
    startOnLoad: false,
    securityLevel: 'loose',
    // base + themeVariables 才会真正吃我们的配色;default 会强推它自带的淡紫蓝
    theme: 'base',
    flowchart: {
      htmlLabels: true,
      nodeSpacing: 50,
      rankSpacing: 50,
      padding: 15,
      curve: 'basis',
    },
    themeVariables: mermaidThemeVariables(),
  })
  appliedScheme = scheme
  return true
}

async function renderMermaidDiagrams(force = false) {
  if (typeof window === 'undefined') return

  const pending = document.querySelectorAll('.mermaid-diagram[data-rendered="false"]')
  const anyDiagram = document.querySelector('.mermaid-diagram')
  // 页面上没有图就别为了初始化去拉 mermaid 包
  if (!anyDiagram || (!force && pending.length === 0)) return

  let mermaid: MermaidApi
  try {
    mermaid = await loadMermaid()
  } catch (e) {
    console.error('[mermaid] 运行时加载失败', e)
    return
  }

  const schemeChanged = applyTheme(mermaid)
  if (force || schemeChanged) {
    // 主题变了:把已渲染的图标记为待渲染,用原始源码重画一遍
    document.querySelectorAll<HTMLElement>('.mermaid-diagram[data-rendered="true"]').forEach((el) => {
      el.dataset.rendered = 'false'
      el.innerHTML = ''
      const body = el.closest<HTMLElement>('.doc-figure__body')
      if (body) resetZoomButton(body)
    })
  }

  await nextTick()
  await new Promise<void>((r) => requestAnimationFrame(() => r()))

  const nodes = Array.from(
    document.querySelectorAll<HTMLElement>('.mermaid-diagram[data-rendered="false"]')
  )

  for (let i = 0; i < nodes.length; i++) {
    const el = nodes[i]
    const raw = el.dataset.mermaid
    if (!raw) continue

    const source = decodeURIComponent(raw)
    const id = `mermaid-${Date.now()}-${i}-${Math.random().toString(36).slice(2, 8)}`

    try {
      const { svg } = await mermaid.render(id, source)
      el.innerHTML = svg
      el.dataset.rendered = 'true'

      // 放大按钮挂到 figure body 上,与插图共用同一交互与样式。
      // pick 用惰性查询:主题切换重渲后 SVG 节点会被换掉。
      const body = el.closest<HTMLElement>('.doc-figure__body')
      if (body && el.querySelector('svg')) {
        const cap = body.parentElement?.querySelector('.doc-figure__cap')?.textContent?.trim()
        attachZoomButton(body, () => el.querySelector('svg'), cap || '流程图')
      }
    } catch {
      el.dataset.rendered = 'error'
      el.innerHTML = `<pre class="mermaid-error">${escapeHtml(source)}</pre>`
    }
  }
}

function escapeHtml(s: string) {
  return s.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;').replaceAll("'", '&#39;')
}

export function setupMermaid() {
  // 用订阅器而非直接赋值 router.onAfterRouteChange:后者是单值属性,
  // 会被 ReadingProgress 等组件覆盖,导致 SPA 跳转后 mermaid 不渲染。
  // .catch 治「静默失败」:之前 onMounted 调用没接住 reject,加载失败时图直接消失无痕。
  onMounted(() => {
    renderMermaidDiagrams().catch((e) => console.error('[mermaid] onMounted 渲染失败', e))

    // 明暗模式切换要重画:mermaid 把配色烧进了 SVG 内联样式,靠 CSS 覆盖不掉。
    // VitePress 切换主题时改的是 <html class="dark">,所以观察 class 变化。
    const observer = new MutationObserver(() => {
      if (currentScheme() !== appliedScheme) {
        renderMermaidDiagrams().catch((e) => console.error('[mermaid] 主题切换重渲失败', e))
      }
    })
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] })
    onUnmounted(() => observer.disconnect())
  })

  subscribeAfterRouteChange(() => renderMermaidDiagrams().catch((e) => console.error('[mermaid] 路由切换渲染失败', e)))
}
