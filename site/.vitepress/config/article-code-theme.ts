import type { ThemeRegistration } from 'shiki'
import githubLight from 'shiki/themes/github-light.mjs'
import githubDark from 'shiki/themes/github-dark.mjs'

// 保留 github 主题的语法作用域,只整体调成 Cinux 的「冰川青」文章色板:
// 字符串/标题类令牌偏向品牌青绿,注释偏冷灰绿,保持与站点主色一致的可读对比度。
function recolor(theme: ThemeRegistration, name: string, palette: Record<string, string>): ThemeRegistration {
  const color = (value: string | undefined) => value && (palette[value.toLowerCase()] ?? value)
  return {
    ...theme,
    name,
    colors: {
      ...theme.colors,
      'editor.foreground': color(theme.colors?.['editor.foreground'])!,
    },
    tokenColors: theme.tokenColors?.map((rule) => ({
      ...rule,
      settings: { ...rule.settings, foreground: color(rule.settings.foreground) },
    })),
  }
}

export const articleCodeThemes = {
  light: recolor(githubLight, 'cinux-article-light', {
    '#24292e': '#2f3a38',
    '#6a737d': '#6d7b77',
    '#d73a49': '#9e4d5f',
    '#005cc5': '#20688a',
    '#6f42c1': '#5f6ba8',
    '#032f62': '#0f5f52',
    '#22863a': '#2f7a4f',
    '#e36209': '#a8642c',
  }),
  dark: recolor(githubDark, 'cinux-article-dark', {
    '#e1e4e8': '#d7e5e0',
    '#6a737d': '#8fa39d',
    '#f97583': '#e29aac',
    '#79b8ff': '#7fc4cf',
    '#b392f0': '#b3a9e3',
    '#9ecbff': '#9adbc9',
    '#85e89d': '#93cfa4',
    '#ffab70': '#e0bb85',
  }),
}
