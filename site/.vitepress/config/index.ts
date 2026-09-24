import { defineConfig } from 'vitepress'
import { resolve } from 'path'
import { createReadStream, cpSync, existsSync, mkdirSync, statSync } from 'fs'
import { join } from 'path'
import { fileURLToPath } from 'url'
import config from '../../../project.config'
import { getBuildInfo } from './build-info'
import { buildSidebar } from './sidebar'
import { buildLocales } from './locales'
import {
  sharedMarkdown,
  localSearchBoxAlias,
  localSearchOptions,
  applySharedPageData,
  FONT_SIZE_SCRIPT,
  SIDEBAR_WIDTH_SCRIPT,
} from './shared'

const buildInfo = getBuildInfo()
const primaryCode = config.locales.find(l => l.default)?.code ?? config.locales[0]?.code ?? 'zh'

// ── 检查点数据目录的 dev 直供 ─────────────────────────────────
// 生产构建由 scripts/build.ts 把仓库根的 checkpoints/ 拷进 dist;dev 时这里
// 用中间件直供,源文件无需进 srcDir(避免 problem.md 被当页面渲染/进搜索)。
const CHECKPOINTS_ROOT = fileURLToPath(new URL('../../../checkpoints', import.meta.url))
const BASE_PREFIX = config.base.replace(/\/$/, '')

const MIME_BY_EXT: Record<string, string> = {
  '.json': 'application/json; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
  '.cpp': 'text/plain; charset=utf-8',
  '.h': 'text/plain; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
}

function serveCheckpointsInDev() {
  return {
    name: 'cinux-serve-checkpoints',
    configureServer(server: any) {
      server.middlewares.use((req: any, res: any, next: () => void) => {
        const m = decodeURIComponent(req.url ?? '').match(
          new RegExp(`^${BASE_PREFIX}(/checkpoints/.+)$`)
        )
        if (!m) return next()
        const rel = m[1].replace(/^\/checkpoints\//, '').replace(/\?.*$/, '')
        const file = join(CHECKPOINTS_ROOT, rel)
        // 防目录穿越
        if (!file.startsWith(CHECKPOINTS_ROOT) || !existsSync(file) || !statSync(file).isFile()) {
          res.statusCode = 404
          res.end('checkpoint file not found')
          return
        }
        res.setHeader(
          'Content-Type',
          MIME_BY_EXT[file.slice(file.lastIndexOf('.')).toLowerCase()] ?? 'application/octet-stream'
        )
        createReadStream(file).pipe(res)
      })
    },
  }
}

export default defineConfig({
  base: config.base,
  cleanUrls: true,
  lastUpdated: true,

  // dev/单体构建跑在真实 document/ 上,死链必须当错误暴露(分卷构建才放宽)。
  // 两类既有内容债定向豁免,其余一律报错:
  // 1) labs 里指向源码文件的相对链接(源码不在 document/ 树内,产物里天然无目标)
  // 2) notes 里指向尚未成文的笔记的前向引用
  ignoreDeadLinks: [
    // labs 里指向源码文件的相对链接(源码不在 document/ 树内,产物里天然无目标)
    /\.(cpp|hpp|h|c|cc|asm|S|ld|sh|txt)(\?|#|$)/i,
    // notes 里指向尚未成文笔记的前向引用
    /2026-07-06-f13-b-bug2-irq-ist/,
  ],

  srcDir: '../document',
  outDir: '../dist-single',

  title: config.title[primaryCode] ?? config.name,
  description: config.description[primaryCode] ?? '',

  locales: buildLocales(config),

  vue: {
    template: {
      compilerOptions: {
        // mermaid/katex 的自定义元素
        isCustomElement: (tag) => tag.includes('-') || tag.includes('.'),
      },
    },
  },

  head: [
    ['link', { rel: 'icon', href: config.favicon || `${config.base}favicon.ico` }],
    // 浏览器地址栏/壁纸融合(明暗双值,与站点冰川青底一致)
    ['meta', { name: 'theme-color', content: '#F5FAF8' }],
    ['meta', { name: 'theme-color', content: '#0B1512', media: '(prefers-color-scheme: dark)' }],
    ['script', {}, FONT_SIZE_SCRIPT],
    ['script', {}, SIDEBAR_WIDTH_SCRIPT],
  ],

  markdown: sharedMarkdown,

  async transformPageData(pageData) {
    await applySharedPageData(pageData)
  },

  // 单体构建产物同样带上 checkpoints/(分卷管线在 build.ts Step 3.7 已拷贝)
  // outDir 为仓库根 dist-single(相对 site/ 的 ../dist-single),这里从 config/ 起算三级
  buildEnd() {
    if (existsSync(CHECKPOINTS_ROOT)) {
      const out = fileURLToPath(new URL('../../../dist-single/checkpoints', import.meta.url))
      mkdirSync(out, { recursive: true })
      cpSync(CHECKPOINTS_ROOT, out, { recursive: true })
    }
  },

  vite: {
    resolve: {
      alias: localSearchBoxAlias,
    },
    plugins: [serveCheckpointsInDev()],
    build: {
      chunkSizeWarningLimit: 5000,
    },
    ssr: {
      external: ['mermaid'],
    },
  },

  themeConfig: {
    nav: config.nav[primaryCode] || [],

    sidebar: buildSidebar(fileURLToPath(new URL('../../document', import.meta.url)), config),

    // 关闭默认主题底部 prev/next 文字导航(usePrevNext 不读 docFooter 开关,
    // 这样写才关得掉)。卡片版 DocNavCards 经 doc-after 插槽注入。
    docFooter: { prev: false, next: false },

    search: {
      provider: 'local',
      options: localSearchOptions,
    },

    editLink: {
      pattern: `https://github.com/${config.github.owner}/${config.github.repo}/edit/${config.github.branch}/${config.documentsPath}/:path`,
      text: '在 GitHub 上编辑此页',
    },

    footer: {
      message: `${buildInfo.version} · ${buildInfo.sha} · ${buildInfo.date}`,
      copyright: config.copyright,
    },

    socialLinks: [
      { icon: 'github', link: `https://github.com/${config.github.owner}/${config.github.repo}` },
    ],

    lastUpdated: {
      text: '最后更新',
    },
  },
})
