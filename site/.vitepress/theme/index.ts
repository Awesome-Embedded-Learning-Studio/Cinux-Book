import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import type { Theme } from 'vitepress'
import HomeTipBanner from './components/HomeTipBanner.vue'
import HomeArchDiagram from './components/HomeArchDiagram.vue'
import DocNavCards from './components/DocNavCards.vue'
import ScreenshotCarousel from './components/ScreenshotCarousel.vue'
import FontSizeSwitcher from './components/FontSizeSwitcher.vue'
import { setupMermaid } from './mermaid-client'
import projectConfig from '../../../project.config.ts'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout() {
    return h(DefaultTheme.Layout, null, {
      // 首页 features 之前:先睹为快轮播 + 新手提示条
      'home-features-before': () =>
        h('div', { class: 'home-pre-features' }, [
          h(ScreenshotCarousel),
          h(HomeTipBanner, { config: projectConfig }),
        ]),
      'home-features-after': () => h(HomeArchDiagram),
      'doc-after': () => h(DocNavCards),
      // 字号切换器:桌面顶栏 + 移动端抽屉各一份
      'nav-bar-content-after': () => h(FontSizeSwitcher),
      'nav-screen-content-after': () => h(FontSizeSwitcher),
    })
  },
  setup() {
    // mermaid 图在客户端从 CDN 懒加载渲染,切路由后重渲新图
    setupMermaid()
  },
} satisfies Theme
