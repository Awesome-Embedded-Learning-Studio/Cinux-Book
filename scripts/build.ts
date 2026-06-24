// Cinux 分卷并发构建器
//
// 把整站按 project.config.sidebar.volumes × locales 拆成 N 个独立的小型
// VitePress 构建,用手写 Promise worker pool 并发跑(BUILD_CONCURRENCY),
// 内容哈希做增量缓存,最后缝合(合并各卷搜索索引 + 重写跨卷 hash map/site data)。
//
// 设计参考 ~/Tutorial_AwesomeModernCPP/scripts/build.ts(已验证可用,同款
// VitePress 1.6.x),适配 Cinux 的 project.config 单一数据源驱动。详见
// .claude/plans 下的迁移方案与改造清单。

import { execFile } from 'child_process'
import {
  cpSync, mkdirSync, rmSync, writeFileSync,
  readdirSync, readFileSync, existsSync,
  symlinkSync, statSync,
} from 'fs'
import { join, resolve, relative, basename } from 'path'
import { createHash } from 'crypto'
import { createRequire } from 'module'
import type { VolumeConfig, LocaleConfig } from '../site/.vitepress/config/schema'
import projectConfig from '../project.config'

const require = createRequire(import.meta.url)

// ── CLI Flags ───────────────────────────────────────────────

const FORCE_REBUILD = process.argv.includes('--force') || process.argv.includes('--clean')
// 优先级:BUILD_CONCURRENCY 环境变量(CI 按 runner 调)> project.config.build.concurrency > 4
const CONCURRENCY = parseInt(
  process.env.BUILD_CONCURRENCY ?? String(projectConfig.build.concurrency ?? 4),
  10,
)

// ── Configuration ───────────────────────────────────────────

const PROJECT_ROOT = resolve(import.meta.dirname, '..')
const SITE_DIR = join(PROJECT_ROOT, projectConfig.siteDir)
const MAIN_VP = join(SITE_DIR, '.vitepress')
const BUILD_TMP = join(MAIN_VP, '.build-tmp')
const CACHE_DIR = join(MAIN_VP, projectConfig.build.cacheDir ?? '.build-cache')
const MANIFEST_PATH = join(CACHE_DIR, 'manifest.json')
const DIST_FINAL = join(MAIN_VP, 'dist')
const DOCUMENTS = join(PROJECT_ROOT, projectConfig.documentsDir)
const MAIN_PUBLIC = join(MAIN_VP, 'public')
const VITEPRESS_BIN = join(resolve(require.resolve('vitepress/package.json'), '..'), 'bin', 'vitepress.js')

// 每个 locale 对应的「文档根」与路由前缀。default locale 文档在 document/<srcDir>,
// 非 default(如 en)在 document/<locale.dir>/<srcDir>,URL 带 locale.prefix。
interface LocaleView {
  locale: LocaleConfig
  localeKey: string                 // default → 'root';其余 → prefix 去斜杠(如 'en')
  docsRoot: string                  // 该 locale 的文档根(扫描侧边栏用)
  urlPrefix: (vol: VolumeConfig) => string   // 该 locale 下某卷的完整 URL 前缀
}

function buildLocaleViews(): LocaleView[] {
  return projectConfig.locales.map((locale) => {
    const localeKey = locale.default ? 'root' : (locale.prefix?.replace(/\//g, '') || locale.code)
    const docsRoot = locale.default ? DOCUMENTS : join(DOCUMENTS, locale.dir ?? localeKey)
    const urlPrefix = (vol: VolumeConfig) =>
      locale.default ? vol.urlPrefix : `${locale.prefix}${vol.urlPrefix}`
    return { locale, localeKey, docsRoot, urlPrefix }
  })
}

// ── Logging ─────────────────────────────────────────────────

function ts(): string {
  return new Date().toISOString().substring(11, 19)
}

function log(msg: string) { console.log(`[${ts()}] ${msg}`) }
function logStep(msg: string) {
  console.log(`\n[${ts()}] ${'═'.repeat(60)}`)
  log(`  ${msg}`)
  console.log(`[${ts()}] ${'═'.repeat(60)}`)
}

function memMB(): string {
  const m = process.memoryUsage()
  return `RSS=${(m.rss / 1024 / 1024).toFixed(0)}MB Heap=${(m.heapUsed / 1024 / 1024).toFixed(0)}/${(m.heapTotal / 1024 / 1024).toFixed(0)}MB`
}

// ── Helpers ─────────────────────────────────────────────────

function ensureClean(dir: string) {
  if (existsSync(dir)) rmSync(dir, { recursive: true })
  mkdirSync(dir, { recursive: true })
}

function symlinkDir(target: string, link: string) {
  if (existsSync(link)) rmSync(link, { recursive: true })
  symlinkSync(target, link, 'dir')
}

function countMdFiles(dir: string): number {
  let count = 0
  try {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (e.name.startsWith('.')) continue
      const full = join(dir, e.name)
      if (e.isDirectory()) count += countMdFiles(full)
      else if (e.name.endsWith('.md')) count++
    }
  } catch { /* ignore */ }
  return count
}

/** 递归计算目录的内容哈希(文件相对路径 + 完整内容),用于跨机器稳定的变更检测。 */
function hashDir(dir: string): string {
  const h = createHash('sha256')
  function walk(d: string) {
    try {
      const entries = readdirSync(d, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))
      for (const e of entries) {
        if (e.name.startsWith('.')) continue
        const full = join(d, e.name)
        if (e.isDirectory()) { walk(full); continue }
        h.update(`file:${relative(d, full)}\n`)
        h.update(readFileSync(full))
        h.update('\n')
      }
    } catch { /* ignore */ }
  }
  walk(dir)
  return h.digest('hex').substring(0, 16)
}

function hashFile(path: string): string {
  const h = createHash('sha256')
  if (!existsSync(path)) return ''
  h.update(readFileSync(path))
  return h.digest('hex').substring(0, 16)
}

/**
 * 计算影响【所有卷】构建的输入哈希。
 *
 * 只纳入真正的构建输入(site 的 config/plugins/theme/public + project.config +
 * package.json + lockfile + build.ts 本身),**刻意排除 dist/cache/.build-cache**
 * 等产物目录——否则它们每次构建都变,会让增量缓存永不命中(这是参考实现的隐患,
 * 这里用精确白名单根治)。任一输入变化 → 所有卷的 cacheKey 变化 → 全卷重建。
 */
function hashBuildInputs(): string {
  const h = createHash('sha256')
  const inputs: [string, string][] = [
    ['config', hashDir(join(MAIN_VP, 'config'))],
    ['plugins', hashDir(join(MAIN_VP, 'plugins'))],
    ['theme', hashDir(join(MAIN_VP, 'theme'))],
    ['public', hashDir(MAIN_PUBLIC)],
    ['projectConfig', hashFile(join(PROJECT_ROOT, 'project.config.ts'))],
    ['package', hashFile(join(PROJECT_ROOT, 'package.json'))],
    ['lockfile', hashFile(join(PROJECT_ROOT, 'pnpm-lock.yaml'))],
    ['build-script', hashFile(join(PROJECT_ROOT, 'scripts', 'build.ts'))],
  ]
  for (const [label, value] of inputs) {
    h.update(`${label}:${value}\n`)
  }
  return h.digest('hex').substring(0, 16)
}

// ── Manifest (incremental build state) ──────────────────────

interface ManifestEntry { hash: string; timestamp: string }
type Manifest = Record<string, ManifestEntry>

function readManifest(): Manifest {
  if (FORCE_REBUILD) {
    log('  --force: discarding build cache')
    if (existsSync(CACHE_DIR)) rmSync(CACHE_DIR, { recursive: true })
    return {}
  }
  if (!existsSync(MANIFEST_PATH)) return {}
  try { return JSON.parse(readFileSync(MANIFEST_PATH, 'utf-8')) } catch { return {} }
}

function writeManifest(manifest: Manifest) {
  mkdirSync(CACHE_DIR, { recursive: true })
  writeFileSync(MANIFEST_PATH, JSON.stringify(manifest, null, 2))
}

// ── Config Generators ───────────────────────────────────────
//
// 为每个卷/locale 与 root 临时生成一个独立的 VitePress config.ts。它 import Cinux
// 真实的 sidebar/plugins/locales/build-info/project.config 模块,内联组装,保证与
// dev/single 构建(index.ts)的插件链、themeConfig 行为一致。不另建 shared.ts。

function relFromTmpVp(tmpVpDir: string, absTarget: string): string {
  return relative(tmpVpDir, absTarget).replace(/\\/g, '/')
}

// 首屏字号脚本,与 index.ts / FontSizeSwitcher.vue 的 STORAGE_KEY('vp-font-size')一致。
const FONT_SIZE_SCRIPT = `(function(){try{var s=localStorage.getItem('vp-font-size')||'normal';if(s!=='xxsmall'&&s!=='small'&&s!=='normal'&&s!=='large'&&s!=='xxlarge'){s='normal';}document.documentElement.dataset.fontSize=s;}catch(e){}})()`

function generateVolumeConfig(
  vol: VolumeConfig,
  view: LocaleView,
  absSiteDir: string,
  absSrcDir: string,
): string {
  const tmpVpDir = join(absSiteDir, '.vitepress')
  const relSrc = relative(absSiteDir, absSrcDir).replace(/\\/g, '/')
  const outDirName = view.locale.default ? vol.name : `${vol.name}-${view.localeKey}`
  const relOut = relative(absSiteDir, join(BUILD_TMP, 'output', outDirName)).replace(/\\/g, '/')

  const relSidebar = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'config', 'sidebar'))
  const relPlugins = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'plugins'))
  const relBuildInfo = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'config', 'build-info'))
  const relProjectConfig = relFromTmpVp(tmpVpDir, join(PROJECT_ROOT, 'project.config'))

  const prefix = view.urlPrefix(vol)
  // 对非 default locale,把 locale 前缀注入 urlPrefix,让 volumeSidebar 生成的 link 自带前缀。
  const volForSidebar = view.locale.default
    ? vol
    : { ...vol, urlPrefix: prefix }

  const code = view.locale.code
  const title = projectConfig.title[code] ?? projectConfig.name
  const desc = projectConfig.description[code] ?? ''
  const localeDirPart = view.locale.default ? '' : `/${view.locale.dir ?? view.localeKey}`

  return `import { defineConfig } from 'vitepress'
import { resolve } from 'path'
import { volumeSidebar } from '${relSidebar}'
import { resolvePlugins } from '${relPlugins}'
import { getBuildInfo } from '${relBuildInfo}'
import projectConfig from '${relProjectConfig}'

const buildInfo = getBuildInfo()
const docsRoot = ${JSON.stringify(view.docsRoot)}
const githubUrl = \`https://github.com/\${projectConfig.github.owner}/\${projectConfig.github.repo}\`
const editPatternBase = \`\${githubUrl}/edit/\${projectConfig.github.branch}/\${projectConfig.documentsPath}\`

export default defineConfig({
  base: projectConfig.base,
  cleanUrls: true,
  lastUpdated: true,
  srcDir: '${relSrc}',
  outDir: '${relOut}',
  ignoreDeadLinks: true,
  title: ${JSON.stringify(title)},
  description: ${JSON.stringify(desc)},
  lang: '${code}',
  locales: {
    root: {
      label: ${JSON.stringify(view.locale.label)},
      lang: '${code}',
      title: ${JSON.stringify(title)},
      description: ${JSON.stringify(desc)},
    },
  },
  vue: {
    template: {
      compilerOptions: {
        isCustomElement: (tag: string) => tag.includes('-') || tag.includes('.'),
      },
    },
  },
  head: [
    ['link', { rel: 'icon', href: projectConfig.favicon || \`\${projectConfig.base}favicon.ico\` }],
    ['script', {}, ${JSON.stringify(FONT_SIZE_SCRIPT)}],
  ],
  markdown: {
    lineNumbers: true,
    math: projectConfig.plugins.math ?? false,
    theme: { light: 'github-light', dark: 'github-dark' },
    config(md) {
      resolvePlugins(md, projectConfig)
    },
  },
  vite: {
    publicDir: resolve(${JSON.stringify(MAIN_PUBLIC)}),
    build: { chunkSizeWarningLimit: 5000 },
    ssr: { external: ['mermaid'] },
  },
  themeConfig: {
    nav: projectConfig.nav[${JSON.stringify(code)}] || [],
    sidebar: { '${prefix}/': volumeSidebar(docsRoot, ${JSON.stringify(volForSidebar)}) },
    search: { provider: 'local' },
    editLink: {
      pattern: \`\${editPatternBase}${localeDirPart}/:path\`,
      text: 'Edit this page on GitHub',
    },
    footer: {
      message: \`\${buildInfo.version} · \${buildInfo.sha} · \${buildInfo.date}\`,
      copyright: projectConfig.copyright,
    },
    socialLinks: [{ icon: 'github', link: githubUrl }],
  },
})
`
}

function generateRootConfig(absSiteDir: string, absSrcDir: string): string {
  const tmpVpDir = join(absSiteDir, '.vitepress')
  const relSrc = relative(absSiteDir, absSrcDir).replace(/\\/g, '/')
  const relOut = relative(absSiteDir, join(BUILD_TMP, 'output', 'root')).replace(/\\/g, '/')

  const relSidebar = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'config', 'sidebar'))
  const relPlugins = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'plugins'))
  const relBuildInfo = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'config', 'build-info'))
  const relLocales = relFromTmpVp(tmpVpDir, join(MAIN_VP, 'config', 'locales'))
  const relProjectConfig = relFromTmpVp(tmpVpDir, join(PROJECT_ROOT, 'project.config'))

  const primaryLocale = projectConfig.locales.find((l) => l.default)!

  return `import { defineConfig } from 'vitepress'
import { resolve } from 'path'
import { buildSidebar } from '${relSidebar}'
import { resolvePlugins } from '${relPlugins}'
import { getBuildInfo } from '${relBuildInfo}'
import { buildLocales } from '${relLocales}'
import projectConfig from '${relProjectConfig}'

const buildInfo = getBuildInfo()
const docsRoot = ${JSON.stringify(DOCUMENTS)}
const githubUrl = \`https://github.com/\${projectConfig.github.owner}/\${projectConfig.github.repo}\`
const editPatternBase = \`\${githubUrl}/edit/\${projectConfig.github.branch}/\${projectConfig.documentsPath}\`

export default defineConfig({
  base: projectConfig.base,
  cleanUrls: true,
  lastUpdated: true,
  srcDir: '${relSrc}',
  outDir: '${relOut}',
  ignoreDeadLinks: true,
  title: ${JSON.stringify(projectConfig.title[primaryLocale.code] ?? projectConfig.name)},
  description: ${JSON.stringify(projectConfig.description[primaryLocale.code] ?? '')},
  lang: '${primaryLocale.code}',
  locales: buildLocales(projectConfig),
  vue: {
    template: {
      compilerOptions: {
        isCustomElement: (tag: string) => tag.includes('-') || tag.includes('.'),
      },
    },
  },
  head: [
    ['link', { rel: 'icon', href: projectConfig.favicon || \`\${projectConfig.base}favicon.ico\` }],
    ['script', {}, ${JSON.stringify(FONT_SIZE_SCRIPT)}],
  ],
  markdown: {
    lineNumbers: true,
    math: projectConfig.plugins.math ?? false,
    theme: { light: 'github-light', dark: 'github-dark' },
    config(md) {
      resolvePlugins(md, projectConfig)
    },
  },
  vite: {
    publicDir: resolve(${JSON.stringify(MAIN_PUBLIC)}),
    build: { chunkSizeWarningLimit: 5000 },
    ssr: { external: ['mermaid'] },
  },
  themeConfig: {
    nav: projectConfig.nav[${JSON.stringify(primaryLocale.code)}] || [],
    sidebar: buildSidebar(docsRoot, projectConfig),
    search: { provider: 'local' },
    editLink: {
      pattern: \`\${editPatternBase}/:path\`,
      text: 'Edit this page on GitHub',
    },
    footer: {
      message: \`\${buildInfo.version} · \${buildInfo.sha} · \${buildInfo.date}\`,
      copyright: projectConfig.copyright,
    },
    socialLinks: [{ icon: 'github', link: githubUrl }],
  },
})
`
}

// ── Build Tasks ─────────────────────────────────────────────

interface BuildTask {
  id: string                 // vol.name(default) 或 `${vol.name}-${localeKey}`
  vol: VolumeConfig
  view: LocaleView
  volDocDir: string          // 该卷该 locale 的真实文档目录
  cacheKey: string
  cached: boolean
}

interface SearchIndexSource {
  dir: string
  localeKey: string          // 'root' | 'en' | ...
}

function prepareVolume(
  vol: VolumeConfig,
  view: LocaleView,
  volDocDir: string,
  manifest: Manifest,
  buildInputsHash: string,
): BuildTask {
  const id = view.locale.default ? vol.name : `${vol.name}-${view.localeKey}`
  const docHash = existsSync(volDocDir) ? hashDir(volDocDir) : ''
  const cacheKey = `${buildInputsHash}-${docHash}`
  const prev = manifest[id]
  const cached = !FORCE_REBUILD && prev && prev.hash === cacheKey && existsSync(join(CACHE_DIR, 'output', id))
  return { id, vol, view, volDocDir, cacheKey, cached }
}

function execFileAsync(file: string, args: string[], opts?: { cwd?: string }): Promise<void> {
  return new Promise((resolveP, reject) => {
    execFile(file, args, { cwd: opts?.cwd ?? PROJECT_ROOT }, (err, stdout, stderr) => {
      if (stdout) process.stdout.write(stdout)
      if (stderr) process.stderr.write(stderr)
      if (err) reject(err)
      else resolveP()
    })
  })
}

async function buildVolume(task: BuildTask): Promise<string> {
  const { id, vol, view, volDocDir } = task
  const srcDirName = id
  const volSrcDir = join(BUILD_TMP, `src-${srcDirName}`)
  const tmpSite = join(BUILD_TMP, `site-${srcDirName}`)
  const volOutput = join(BUILD_TMP, 'output', srcDirName)
  const cachedOutput = join(CACHE_DIR, 'output', srcDirName)

  // 命中缓存:直接拷贝,跳过构建
  if (task.cached) {
    log(`  ${id}: ✓ cached (unchanged)`)
    mkdirSync(volOutput, { recursive: true })
    cpSync(cachedOutput, volOutput, { recursive: true })
    return volOutput
  }

  const mdCount = countMdFiles(volDocDir)
  log(`  ${id}: building ${mdCount} files...`)

  // 准备源:default → src-<id>/<srcDir>;非 default → src-<id>/<locale.dir>/<srcDir>
  if (view.locale.default) {
    mkdirSync(join(volSrcDir, vol.srcDir), { recursive: true })
    cpSync(volDocDir, join(volSrcDir, vol.srcDir), { recursive: true })
  } else {
    const sub = join(volSrcDir, view.locale.dir ?? view.localeKey, vol.srcDir)
    mkdirSync(sub, { recursive: true })
    cpSync(volDocDir, sub, { recursive: true })
  }

  // 准备临时 site:生成单卷 config + symlink 共享的 theme/plugins/public
  mkdirSync(join(tmpSite, '.vitepress'), { recursive: true })
  writeFileSync(join(tmpSite, '.vitepress', 'config.ts'), generateVolumeConfig(vol, view, tmpSite, volSrcDir))
  symlinkDir(join(MAIN_VP, 'theme'), join(tmpSite, '.vitepress', 'theme'))
  symlinkDir(join(MAIN_VP, 'plugins'), join(tmpSite, '.vitepress', 'plugins'))
  symlinkDir(MAIN_PUBLIC, join(tmpSite, '.vitepress', 'public'))

  const t0 = Date.now()
  await execFileAsync(process.execPath, [VITEPRESS_BIN, 'build', relative(PROJECT_ROOT, tmpSite)])
  const elapsed = ((Date.now() - t0) / 1000).toFixed(1)

  if (!existsSync(volOutput)) throw new Error(`${id}: output dir not found after build`)
  log(`  ${id}: ✓ built in ${elapsed}s (${mdCount} files, ${memMB()})`)

  // 存入缓存
  mkdirSync(join(CACHE_DIR, 'output'), { recursive: true })
  if (existsSync(cachedOutput)) rmSync(cachedOutput, { recursive: true })
  cpSync(volOutput, cachedOutput, { recursive: true })

  return volOutput
}

/** 用有限并发跑任务:固定数量 worker 从共享队列抢活(经典 worker pool)。 */
async function runParallel<T>(tasks: T[], fn: (t: T) => Promise<void>, limit: number): Promise<void> {
  let idx = 0
  const workers: Promise<void>[] = []
  for (let i = 0; i < Math.min(limit, tasks.length); i++) {
    workers.push((async () => {
      while (idx < tasks.length) {
        const task = tasks[idx++]
        if (task) await fn(task)
      }
    })())
  }
  await Promise.all(workers)
}

// ── Cross-Volume Data Unification ────────────────────────────
//
// 每卷是孤立构建的,产物里各有自己的 __VP_HASH_MAP__(路由→资源哈希)和
// __VP_SITE_DATA__(含 sidebar/nav)。这里把所有卷的 hash map 合并成一份,并把
// root 的完整 site data 分发给每个 HTML,让 N 个独立构建在运行时表现为一个站点。
// 正则依赖 VitePress 1.6.x 的 SSR 注水格式——升 minor 前需重新验证。

function unifyCrossVolumeData(distDir: string) {
  logStep('Step 3.5/4: Unifying cross-volume hash maps & site data')

  const htmlFiles: string[] = []
  function walk(d: string) {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const full = join(d, e.name)
      if (e.isDirectory()) walk(full)
      else if (e.name.endsWith('.html')) htmlFiles.push(full)
    }
  }
  walk(distDir)
  log(`  Found ${htmlFiles.length} HTML files`)

  const mergedHashMap: Record<string, string> = {}
  let rootSiteDataExpr = ''

  for (const f of htmlFiles) {
    const c = readFileSync(f, 'utf-8')

    const hmMatch = c.match(/__VP_HASH_MAP__\s*=\s*JSON\.parse\("(.+?)"\)/)
    if (hmMatch) {
      try {
        const mapObj: Record<string, string> = JSON.parse(new Function(`return "${hmMatch[1]}"`)())
        Object.assign(mergedHashMap, mapObj)
      } catch { /* skip */ }
    }

    if (f === join(distDir, 'index.html')) {
      const sdMatch = c.match(/__VP_SITE_DATA__\s*=\s*JSON\.parse\("(.+?)"\)/)
      if (sdMatch) rootSiteDataExpr = sdMatch[1]
    }
  }

  const totalEntries = Object.keys(mergedHashMap).length
  log(`  Merged hash map: ${totalEntries} entries`)
  log(`  Root site data: ${rootSiteDataExpr ? 'found' : 'MISSING'}`)

  const hmJsLiteral = JSON.stringify(JSON.stringify(mergedHashMap))

  let patched = 0
  for (const f of htmlFiles) {
    let c = readFileSync(f, 'utf-8')
    let changed = false

    const hmReplace = c.replace(
      /__VP_HASH_MAP__\s*=\s*JSON\.parse\(".+?"\)/,
      `__VP_HASH_MAP__=JSON.parse(${hmJsLiteral})`,
    )
    if (hmReplace !== c) { c = hmReplace; changed = true }

    if (rootSiteDataExpr && f !== join(distDir, 'index.html')) {
      const sdReplace = c.replace(
        /__VP_SITE_DATA__\s*=\s*JSON\.parse\(".+?"\)/,
        `__VP_SITE_DATA__=JSON.parse("${rootSiteDataExpr}")`,
      )
      if (sdReplace !== c) { c = sdReplace; changed = true }
    }

    if (changed) {
      writeFileSync(f, c)
      patched++
    }
  }
  log(`  Patched ${patched} files with unified data`)
}

// ── Search Index Merge ──────────────────────────────────────

const LOCALE_KEYS = buildLocaleViews().map((v) => v.localeKey)

function findSearchIndexFiles(dir: string): Map<string, string> {
  const result = new Map<string, string>()
  const chunksDir = join(dir, 'assets', 'chunks')
  if (!existsSync(chunksDir)) return result
  const pattern = new RegExp(`^@localSearchIndex(${LOCALE_KEYS.join('|')})\\.[^.]+\\.js$`)
  for (const f of readdirSync(chunksDir)) {
    const m = f.match(pattern)
    if (m) result.set(m[1], join(chunksDir, f))
  }
  return result
}

type SerializedSearchIndex = {
  documentCount: number
  nextId: number
  documentIds: Record<string, string>
  fieldIds: Record<string, number>
  fieldLength: Record<string, number[]>
  averageFieldLength: number[]
  storedFields: Record<string, Record<string, unknown>>
  dirtCount: number
  index: Array<[string, Record<string, Record<string, number>>]>
  serializationVersion: number
}

function findSearchIndexExportStart(content: string): number {
  let match: RegExpExecArray | null
  let exportStart = -1
  const exportPattern = /;?\s*export\s*\{/g
  while ((match = exportPattern.exec(content)) !== null) {
    exportStart = match.index
  }
  return exportStart
}

function extractSearchIndex(indexPath: string): SerializedSearchIndex | null {
  const content = readFileSync(indexPath, 'utf-8')
  const assignment = content.match(/^const\s+\w+\s*=\s*/)
  const exportStart = findSearchIndexExportStart(content)
  if (!assignment || exportStart === -1) {
    log(`  ⚠ Could not parse: ${relative(PROJECT_ROOT, indexPath)}`)
    return null
  }
  let expr = content.slice(assignment[0].length, exportStart).trim()
  if (expr.endsWith(';')) expr = expr.slice(0, -1).trim()
  const jsonStr: string = new Function(`return (${expr})`)()
  return JSON.parse(jsonStr)
}

function mergeSerializedSearchIndexes(indexes: SerializedSearchIndex[]): SerializedSearchIndex {
  if (indexes.length === 0) throw new Error('No search indexes to merge')

  const fieldIds = indexes[0].fieldIds
  const fieldCount = Object.keys(fieldIds).length
  const merged: SerializedSearchIndex = {
    documentCount: 0,
    nextId: 0,
    documentIds: {},
    fieldIds,
    fieldLength: {},
    averageFieldLength: Array(fieldCount).fill(0),
    storedFields: {},
    dirtCount: 0,
    index: [],
    serializationVersion: indexes[0].serializationVersion,
  }

  const termIndex = new Map<string, Record<string, Record<string, number>>>()
  const fieldLengthSums = Array(fieldCount).fill(0)

  for (const data of indexes) {
    const localToGlobal = new Map<string, string>()
    const fieldMap = new Map<string, string>()

    for (const [fieldName, localFieldId] of Object.entries(data.fieldIds)) {
      const targetFieldId = fieldIds[fieldName]
      if (targetFieldId === undefined) {
        throw new Error(`Incompatible search field: ${fieldName}`)
      }
      fieldMap.set(String(localFieldId), String(targetFieldId))
    }

    for (const [localId, url] of Object.entries(data.documentIds)) {
      const globalId = String(merged.nextId++)
      localToGlobal.set(localId, globalId)
      merged.documentIds[globalId] = url
      merged.storedFields[globalId] = data.storedFields[localId] || {}
      const lengths = data.fieldLength[localId] || []
      merged.fieldLength[globalId] = Array(fieldCount).fill(0)
      for (const [localFieldId, targetFieldId] of fieldMap) {
        const len = lengths[Number(localFieldId)] || 0
        const targetIndex = Number(targetFieldId)
        merged.fieldLength[globalId][targetIndex] = len
        fieldLengthSums[targetIndex] += len
      }
    }

    merged.dirtCount += data.dirtCount || 0

    for (const [term, postings] of data.index) {
      const mergedPostings = termIndex.get(term) || {}
      for (const [localFieldId, docs] of Object.entries(postings)) {
        const targetFieldId = fieldMap.get(localFieldId)
        if (targetFieldId === undefined) continue
        const fieldPostings = mergedPostings[targetFieldId] || {}
        for (const [localId, frequency] of Object.entries(docs)) {
          const globalId = localToGlobal.get(localId)
          if (globalId === undefined) continue
          fieldPostings[globalId] = (fieldPostings[globalId] || 0) + frequency
        }
        mergedPostings[targetFieldId] = fieldPostings
      }
      termIndex.set(term, mergedPostings)
    }
  }

  merged.documentCount = Object.keys(merged.documentIds).length
  merged.averageFieldLength = fieldLengthSums.map((sum) => merged.documentCount > 0 ? sum / merged.documentCount : 0)
  merged.index = [...termIndex.entries()]
  return merged
}

function buildSearchIndexJs(index: SerializedSearchIndex): string {
  const json = JSON.stringify(index)
  return `const e=${JSON.stringify(json)};export{e as default};`
}

async function mergeSearchIndexes(sources: SearchIndexSource[], finalDist: string) {
  logStep('Step 3/4: Merging search indexes')

  const indexesByLocale: Record<string, SerializedSearchIndex[]> = {}
  const targetsByLocale: Record<string, Set<string>> = {}
  for (const key of LOCALE_KEYS) {
    indexesByLocale[key] = []
    targetsByLocale[key] = new Set()
  }

  for (const source of sources) {
    for (const [localeKey, indexPath] of findSearchIndexFiles(source.dir)) {
      if (!indexesByLocale[localeKey]) continue // 未知 locale,跳过
      const index = extractSearchIndex(indexPath)
      if (!index) continue
      log(`  ${localeKey}: ${index.documentCount} docs from ${relative(PROJECT_ROOT, source.dir)}`)
      indexesByLocale[localeKey].push(index)

      const target = join(finalDist, 'assets', 'chunks', basename(indexPath))
      if (existsSync(target)) {
        targetsByLocale[localeKey].add(target)
      } else {
        log(`  ⚠ ${localeKey}: target missing for ${basename(indexPath)}`)
      }
    }
  }

  for (const localeKey of LOCALE_KEYS) {
    const indexes = indexesByLocale[localeKey]
    if (indexes.length === 0) { log(`  ${localeKey}: no indexes, skipping`); continue }
    const mergedIndex = mergeSerializedSearchIndexes(indexes)
    log(`  ${localeKey}: merging ${mergedIndex.documentCount} total docs...`)
    const js = buildSearchIndexJs(mergedIndex)
    const allTargets = [...targetsByLocale[localeKey]]
    if (allTargets.length === 0) {
      log(`  ⚠ ${localeKey}: no target index files in final dist!`)
      continue
    }
    writeFileSync(allTargets[0], js)
    const canonicalName = basename(allTargets[0])
    const stub = `export{default}from"./${canonicalName}";`
    for (let i = 1; i < allTargets.length; i++) {
      writeFileSync(allTargets[i], stub)
    }
    const savedMB = ((js.length - stub.length) * (allTargets.length - 1) / 1024 / 1024).toFixed(1)
    log(`  ${localeKey}: ✓ 1 canonical + ${allTargets.length - 1} stubs (saved ${savedMB} MB)`)
  }
}

// ── Main ────────────────────────────────────────────────────

async function main() {
  const localeViews = buildLocaleViews()
  logStep('Cinux 分卷并发构建 — VitePress per-volume build')
  log(`  Project:     ${PROJECT_ROOT}`)
  log(`  Volumes:     ${projectConfig.sidebar.volumes.length} (× ${localeViews.length} locale)`)
  log(`  Concurrency: ${CONCURRENCY}`)
  log(`  Force:       ${FORCE_REBUILD}`)
  log(`  Memory:      ${memMB()}`)
  const start = Date.now()

  // ── Prepare ─────────────────────────────────────────────
  ensureClean(BUILD_TMP)
  ensureClean(DIST_FINAL)
  mkdirSync(join(BUILD_TMP, 'output'), { recursive: true })

  const manifest = readManifest()
  const buildInputsHash = hashBuildInputs()

  // ── Step 1: Build root ──────────────────────────────────
  logStep('Step 1/4: Building root site')

  const rootSrcDir = join(BUILD_TMP, 'root-src')
  mkdirSync(rootSrcDir, { recursive: true })
  for (const page of projectConfig.build.rootPages ?? ['index.md']) {
    const s = join(DOCUMENTS, page)
    if (existsSync(s)) cpSync(s, join(rootSrcDir, page))
  }
  for (const asset of projectConfig.build.rootAssets ?? []) {
    const s = join(DOCUMENTS, asset)
    if (existsSync(s)) cpSync(s, join(rootSrcDir, asset), statSync(s).isDirectory() ? { recursive: true } : undefined)
  }
  // 非 default locale 的根页面(如 document/en/index.md)
  for (const view of localeViews) {
    if (view.locale.default) continue
    const localeRoot = view.docsRoot
    if (!existsSync(localeRoot)) continue
    const localeSrcDir = join(rootSrcDir, view.locale.dir ?? view.localeKey)
    mkdirSync(localeSrcDir, { recursive: true })
    for (const page of projectConfig.build.rootPages ?? ['index.md']) {
      const s = join(localeRoot, page)
      if (existsSync(s)) cpSync(s, join(localeSrcDir, page))
    }
  }

  const rootTmpSite = join(BUILD_TMP, 'site-root')
  mkdirSync(join(rootTmpSite, '.vitepress'), { recursive: true })
  writeFileSync(join(rootTmpSite, '.vitepress', 'config.ts'), generateRootConfig(rootTmpSite, rootSrcDir))
  symlinkDir(join(MAIN_VP, 'theme'), join(rootTmpSite, '.vitepress', 'theme'))
  symlinkDir(join(MAIN_VP, 'plugins'), join(rootTmpSite, '.vitepress', 'plugins'))
  symlinkDir(MAIN_PUBLIC, join(rootTmpSite, '.vitepress', 'public'))

  const rootT0 = Date.now()
  await execFileAsync(process.execPath, [VITEPRESS_BIN, 'build', '.'], { cwd: rootTmpSite })
  const rootOutput = join(BUILD_TMP, 'output', 'root')
  if (existsSync(rootOutput)) cpSync(rootOutput, DIST_FINAL, { recursive: true })
  log(`  Root: ${((Date.now() - rootT0) / 1000).toFixed(1)}s`)

  // ── Step 2: Build volumes in parallel ────────────────────
  logStep('Step 2/4: Building volumes (parallel)')

  const tasks: BuildTask[] = []
  for (const vol of projectConfig.sidebar.volumes) {
    for (const view of localeViews) {
      const volDocDir = join(view.docsRoot, vol.srcDir)
      if (!existsSync(volDocDir)) continue // 该 locale 下此卷不存在(如 en 暂空)→ 自动跳过
      tasks.push(prepareVolume(vol, view, volDocDir, manifest, buildInputsHash))
    }
  }

  const cachedCount = tasks.filter((t) => t.cached).length
  const buildCount = tasks.length - cachedCount
  log(`  Tasks: ${tasks.length} total, ${cachedCount} cached, ${buildCount} to build`)
  log(`  Concurrency: ${CONCURRENCY}\n`)

  const searchSources: SearchIndexSource[] = [{ dir: rootOutput, localeKey: 'root' }]
  const newManifest: Manifest = {}

  await runParallel(tasks, async (task) => {
    const volOutput = await buildVolume(task)
    searchSources.push({ dir: volOutput, localeKey: task.view.localeKey })
    cpSync(volOutput, DIST_FINAL, { recursive: true })
    newManifest[task.id] = { hash: task.cacheKey, timestamp: new Date().toISOString() }
  }, CONCURRENCY)

  // ── Step 3: Merge search indexes ────────────────────────
  await mergeSearchIndexes(searchSources, DIST_FINAL)

  // ── Step 3.5: Unify hash maps and site data ─────────────
  unifyCrossVolumeData(DIST_FINAL)

  // ── Step 4: Finalize ────────────────────────────────────
  logStep('Step 4/4: Finalizing')
  rmSync(BUILD_TMP, { recursive: true })
  writeManifest(newManifest)

  let outputFiles = 0
  function countFiles(d: string) { for (const e of readdirSync(d, { withFileTypes: true })) { if (e.isDirectory()) countFiles(join(d, e.name)); else outputFiles++ } }
  countFiles(DIST_FINAL)

  const elapsed = ((Date.now() - start) / 1000).toFixed(1)
  log(`\n  ═══ Build Summary ═══`)
  log(`  Status:   ✓ SUCCESS`)
  log(`  Time:     ${elapsed}s (${cachedCount} cached, ${buildCount} built)`)
  log(`  Output:   ${relative(PROJECT_ROOT, DIST_FINAL)} (${outputFiles} files)`)
  log(`  Memory:   ${memMB()}`)
  log(`  Tip:      Use --force for full rebuild, BUILD_CONCURRENCY=N to adjust parallelism`)
}

main().catch((err) => {
  log('\n  BUILD FAILED')
  console.error(err)
  process.exit(1)
})
