import { nextTick, onMounted } from 'vue'
import { subscribeAfterRouteChange } from './router-hooks'
import { openLightbox } from './lightbox'

/**
 * 图的「点击放大」统一入口。
 *
 * 流程图(mermaid-client 渲染完 SVG 后)与插图(本模块扫描 figure)共用同一个
 * 按钮、同一套样式、同一个灯箱,读者不需要学两种交互。
 *
 * 按钮挂在 .doc-figure__body 上(position: relative 由 article-figure.css 给),
 * 而不是挂在 img/svg 上——图元素本身可能被 panzoom 或 mermaid 重绘。
 */

// Feather maximize-2 图标(四角向外箭头),currentColor 随主题。
const MAXIMIZE_ICON =
  '<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" ' +
  'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
  '<polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/>' +
  '<line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>'

/** 已挂过按钮的容器,避免路由往返或 mermaid 重渲时挂出两个按钮。 */
const MARK = 'figureZoom'

/**
 * 给一个 .doc-figure__body 挂放大按钮。
 * @param body  figure 的 body 容器(按钮的定位父级)
 * @param pick  点击时取当前图元素(惰性取:mermaid 重渲后 DOM 已换)
 * @param label 无障碍描述
 */
export function attachZoomButton(
  body: HTMLElement,
  pick: () => SVGElement | HTMLImageElement | null,
  label: string,
): void {
  if (body.dataset[MARK] === 'on') return
  body.dataset[MARK] = 'on'
  body.classList.add('doc-figure__body--zoomable')

  const btn = document.createElement('button')
  btn.type = 'button'
  btn.className = 'figure-zoom-btn'
  btn.setAttribute('aria-label', '放大查看')
  btn.title = '放大查看'
  btn.innerHTML = MAXIMIZE_ICON
  btn.addEventListener('click', (e) => {
    e.preventDefault()
    const node = pick()
    if (node) openLightbox({ node, label, trigger: btn })
  })
  body.appendChild(btn)

  // 点图本体也能放大(按钮只是显式入口);点图注/空白不触发
  const openFromNode = (e: Event) => {
    const node = pick()
    if (!node) return
    if (e.target !== node && !node.contains(e.target as Node)) return
    openLightbox({ node, label, trigger: btn })
  }
  body.addEventListener('click', openFromNode)
}

/** 清掉标记,让 mermaid 主题重渲后可以重新挂按钮。 */
export function resetZoomButton(body: HTMLElement): void {
  delete body.dataset[MARK]
  body.classList.remove('doc-figure__body--zoomable')
  body.querySelector('.figure-zoom-btn')?.remove()
}

/** 扫描正文里的插图,给它们挂上放大按钮。 */
function scanImageFigures(): void {
  if (typeof window === 'undefined') return

  const figures = document.querySelectorAll<HTMLElement>('.doc-figure--image')
  figures.forEach((fig) => {
    const body = fig.querySelector<HTMLElement>('.doc-figure__body')
    const img = fig.querySelector('img')
    if (!body || !img) return

    // 纯装饰的小图标(比如徽章)放大没意义,按渲染尺寸过滤。
    // 图可能还没加载完(naturalWidth=0),此时先挂上,不因加载时序漏掉。
    const tooSmall = img.complete && img.naturalWidth > 0 && img.naturalWidth < 240
    if (tooSmall) return

    const cap = fig.querySelector('.doc-figure__cap')?.textContent?.trim()
    attachZoomButton(body, () => fig.querySelector('img'), cap || img.alt || '插图')
  })
}

export function setupFigureZoom(): void {
  onMounted(() => {
    void nextTick(() => scanImageFigures())
  })
  subscribeAfterRouteChange(() => {
    void nextTick(() => scanImageFigures())
  })
}
