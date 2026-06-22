import type MarkdownIt from 'markdown-it'
import type { ProjectConfig } from '../config/schema'
import { cppTemplateEscapePlugin } from './escape-cpp-templates'
import { kbdPlugin } from './kbd-plugin'
import { languageAliasPlugin } from './language-aliases'
import { mermaidPlugin } from './mermaid-plugin'
import { codeFoldPlugin } from './code-fold-plugin'

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
  if (config.plugins.codeFold) {
    md.use(codeFoldPlugin)
  }
}
