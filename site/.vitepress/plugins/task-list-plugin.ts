import type { PluginSimple } from 'markdown-it'
import type MarkdownIt from 'markdown-it'

/**
 * GitHub 风格任务列表。
 *
 *   - [x] 已完成
 *   - [ ] 待做
 *
 * markdown-it 核心不含此语法，VitePress 也没装对应插件，原先这两行会原样
 * 输出字面量 `[x] 已完成`。实验册的「操作清单」很依赖它，所以自己实现一版
 * （逻辑很短，不值得为它引一个新依赖）。
 *
 * 复选框设为 disabled：这是静态文档，勾选状态由作者在 md 里决定，
 * 不做客户端持久化（真正需要记录进度的场景走 checkpoints 系统）。
 */

const TASK_RE = /^\[([ xX])\][ \t]+/

export const taskListPlugin: PluginSimple = (md: MarkdownIt) => {
  md.core.ruler.push('task_list', (state) => {
    const tokens = state.tokens

    for (let i = 0; i < tokens.length; i++) {
      if (tokens[i].type !== 'list_item_open') continue

      // 结构固定为 list_item_open → paragraph_open → inline
      const inline = tokens[i + 2]
      if (tokens[i + 1]?.type !== 'paragraph_open' || inline?.type !== 'inline') continue

      const first = inline.children?.[0]
      if (!first || first.type !== 'text') continue

      const m = TASK_RE.exec(first.content)
      if (!m) continue

      const checked = m[1] !== ' '
      // 吃掉 "[x] " 前缀，余下内容保持原样（含行内代码、链接等）
      first.content = first.content.slice(m[0].length)

      const box = new state.Token('html_inline', '', 0)
      box.content =
        `<input class="task-list-checkbox" type="checkbox" disabled${checked ? ' checked' : ''}>`
      // 文本包一层 span，好让 CSS 用 :checked + * 弱化已完成项
      const labelOpen = new state.Token('html_inline', '', 0)
      labelOpen.content = '<span class="task-list-label">'
      const labelClose = new state.Token('html_inline', '', 0)
      labelClose.content = '</span>'

      inline.children = [box, labelOpen, ...(inline.children ?? []), labelClose]

      // 标记到 li 与 ul 上，供 CSS 去掉原生标记
      tokens[i].attrJoin('class', 'task-list-item')
      // 往前找最近的 bullet_list_open（list_item 必在其内）
      for (let j = i - 1; j >= 0; j--) {
        if (tokens[j].type === 'bullet_list_open') {
          if (!/(^|\s)contains-task-list(\s|$)/.test(tokens[j].attrGet('class') ?? '')) {
            tokens[j].attrJoin('class', 'contains-task-list')
          }
          break
        }
        if (tokens[j].type === 'bullet_list_close') break
      }
    }

    return true
  })
}
