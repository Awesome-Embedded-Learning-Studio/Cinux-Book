import { defineConfig } from 'vitepress'
import type { DefaultTheme } from 'vitepress'
import { buildSidebar } from './sidebar'
import { resolvePlugins } from '../plugins'
import type { ProjectConfig } from './schema'
import { getBuildInfo } from './build-info'
import { resolve } from 'path'

import projectConfig from '../../../project.config'

const primaryLocale = projectConfig.locales.find(l => l.default)!
const defaultTitle = projectConfig.title[primaryLocale.code]
const defaultDesc = projectConfig.description[primaryLocale.code]
const githubUrl = `https://github.com/${projectConfig.github.owner}/${projectConfig.github.repo}`
const editPatternBase = `${githubUrl}/edit/${projectConfig.github.branch}/${projectConfig.github.documentsPath}`

// 构建期从 git 取版本信号,用于页脚展示(Cinux 有 42 个 tag,版本号有真实意义)。
const buildInfo = getBuildInfo()

// Resolve docsRoot relative to this file (site/.vitepress/config/)
const docsRoot = new URL(`../../../${projectConfig.documentsDir}`, import.meta.url).pathname.replace(/\/$/, '')

// Build locales config
function buildLocales(): Record<string, any> {
  const locales: Record<string, any> = {}

  for (const locale of projectConfig.locales) {
    const locKey = locale.default ? 'root' : (locale.prefix?.replace(/\//g, '') || locale.code)
    const title = projectConfig.title[locale.code]
    const desc = projectConfig.description[locale.code]

    const baseConfig: any = {
      label: locale.label,
      lang: locale.code,
      title,
      description: desc,
    }

    if (!locale.default && locale.prefix) {
      baseConfig.link = locale.prefix
    }

    if (!locale.default) {
      baseConfig.themeConfig = {
        nav: projectConfig.nav[locale.code] || [],
        editLink: {
          pattern: `${editPatternBase}${locale.dir ? `/${locale.dir}` : ''}/:path`,
          text: `Edit this page on GitHub`,
        },
      }
    }

    locales[locKey] = baseConfig
  }

  return locales
}

export default defineConfig({
  srcDir: `../${projectConfig.documentsDir}`,
  title: defaultTitle,
  description: defaultDesc,
  lang: primaryLocale.code,
  base: projectConfig.base,
  cleanUrls: true,
  lastUpdated: true,
  ignoreDeadLinks: false,

  vue: {
    template: {
      compilerOptions: {
        isCustomElement: (tag: string) => tag.includes('-') || tag.includes('.'),
      },
    },
  },

  locales: buildLocales(),

  head: [
    ['link', { rel: 'icon', href: projectConfig.favicon || `${projectConfig.base}favicon.ico` }],
    // 首屏立即应用字号档(从 localStorage 读,默认 normal),防刷新闪烁。
    // 与 FontSizeSwitcher.vue 的 STORAGE_KEY('vp-font-size')保持一致。
    [
      'script',
      {},
      `(function(){try{var s=localStorage.getItem('vp-font-size')||'normal';if(s!=='xxsmall'&&s!=='small'&&s!=='normal'&&s!=='large'&&s!=='xxlarge'){s='normal';}document.documentElement.dataset.fontSize=s;}catch(e){}})()`,
    ],
  ],

  markdown: {
    lineNumbers: true,
    math: projectConfig.plugins.math ?? false,
    theme: {
      light: 'github-light',
      dark: 'github-dark',
    },
    config(md) {
      resolvePlugins(md, projectConfig)
    },
  },

  vite: {
    publicDir: resolve(__dirname, '../public'),
    build: {
      chunkSizeWarningLimit: 5000,
    },
    // 防御性:mermaid 只在浏览器从 CDN 加载(见 mermaid-client.ts),这里声明 external
    // 避免 VitePress SSR / optimizeDeps 阶段对 mermaid 做无谓的预构建或报错。
    ssr: {
      external: ['mermaid'],
    },
  },

  themeConfig: {
    nav: projectConfig.nav[primaryLocale.code] || [],
    sidebar: buildSidebar(docsRoot, projectConfig),

    search: {
      provider: 'local',
    },

    editLink: {
      pattern: `${editPatternBase}/:path`,
      text: 'Edit this page on GitHub',
    },

    footer: {
      message: `${buildInfo.version} · ${buildInfo.sha} · ${buildInfo.date}`,
      copyright: projectConfig.copyright,
    },

    socialLinks: [
      { icon: 'github', link: githubUrl },
    ],
  } satisfies DefaultTheme.Config,
})
