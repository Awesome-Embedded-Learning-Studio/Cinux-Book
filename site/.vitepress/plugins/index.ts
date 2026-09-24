import type MarkdownIt from 'markdown-it'
import type { ProjectConfig } from '../config/schema'
import { cppTemplateEscapePlugin } from './escape-cpp-templates'
import { kbdPlugin } from './kbd-plugin'
import { languageAliasPlugin } from './language-aliases'
import { mermaidPlugin } from './mermaid-plugin'
import { figurePlugin } from './figure-plugin'
import { tablePlugin } from './table-plugin'
import { taskListPlugin } from './task-list-plugin'
import { codeFoldPlugin } from './code-fold-plugin'
import { codeLabelPlugin } from './code-label-plugin'

export function resolvePlugins(md: MarkdownIt, config: ProjectConfig): void {
  md.use(languageAliasPlugin)
  if (config.plugins.cppTemplateEscape) {
    cppTemplateEscapePlugin(md)
  }
  if (config.plugins.kbd) {
    md.use(kbdPlugin)
  }
  // 装配顺序有约束:mermaid 必须在 codeFold 之前(它把 ```mermaid fence 改型为
  // mermaid_diagram,让 codeFold 的 fence 覆写看不到它);codeFold 必须最后(它覆写
  // renderer.rules.fence,要包住前面所有插件已处理好的整段 fence HTML,含
  // cppTemplateEscape 的转义产物 + Shiki 高亮 + 复制按钮/行号)。
  if (config.plugins.mermaid) {
    md.use(mermaidPlugin)
  }
  // figure 只改 paragraph(单图成段),与 mermaid/fence 改型互不相干;放在 mermaid
  // 之后,让两者产出同构的 figure 结构与共享编号。
  md.use(figurePlugin)
  // 表格包滚动容器 + 表注;与 figure 互不重叠(一个吃 table_open,一个吃 paragraph)
  md.use(tablePlugin)
  md.use(taskListPlugin)
  // codeLabel 在 codeFold 之前:给 fence 加文件名标签(纯 fence.meta 增强,不覆写 renderer)
  if (config.plugins.codeLabel !== false) {
    md.use(codeLabelPlugin)
  }
  if (config.plugins.codeFold) {
    md.use(codeFoldPlugin)
  }
}
