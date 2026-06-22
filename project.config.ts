import { defineProject } from './site/.vitepress/config/schema'

export default defineProject({
  name: 'cinux',
  title: { 'zh-CN': 'Cinux' },
  description: { 'zh-CN': '64位OS教程' },
  base: '/Cinux-Book/',
  copyright: 'Copyright © 2026 Charliechen - 保留所有权利',

  documentsDir: 'document',
  siteDir: 'site',

  locales: [
    { code: 'zh-CN', label: '中文', default: true },
  ],

  nav: {
    'zh-CN': [
      { text: '首页', link: '/' },
      { text: '主书', link: '/book/' },
      { text: '实验册', link: '/labs/' },
      {
        text: '参考',
        items: [
          { text: '子系统参考', link: '/reference/' },
          { text: '调试笔记', link: '/debug-notes/' },
        ],
      },
      {
        text: '归档',
        items: [
          { text: '动手实践(hands-on)', link: '/hands-on/' },
          { text: '代码通读(read-through)', link: '/read-through/' },
          { text: '发布教程(tutorial)', link: '/tutorial/' },
          { text: '原始笔记(notes)', link: '/notes/' },
          { text: '开发流程(ci)', link: '/ci/' },
        ],
      },
      { text: 'GitHub', link: 'https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book' },
    ],
  },

  sidebar: {
    volumes: [
      { name: 'book', srcDir: 'book', urlPrefix: '/book' },
      { name: 'labs', srcDir: 'labs', urlPrefix: '/labs' },
      { name: 'reference', srcDir: 'reference', urlPrefix: '/reference' },
      { name: 'debug-notes', srcDir: 'debug-notes', urlPrefix: '/debug-notes' },
      { name: 'hands-on', srcDir: 'hands-on', urlPrefix: '/hands-on' },
      { name: 'read-through', srcDir: 'read-through', urlPrefix: '/read-through' },
      { name: 'tutorial', srcDir: 'tutorial', urlPrefix: '/tutorial' },
      { name: 'notes', srcDir: 'notes', urlPrefix: '/notes' },
      { name: 'ci', srcDir: 'ci', urlPrefix: '/ci' },
    ],
  },

  github: {
    owner: 'Awesome-Embedded-Learning-Studio',
    repo: 'Cinux-Book',
    branch: 'main',
    documentsPath: 'document',
  },

  build: {
    concurrency: 4,
    rootPages: ['index.md'],
    rootAssets: [],
  },

  plugins: {
    cppTemplateEscape: true,
    kbd: true,
    math: true,
    mermaid: true,
    codeFold: true,
  },

  homeBanner: {
    'zh-CN': '🚀 新手必读：从环境搭建开始，请查看 <a href="/Cinux-Book/book/">主书</a>，跟着从零构建你的第一个 x86_64 内核。',
  },
})
