import type { ProjectConfig } from './schema'

// 构建 VitePress locales 配置。抽成独立模块作为单一真相源,
// 供 dev/single 构建(config/index.ts)与分卷构建的 root config 复用,
// 避免双语 locale 逻辑散落多处导致漏改。
export function buildLocales(config: ProjectConfig): Record<string, any> {
  const githubUrl = `https://github.com/${config.github.owner}/${config.github.repo}`
  const editPatternBase = `${githubUrl}/edit/${config.github.branch}/${config.documentsPath}`

  const locales: Record<string, any> = {}

  for (const locale of config.locales) {
    const locKey = locale.default ? 'root' : (locale.prefix?.replace(/\//g, '') || locale.code)
    const title = config.title[locale.code]
    const desc = config.description[locale.code]

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
        nav: config.nav[locale.code] || [],
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
