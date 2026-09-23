import type { DefaultTheme } from 'vitepress'
import { readdirSync, statSync, readFileSync, existsSync } from 'fs'
import { join } from 'path'
import type { ProjectConfig, VolumeConfig } from './schema'

type SidebarItem = DefaultTheme.SidebarItem

function extractTitle(filePath: string): string | null {
  try {
    const content = readFileSync(filePath, 'utf-8')
    const fmMatch = content.match(/^---[\s\S]*?^title:\s*['"]?(.+?)['"]?\s*$/m)
    if (fmMatch) return escapeHtml(fmMatch[1])
    const h1 = content.match(/^#\s+(.+)$/m)
    if (h1) return escapeHtml(h1[1].replace(/\{.*?\}/g, '').trim())
  } catch { /* ignore */ }
  return null
}

// 标题最终经 v-html 渲染(VitePress sidebar 默认行为),必须先转义——
// 否则含 <T>、<Args> 的 C++ 模板标题会被浏览器当 HTML 标签解析吃掉后续文本
// (对齐上游 Tutorial_AwesomeModernCPP 的安全修复)
function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

function humanize(name: string): string {
  return name
    .replace(/^\d+[-]?/, '')
    .replace(/[-_]/g, ' ')
    .replace(/\b\w/g, c => c.toUpperCase())
}

// frontmatter 里的 sidebar_order:比数字前缀更细的排序控制(可选;按路径缓存,排序期间不重复读盘)
const orderCache = new Map<string, number | null>()
function extractSidebarOrder(filePath: string): number | null {
  if (orderCache.has(filePath)) return orderCache.get(filePath) ?? null
  let order: number | null = null
  try {
    const m = readFileSync(filePath, 'utf8').match(/^sidebar_order:\s*(\d+)\s*$/m)
    order = m ? parseInt(m[1]) : null
  } catch { /* ignore */ }
  orderCache.set(filePath, order)
  return order
}

// Directories to skip when scanning
const SKIP_DIRS = new Set([
  'reference',
  'ai_prompts',
  '_build',
  'site',
  '.git',
])

function entryOrder(name: string, fullPath: string): number {
  return extractSidebarOrder(fullPath) ?? parseInt(name.match(/^(\d+)/)?.[1] ?? '0', 10)
}

function sortEntries(a: string, b: string, dir: string): number {
  const oa = entryOrder(a, join(dir, a))
  const ob = entryOrder(b, join(dir, b))
  if (oa !== ob) return oa - ob
  // 中文条目按拼音locale排序,数字前缀缺失时也稳定
  return a.localeCompare(b, 'zh-CN')
}

function scanDir(dir: string, urlPrefix: string, depth = 0): SidebarItem[] {
  if (depth > 5) return []

  let entries: string[]
  try {
    entries = readdirSync(dir).filter(e =>
      !e.startsWith('.') &&
      !SKIP_DIRS.has(e) &&
      e !== 'hooks' &&
      e !== 'stylesheets' &&
      e !== 'javascripts' &&
      e !== 'images' &&
      e !== 'logo'
    )
  } catch { return [] }

  entries.sort((a, b) => sortEntries(a, b, dir))
  const items: SidebarItem[] = []

  for (const name of entries) {
    const fullPath = join(dir, name)
    if (!statSync(fullPath).isDirectory() && !name.endsWith('.md')) continue

    if (statSync(fullPath).isDirectory()) {
      const subItems = scanDir(fullPath, `${urlPrefix}/${name}`, depth + 1)
      const indexPath = join(fullPath, 'index.md')
      const title = extractTitle(indexPath) || humanize(name)

      if (subItems.length > 0) {
        items.push({
          text: title,
          link: existsSync(indexPath) ? `${urlPrefix}/${name}/` : undefined,
          items: subItems,
          collapsed: depth > 0,
        })
      } else if (existsSync(indexPath)) {
        items.push({ text: title, link: `${urlPrefix}/${name}/` })
      }
    } else if (name !== 'index.md' && name !== 'tags.md') {
      const title = extractTitle(fullPath) || humanize(name.replace(/\.md$/, ''))
      items.push({ text: title, link: `${urlPrefix}/${name.replace(/\.md$/, '')}` })
    }
  }

  return items
}

export function volumeSidebar(
  docsRoot: string,
  vol: VolumeConfig
): DefaultTheme.SidebarItem[] {
  const dir = join(docsRoot, vol.srcDir)
  const indexPath = join(dir, 'index.md')
  const items = scanDir(dir, vol.urlPrefix)

  const overviewTitle = extractTitle(indexPath) || humanize(vol.srcDir)
  return [
    { text: overviewTitle, link: `${vol.urlPrefix}/` },
    ...items,
  ]
}

export function buildSidebar(
  docsRoot: string,
  config: ProjectConfig
): DefaultTheme.Sidebar {
  const sidebar: DefaultTheme.Sidebar = {}

  for (const vol of config.sidebar.volumes) {
    sidebar[`${vol.urlPrefix}/`] = volumeSidebar(docsRoot, vol)
  }

  if (config.sidebar.extra) {
    Object.assign(sidebar, config.sidebar.extra)
  }

  return sidebar
}
