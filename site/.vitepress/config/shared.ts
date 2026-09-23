import type { PageData } from 'vitepress'
import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { join } from 'node:path'
import config from '../../../project.config'
import { resolvePlugins } from '../plugins'
import { articleCodeThemes } from './article-code-theme'
import { getGitTimestampMs } from './git-timestamp'
import { applyTagsPageData } from './tags-manifest'

// 单一 markdown 配置来源:config/index.ts(dev/单体构建)和 scripts/build.ts
// 分卷构建的临时 config 都从这里取,改 markdown 只改这一处。
// (对齐上游 Tutorial_AwesomeModernCPP 的 shared.ts 架构)
export const sharedMarkdown = {
  lineNumbers: true,
  math: (config.plugins.math ?? false) as boolean,
  // ld(GNU linker script)、nasm(NASM 汇编)不在 Shiki 默认 bundle,
  // 映射到近似语言,避免 "language not loaded, falling back to txt" 告警刷屏。
  languageAlias: {
    ld: 'c',
    nasm: 'asm',
  },
  // Shiki 的纯文本别名不会解析自定义 output;注册空语法以保留语义标签。
  languages: [{ name: 'output', scopeName: 'text.output', patterns: [] }],
  theme: articleCodeThemes,
  config(md: any) {
    resolvePlugins(md, config)
  },
}

// 覆盖 vitepress 内置搜索盒:VPLocalSearchBox 只被 VPNavBarSearch 以
// './VPLocalSearchBox.vue' 这一 specifier 引用,alias 指到我们的覆盖版
// (索引构建+查询挪进 Web Worker,修搜索卡顿)。
// index.ts(dev/单体 build)与分卷构建的临时 config 都要用,单一来源防漏改。
export const localSearchBoxAlias = {
  './VPLocalSearchBox.vue': fileURLToPath(
    new URL('../theme/components/VPLocalSearchBox.vue', import.meta.url)
  ),
}

// 所有 config(dev/root/分卷)共享的页面数据后处理:
// 1) tags 页/文章页的标签注入(tags-manifest)
// 2) 分卷构建把 md 复制到临时目录,VitePress 对副本跑 git log 拿不到历史,
//    用 document/ 下真实源文件的提交时间覆盖 lastUpdated(详见 git-timestamp.ts)
export async function applySharedPageData(pageData: PageData): Promise<void> {
  applyTagsPageData(pageData)
  const ms = getGitTimestampMs(pageData.relativePath)
  if (ms) {
    pageData.lastUpdated = ms
  }
}

// 首屏立即应用字号档(与 FontSizeSwitcher.vue 的 STORAGE_KEY 一致),防刷新闪烁
export const FONT_SIZE_SCRIPT = `(function(){try{var s=localStorage.getItem('vp-font-size')||'normal';if(s!=='xxsmall'&&s!=='small'&&s!=='normal'&&s!=='large'&&s!=='xxlarge'){s='normal';}document.documentElement.dataset.fontSize=s;}catch(e){}})()`

// 首屏立即应用侧栏宽度(左导航+右大纲),防刷新闪烁。key 与 ResizableSidebar.vue 一致
export const SIDEBAR_WIDTH_SCRIPT = `(function(){try{var w=parseInt(localStorage.getItem('vp-sidebar-width'));if(!w||w<200||w>480){w=240;}document.documentElement.style.setProperty('--vp-sidebar-width',w+'px');var a=parseInt(localStorage.getItem('vp-aside-width'));if(!a||a<180||a>360){a=256;}document.documentElement.style.setProperty('--vp-aside-width',a+'px');}catch(e){}})()`

// ── 本地搜索:渲染缓存 ─────────────────────────────────────────
// 723 篇 md 全量重渲很慢;把「渲染成 HTML 后的搜索索引片段」按内容哈希落盘,
// 增量构建时直接命中。渲染管线行为变化时(markdown 插件增删、vitepress 升级)
// 手动递增 SEARCH_CACHE_VERSION 使旧缓存整体失效。
const SEARCH_CACHE_DIR = join(fileURLToPath(new URL('./', import.meta.url)), '.search-cache')
const SEARCH_CACHE_BUNDLE = join(SEARCH_CACHE_DIR, 'bundle.json')
const SEARCH_CACHE_VERSION = 'v1'

let searchRenderBundle: Record<string, string> | null = null
let searchRenderDirty = false
let searchRenderFlushTimer: ReturnType<typeof setTimeout> | null = null

function loadSearchRenderBundle(): Record<string, string> {
  if (searchRenderBundle === null) {
    try {
      searchRenderBundle = JSON.parse(readFileSync(SEARCH_CACHE_BUNDLE, 'utf8'))
    } catch {
      searchRenderBundle = {}
    }
  }
  return searchRenderBundle
}

function scheduleSearchRenderFlush() {
  searchRenderDirty = true
  if (searchRenderFlushTimer === null) {
    searchRenderFlushTimer = setTimeout(() => {
      searchRenderFlushTimer = null
      if (!searchRenderDirty) return
      searchRenderDirty = false
      try {
        mkdirSync(SEARCH_CACHE_DIR, { recursive: true })
        writeFileSync(SEARCH_CACHE_BUNDLE, JSON.stringify(loadSearchRenderBundle()))
      } catch {
        // 缓存写失败不影响索引构建,只是下次还得重渲染
      }
    }, 500)
  }
}

export const localSearchOptions = {
  _render(src: string, env: Record<string, any>, md: any) {
    const key = createHash('sha1').update(SEARCH_CACHE_VERSION).update(src).digest('hex')
    const bundle = loadSearchRenderBundle()
    if (key in bundle) {
      return bundle[key]
    }
    const fast = process.env.npm_lifecycle_event === 'dev'
    const saved = md.options.highlight
    if (fast) md.options.highlight = undefined
    let html: string
    try {
      html = md.render(src, env)
    } finally {
      if (fast) md.options.highlight = saved
    }
    // 与 vitepress 默认行为对齐:frontmatter 声明 search: false 的页面不入索引
    bundle[key] = env.frontmatter?.search === false ? '' : html
    scheduleSearchRenderFlush()
    return bundle[key]
  },
}
