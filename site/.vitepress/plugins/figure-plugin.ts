import type { PluginSimple } from 'markdown-it'
import type MarkdownIt from 'markdown-it'
import type Token from 'markdown-it/lib/token.mjs'

/**
 * 图与图注基建。
 *
 * 目标:作者写标准 Markdown 就得到带编号图注的插图,不需要记任何自定义语法。
 *
 *   ![CPU 三态迁移](./state.png)
 *   *就绪态只能由调度器推入运行态*
 *
 * 渲染为:
 *   <figure class="doc-figure doc-figure--image is-numbered">
 *     <div class="doc-figure__body"><img loading="lazy" ...></div>
 *     <figcaption class="doc-figure__cap">就绪态只能由调度器推入运行态</figcaption>
 *   </figure>
 *
 * 图注来源优先级:紧随其后的整段强调文本 > 图片 alt。
 * 两者都没有时仍包 figure(统一布局),但不加 is-numbered——编号只给真正
 * 有说明的图,避免读者看到「图 1、图 3」这种断号。
 *
 * 编号本身交给 CSS counter(见 article-figure.css):流程图与插图共用一个
 * 计数器,因此同一页里两类图的序号是连续的。插件不维护计数状态,SSR 与
 * HMR 都不会算错。
 */

/** 段落内容是否为「仅一张图片」(允许纯空白与换行)。 */
function loneImage(inline: Token): Token | null {
  const kids = inline.children
  if (!kids || kids.length === 0) return null

  let img: Token | null = null
  for (const k of kids) {
    if (k.type === 'image') {
      if (img) return null // 一段里多张图:按图墙处理,不套 figure
      img = k
    } else if (k.type === 'softbreak' || k.type === 'hardbreak') {
      continue
    } else if (k.type === 'text' && k.content.trim() === '') {
      continue
    } else {
      return null // 图旁边还有正文,说明是行内插图
    }
  }
  return img
}

/** 段落是否整体被单层强调包裹(`*图注*` / `_图注_`)。 */
function wrappedInEmphasis(inline: Token): boolean {
  const kids = inline.children
  if (!kids || kids.length < 3) return false
  if (kids[0].type !== 'em_open' || kids[kids.length - 1].type !== 'em_close') return false

  // 必须是同一层开闭:中途归零说明是 *a* 文本 *b* 这种多段强调
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

function makeToken(Ctor: typeof Token, type: string, tag: string, nesting: Token['nesting']): Token {
  return new Ctor(type, tag, nesting)
}

export const figurePlugin: PluginSimple = (md: MarkdownIt) => {
  md.core.ruler.push('doc_figure', (state) => {
    const TokenCtor = state.Token
    const src = state.tokens
    const out: Token[] = []

    for (let i = 0; i < src.length; i++) {
      const isParagraph =
        src[i].type === 'paragraph_open' &&
        src[i + 1]?.type === 'inline' &&
        src[i + 2]?.type === 'paragraph_close'

      const img = isParagraph ? loneImage(src[i + 1]) : null
      if (!img) {
        out.push(src[i])
        continue
      }

      // 图注:优先取紧随的整段强调文本,并剥掉外层 <em>(figcaption 自己定样式)
      let capInline: Token | null = null
      let consumed = 3
      const hasEmParagraph =
        src[i + 3]?.type === 'paragraph_open' &&
        src[i + 4]?.type === 'inline' &&
        src[i + 5]?.type === 'paragraph_close' &&
        wrappedInEmphasis(src[i + 4])

      if (hasEmParagraph) {
        capInline = src[i + 4]
        capInline.children = capInline.children!.slice(1, -1)
        consumed = 6
      } else if (img.content.trim() !== '') {
        // 退回 alt:技术书里 alt 基本就是图注,复用它的 inline 子节点以保留行内格式
        capInline = makeToken(TokenCtor, 'inline', '', 0)
        capInline.content = img.content
        capInline.children = img.children ? [...img.children] : []
        capInline.level = 0
      }

      img.attrSet('loading', 'lazy')
      img.attrSet('decoding', 'async')

      // 继承原段落层级:图可能嵌在引用或列表里,层级错乱会让渲染缩进失真
      const lv = src[i].level

      const figureOpen = makeToken(TokenCtor, 'figure_open', 'figure', 1)
      figureOpen.attrSet(
        'class',
        capInline
          ? 'doc-figure doc-figure--image is-numbered'
          : 'doc-figure doc-figure--image'
      )
      figureOpen.block = true
      figureOpen.level = lv

      const bodyOpen = makeToken(TokenCtor, 'figure_body_open', 'div', 1)
      bodyOpen.attrSet('class', 'doc-figure__body')
      bodyOpen.block = true
      bodyOpen.level = lv + 1
      const bodyClose = makeToken(TokenCtor, 'figure_body_close', 'div', -1)
      bodyClose.block = true
      bodyClose.level = lv + 1

      const figureClose = makeToken(TokenCtor, 'figure_close', 'figure', -1)
      figureClose.block = true
      figureClose.level = lv

      out.push(figureOpen, bodyOpen, src[i + 1], bodyClose)

      if (capInline) {
        const capOpen = makeToken(TokenCtor, 'figcaption_open', 'figcaption', 1)
        capOpen.attrSet('class', 'doc-figure__cap')
        capOpen.block = true
        capOpen.level = lv + 1
        const capClose = makeToken(TokenCtor, 'figcaption_close', 'figcaption', -1)
        capClose.block = true
        capClose.level = lv + 1
        out.push(capOpen, capInline, capClose)
      }

      out.push(figureClose)
      i += consumed - 1
    }

    state.tokens = out
  })
}
