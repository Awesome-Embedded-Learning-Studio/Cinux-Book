import { existsSync, readFileSync, readdirSync } from 'node:fs'
import { basename, join, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import matter from 'gray-matter'
import type { PageData } from 'vitepress'

// 本文件恒定位于 site/.vitepress/config/,上推三级 = 项目根。
// 分卷构建时本文件从真实位置加载(不被复制进 .build-tmp),路径稳定。
const HERE = fileURLToPath(new URL('./', import.meta.url))
const PROJECT_ROOT = join(HERE, '..', '..', '..')
const DOCUMENTS = join(PROJECT_ROOT, 'document')
const TAGS_CONFIG = join(PROJECT_ROOT, 'scripts', 'tags.json')

// ── 标签白名单(scripts/tags.json,单一数据源) ─────────────────
// 与上游同构:difficulty(受众/难度)与 platform(运行平台)由系统识别,
// 其余主题标签按分类上墙。白名单外主题标签不上墙但照常计数展示。

interface TagCategory {
  key: string
  zh: string
  tags: string[]
}

interface TagsConfig {
  difficulty_tags: string[]
  platform_tags: string[]
  topic_categories: TagCategory[]
}

const {
  difficulty_tags: DIFFICULTY_TAGS,
  platform_tags: PLATFORM_TAGS,
  topic_categories: TOPIC_CATEGORIES,
} = loadTagConfig()

function loadTagConfig(): TagsConfig {
  try {
    const raw = JSON.parse(readFileSync(TAGS_CONFIG, 'utf8')) as TagsConfig
    return {
      difficulty_tags: raw.difficulty_tags ?? [],
      platform_tags: raw.platform_tags ?? [],
      topic_categories: raw.topic_categories ?? [],
    }
  } catch (err) {
    console.warn('[tags-manifest] scripts/tags.json 缺失或损坏,标签系统降级为空配置:', err)
    return { difficulty_tags: [], platform_tags: [], topic_categories: [] }
  }
}

export const DIFFICULTY_SET = new Set(DIFFICULTY_TAGS)
export const PLATFORM_SET = new Set(PLATFORM_TAGS)

// ── 卷序与卷名(侧栏/标签页排序展示) ────────────────────────────

const VOLUME_ORDER = [
  'primer',
  ...Array.from({ length: 17 }, (_, i) => `book/${String(i + 1).padStart(2, '0')}`),
  'labs',
  'reference',
  'notes',
  'debug-notes',
  'ci',
]

const VOLUME_LABELS: Record<string, string> = {
  'primer': '前置知识',
  'book/01-boot': '第 01 卷 · 引导扇区',
  'book/02-mini-kernel': '第 02 卷 · 最小内核',
  'book/03-big-kernel': '第 03 卷 · 大内核',
  'book/04-developer': '第 04 卷 · 开发环境',
  'book/05-memory': '第 05 卷 · 内存管理',
  'book/06-process': '第 06 卷 · 进程管理',
  'book/07-userland': '第 07 卷 · 用户态',
  'book/08-filesystem': '第 08 卷 · 文件系统',
  'book/09-gui': '第 09 卷 · 图形界面',
  'book/10-multitasking': '第 10 卷 · 多任务',
  'book/11-foundation': '第 11 卷 · 内核基础',
  'book/12-storage': '第 12 卷 · 存储设备',
  'book/13-memory-advanced': '第 13 卷 · 内存进阶',
  'book/14-process-advanced': '第 14 卷 · 进程进阶',
  'book/15-smp': '第 15 卷 · 多核',
  'book/16-security': '第 16 卷 · 安全',
  'book/17-net': '第 17 卷 · 网络',
  'labs': '动手实验',
  'reference': '参考手册',
  'notes': '开发笔记',
  'debug-notes': '排错笔记',
  'ci': 'CI 之道',
}

// ── 数据形状(TagExplorer.vue 消费) ─────────────────────────

export interface TagArticle {
  /** 标题 */
  t: string
  /** 站内链接(clean URL,不含 base,组件侧 withBase) */
  h: string
  /** 所属卷 key(book/ 两级,其余一级;标签查 VOLUME_LABELS) */
  v: string
  /** beginner | intermediate | advanced(缺省则无难度筛选) */
  d?: string
  /** platform: qemu | wsl | real-machine … */
  p?: string
  /** 预估阅读分钟 */
  m?: number
  /** 主题标签(难度/平台标签已剔除) */
  tg: string[]
}

export interface TagsIndex {
  locale: 'zh'
  /** 标签墙上展示的主题标签总数(仅 count>0) */
  tagCount: number
  articleCount: number
  volLabels: Record<string, string>
  categories: Array<{ key: string; label: string; labelEn: string; tags: Array<{ name: string; count: number }> }>
  articles: TagArticle[]
}

// ── 扫描 ────────────────────────────────────────────────────

/** 不算「文章」的文件:标签页自身、404、README(index.md 单独判定,见 isArticleIndex) */
const SKIP_FILE_NAMES = new Set(['tags.md', '404.md', 'README.md'])
/** 不进扫描的目录:资源与公共目录 */
const SKIP_DIR_NAMES = ['images', 'public', 'stylesheets', 'javascripts', 'hooks', 'ai_prompts']

function walkMd(dir: string, out: string[]) {
  let entries
  try {
    entries = readdirSync(dir, { withFileTypes: true })
  } catch {
    return
  }
  for (const e of entries) {
    if (e.name.startsWith('.')) continue
    const full = join(dir, e.name)
    if (e.isDirectory()) {
      if (SKIP_DIR_NAMES.includes(e.name)) continue
      walkMd(full, out)
    } else if (e.name.endsWith('.md') && !SKIP_FILE_NAMES.has(e.name)) {
      out.push(full)
    }
  }
}

/**
 * index.md 多数是导航页,但也有带文章级 frontmatter 的例外(如 debug-notes 的
 * 单文件笔记线)。判别:带 tags 或 difficulty 的算文章;纯目录导航页不进列表。
 */
function isArticleIndex(fm: Record<string, unknown>): boolean {
  return Array.isArray(fm.tags) && (fm.tags as unknown[]).length > 0
}

/** book/ 下两级目录才是卷,其余顶级目录即卷 */
function volumeKeyOf(rel: string): string {
  const parts = rel.split('/')
  if (parts[0] === 'book' && parts.length > 1) return `${parts[0]}/${parts[1]}`
  return parts[0]
}

function toArticle(absPath: string, docRoot: string): TagArticle | null {
  // frontmatter 永远在文件头,截 16KB 足够,免去整读长文
  const raw = readFileSync(absPath, 'utf8').slice(0, 16384)
  let fm: Record<string, unknown>
  try {
    fm = matter(raw).data as Record<string, unknown>
  } catch {
    return null
  }
  const title = typeof fm.title === 'string' ? fm.title.trim() : ''
  if (!title) return null
  // 纯导航 index.md 不进文章列表
  if (basename(absPath) === 'index.md' && !isArticleIndex(fm)) return null

  const relWithExt = relative(docRoot, absPath).split(sep).join('/')
  // 目录 index 页的 URL 是目录本身(尾斜杠),普通页面去掉 .md
  const isDirIndex = relWithExt === 'index.md' || relWithExt.endsWith('/index.md')
  const rel = isDirIndex ? relWithExt.replace(/(^|\/)index\.md$/, '$1') : relWithExt.replace(/\.md$/, '')
  const href = `/${rel}`

  const tags = Array.isArray(fm.tags) ? fm.tags.filter((t): t is string => typeof t === 'string') : []
  const topicTags = tags.filter(t => !PLATFORM_SET.has(t) && !DIFFICULTY_SET.has(t))

  const difficulty
    = typeof fm.difficulty === 'string' && DIFFICULTY_SET.has(fm.difficulty)
      ? fm.difficulty
      : tags.find(t => DIFFICULTY_SET.has(t))
  const platform
    = typeof fm.platform === 'string' && PLATFORM_SET.has(fm.platform)
      ? fm.platform
      : tags.find(t => PLATFORM_SET.has(t))

  const minutes = typeof fm.reading_time_minutes === 'number' ? fm.reading_time_minutes : undefined

  return {
    t: title,
    h: href,
    v: volumeKeyOf(relWithExt),
    d: difficulty,
    p: platform,
    m: minutes,
    tg: topicTags,
  }
}

const cache = new Map<'zh', TagsIndex>()

export function getTagsIndex(locale: 'zh' = 'zh'): TagsIndex {
  const hit = cache.get(locale)
  if (hit) return hit

  const files: string[] = []
  if (existsSync(DOCUMENTS)) walkMd(DOCUMENTS, files)

  const volOrder = new Map(VOLUME_ORDER.map((k, i) => [k, i]))
  const articles = files
    .map(f => toArticle(f, DOCUMENTS))
    .filter((a): a is TagArticle => a !== null)
    .sort((a, b) => {
      const va = volOrder.get(a.v) ?? VOLUME_ORDER.length
      const vb = volOrder.get(b.v) ?? VOLUME_ORDER.length
      return va !== vb ? va - vb : a.h.localeCompare(b.h, 'zh-CN')
    })

  // 标签计数(仅主题标签;墙只渲染 count>0 的)
  const counts = new Map<string, number>()
  for (const a of articles) for (const t of a.tg) counts.set(t, (counts.get(t) ?? 0) + 1)

  const categories = TOPIC_CATEGORIES.map(cat => ({
    key: cat.key,
    label: cat.zh,
    labelEn: cat.key,
    tags: cat.tags
      .map(name => ({ name, count: counts.get(name) ?? 0 }))
      .filter(t => t.count > 0)
      .sort((a, b) => b.count - a.count || a.name.localeCompare(b.name, 'zh-CN')),
  })).filter(cat => cat.tags.length > 0)

  const tagCount = categories.reduce((n, c) => n + c.tags.length, 0)

  // 体检:白名单里从未用到的主题标签——多半是改名残留,提醒清理
  const used = new Set(counts.keys())
  const dead = TOPIC_CATEGORIES.flatMap(c => c.tags).filter(t => !used.has(t))
  if (dead.length > 0) {
    console.warn(`[tags-manifest] 白名单主题标签从未被使用(${dead.length}): ${dead.join('、')} —— 考虑从 scripts/tags.json 清理`)
  }
  const untagged = articles.filter(a => a.tg.length === 0).length
  if (untagged > 0) {
    console.warn(`[tags-manifest] 有 ${untagged} 篇文章没有主题标签,不出现在任何标签过滤结果里`)
  }

  const index: TagsIndex = {
    locale,
    tagCount,
    articleCount: articles.length,
    volLabels: VOLUME_LABELS,
    categories,
    articles,
  }
  cache.set(locale, index)
  return index
}

/** 同时供 dev 主配置与 build.ts 的 root 配置使用:给 tags 页注入索引数据,
 *  给文章页注入算好的主题标签(文章页底部徽章用)。 */
export function applyTagsPageData(page: PageData): void {
  if (page.relativePath === 'tags.md') {
    page.frontmatter.sidebar = false
    page.frontmatter.aside = false
    page.frontmatter.tagsIndex = getTagsIndex('zh')
    return
  }
  // 文章页:客户端组件拿不到 tags.json(node 侧数据),构建期把「过滤掉
  // 难度/平台标签后的主题标签」注入 frontmatter,单一数据源不破。
  // 没有主题标签的页面不注入,组件据此什么都不渲染。
  const tags = page.frontmatter.tags
  if (Array.isArray(tags)) {
    const topic = tags.filter(t => typeof t === 'string' && !PLATFORM_SET.has(t) && !DIFFICULTY_SET.has(t))
    if (topic.length > 0) {
      page.frontmatter.topicTags = topic
      page.frontmatter.tagsPageBase = '/tags'
    }
  }
}
