import type { PluginSimple } from 'markdown-it'
import type MarkdownIt from 'markdown-it'

/**
 * 流程图基建。
 *
 * 把 ```mermaid fence 改型成自定义 token,让 Shiki 永远看不到它(core rule 在
 * 分词之后、渲染之前跑,因此不受 VitePress 何时覆写 fence renderer 影响)。
 *
 * fence info 的尾部作为图注,与插图共用同一套 figure/figcaption 结构和编号
 * 计数器(见 figure-plugin.ts 与 article-figure.css):
 *
 *   ```mermaid 中断进入内核的三条路径
 *   flowchart LR
 *   ...
 *   ```
 *
 * 不带图注时仍包 figure 以统一布局,但不加 is-numbered——编号只给有说明的图。
 */

function escapeAttr(s: string): string {
  return s
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;')
}

export const mermaidPlugin: PluginSimple = (md: MarkdownIt) => {
  md.core.ruler.push('mermaid_block', (state) => {
    for (let i = 0; i < state.tokens.length; i++) {
      const token = state.tokens[i]
      if (token.type !== 'fence') continue

      const info = token.info.trim()
      // 认 `mermaid` 与 `mermaid <图注>`,但不误伤 `mermaidjs` 这类语言名
      if (info !== 'mermaid' && !/^mermaid\s/.test(info)) continue

      token.type = 'mermaid_diagram'
      token.tag = ''
      token.nesting = 0
      token.meta = { ...(token.meta ?? {}), caption: info.slice('mermaid'.length).trim() }
    }
    return true
  })

  md.renderer.rules.mermaid_diagram = (tokens, idx) => {
    const token = tokens[idx]
    const encoded = encodeURIComponent(token.content.trim())
    const caption = (token.meta as { caption?: string } | undefined)?.caption ?? ''

    const cls = caption
      ? 'doc-figure doc-figure--diagram is-numbered'
      : 'doc-figure doc-figure--diagram'

    // 图注走 renderInline,让作者能在图注里用 `code`、**强调**、链接
    const capHtml = caption
      ? `<figcaption class="doc-figure__cap">${md.renderInline(caption)}</figcaption>`
      : ''

    return (
      `<figure class="${cls}">` +
      `<div class="doc-figure__body">` +
      `<div class="mermaid-diagram" data-mermaid="${escapeAttr(encoded)}" data-rendered="false"></div>` +
      `</div>${capHtml}</figure>`
    )
  }
}
