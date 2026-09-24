import type { PluginSimple } from 'markdown-it'
import type MarkdownIt from 'markdown-it'
import type Token from 'markdown-it/lib/token.mjs'

/**
 * 表格基建，与图共用同一套「图注 + 编号」语言。
 *
 *   | 寄存器 | 作用 |
 *   |:---|:---|
 *   | CR3 | 页目录基址 |
 *
 *   *分页相关的控制寄存器*
 *
 * 渲染为:
 *   <figure class="doc-table is-numbered">
 *     <div class="doc-table__scroll"><table>…</table></div>
 *     <figcaption class="doc-table__cap">分页相关的控制寄存器</figcaption>
 *   </figure>
 *
 * 两个必要性:
 * 1. 横向滚动。VitePress 默认给 table 用 display:block + overflow-x:auto 换取
 *    滚动能力，但我们为了对齐把它改回了 display:table(block 会让列宽算法失效)。
 *    因此必须由外层容器提供滚动，否则宽表格直接撑破版面。
 * 2. 表注编号。表格与图一样需要在正文里被引用（「见表 2」）。
 *
 * 表与图各自编号（图 1/图 2 与表 1/表 2 两套序列），因为读者检索时
 * 是分开找的；而插图与流程图合用一套，因为它们都是「图」。
 */

function wrappedInEmphasis(inline: Token): boolean {
  const kids = inline.children
  if (!kids || kids.length < 3) return false
  if (kids[0].type !== 'em_open' || kids[kids.length - 1].type !== 'em_close') return false

  let depth = 0
  for (let i = 0; i < kids.length; i++) {
    if (kids[i].type === 'em_open') depth++
    else if (kids[i].type === 'em_close') {
      depth--
      if (depth === 0 && i !== kids.length - 1) return false
    }
  }
  return depth === 0
}

export const tablePlugin: PluginSimple = (md: MarkdownIt) => {
  // VitePress 的 table_open 规则把 tabindex="0" 硬编码进了输出字符串
  // （它原本靠 table 自身当滚动容器）。滚动已交给外层 __scroll，这里改回
  // 默认渲染，避免同一张表出现两个 Tab 停留点。
  md.renderer.rules.table_open = (tokens, idx, options, _env, self) =>
    self.renderToken(tokens, idx, options)

  md.core.ruler.push('doc_table', (state) => {
    const TokenCtor = state.Token
    const src = state.tokens
    const out: Token[] = []

    for (let i = 0; i < src.length; i++) {
      if (src[i].type !== 'table_open') {
        out.push(src[i])
        continue
      }

      // 找配对的 table_close(表格不会嵌套，直接线性扫）
      let end = i
      while (end < src.length && src[end].type !== 'table_close') end++
      if (end >= src.length) {
        out.push(src[i])
        continue
      }

      // 表注:紧随表格的整段强调文本，剥掉外层 <em>
      let capInline: Token | null = null
      let consumedTail = 0
      if (
        src[end + 1]?.type === 'paragraph_open' &&
        src[end + 2]?.type === 'inline' &&
        src[end + 3]?.type === 'paragraph_close' &&
        wrappedInEmphasis(src[end + 2])
      ) {
        capInline = src[end + 2]
        capInline.children = capInline.children!.slice(1, -1)
        consumedTail = 3
      }

      // VitePress 默认给 table 挂 tabindex="0"(它原本靠 table 自身做滚动容器)。
      // 滚动已交给外层 __scroll，这里摘掉,避免同一张表出现两个 Tab 停留点。
      const tabAttr = src[i].attrIndex('tabindex')
      if (tabAttr >= 0) src[i].attrs!.splice(tabAttr, 1)

      const lv = src[i].level

      const figOpen = new TokenCtor('doc_table_open', 'figure', 1)
      figOpen.attrSet('class', capInline ? 'doc-table is-numbered' : 'doc-table')
      figOpen.block = true
      figOpen.level = lv

      const scrollOpen = new TokenCtor('doc_table_scroll_open', 'div', 1)
      scrollOpen.attrSet('class', 'doc-table__scroll')
      // tabindex 让键盘用户也能滚动宽表格(无障碍要求:可滚动区域须可聚焦)
      scrollOpen.attrSet('tabindex', '0')
      scrollOpen.attrSet('role', 'region')
      scrollOpen.block = true
      scrollOpen.level = lv + 1

      const scrollClose = new TokenCtor('doc_table_scroll_close', 'div', -1)
      scrollClose.block = true
      scrollClose.level = lv + 1

      const figClose = new TokenCtor('doc_table_close', 'figure', -1)
      figClose.block = true
      figClose.level = lv

      out.push(figOpen, scrollOpen, ...src.slice(i, end + 1), scrollClose)

      if (capInline) {
        const capOpen = new TokenCtor('doc_table_cap_open', 'figcaption', 1)
        capOpen.attrSet('class', 'doc-table__cap')
        capOpen.block = true
        capOpen.level = lv + 1
        const capClose = new TokenCtor('doc_table_cap_close', 'figcaption', -1)
        capClose.block = true
        capClose.level = lv + 1
        out.push(capOpen, capInline, capClose)
      }

      out.push(figClose)
      i = end + consumedTail
    }

    state.tokens = out
  })
}
