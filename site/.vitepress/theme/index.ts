import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import type { Theme } from 'vitepress'
import './custom.css'
import './article-code.css'
import './article-quote.css'
import './tags.css'

// Cinux 原有组件
import DocNavCards from './components/DocNavCards.vue'
import ScreenshotCarousel from './components/ScreenshotCarousel.vue'
import HomeLaunchpad from './components/HomeLaunchpad.vue'
// 对齐移植:阅读体验
import FontSizeSwitcher from './components/FontSizeSwitcher.vue'
import NavSpinner from './components/NavSpinner.vue'
import ReadingProgress from './components/ReadingProgress.vue'
import ResizableSidebar from './components/ResizableSidebar.vue'
import MermaidLightbox from './components/MermaidLightbox.vue'
// 对齐移植:首页
import HomeHeroVisual from './components/HomeHeroVisual.vue'
import ProofStrip from './components/ProofStrip.vue'
import HomePathExplorer from './components/HomePathExplorer.vue'
// 对齐移植:内容系统
import DocTags from './components/DocTags.vue'
import { setupMermaid } from './mermaid-client'

export default {
  extends: DefaultTheme,
  Layout() {
    return h(DefaultTheme.Layout, null, {
      // 全局:路由切换 spinner + 阅读进度条 + 可拖拽侧栏 + mermaid 灯箱
      'layout-top': () => [
        h(NavSpinner),
        h(ReadingProgress),
        h(ResizableSidebar),
        h(MermaidLightbox),
      ],
      // 文档页底部(上下页导航之前):主题标签徽章
      'doc-footer-before': () => h(DocTags),
      // 上下页导航之后:Cinux 原有的分区导航卡片
      'doc-after': () => h(DocNavCards),
      // 首页 hero 右侧视觉
      'home-hero-image': () => h(HomeHeroVisual),
      // hero 下方:课程属性条
      'home-hero-after': () => h(ProofStrip),
      // 首页正文:入口控制台 → 实机画面 → 17 卷成长路线。
      // 默认 VPFeatures 隐藏,避免同一批入口被重复展示。
      'home-features-before': () => [
        h(HomeLaunchpad),
        h(ScreenshotCarousel),
      ],
      'home-features-after': () => h(HomePathExplorer),
      // 字号切换器:桌面顶栏 + 移动端抽屉各一份
      'nav-bar-content-after': () => h(FontSizeSwitcher),
      'nav-screen-content-after': () => h(FontSizeSwitcher),
    })
  },
  setup() {
    // mermaid 图在客户端从 CDN 懒加载渲染,切路由后重渲新图;
    // 灯箱/路由钩子在组件内部自注册
    setupMermaid()
  },
  enhanceApp({ app }) {
    // md 正文中可全局使用的组件(escape-cpp-templates 的 VUE_COMPONENTS
    // 白名单与之保持同步)
    app.component('ChapterNav', () => import('./components/ChapterNav.vue'))
    app.component('ChapterLink', () => import('./components/ChapterLink.vue'))
    app.component('TagExplorer', () => import('./components/TagExplorer.vue'))
    app.component('ReferenceCard', () => import('./components/ReferenceCard.vue'))
    app.component('ReferenceItem', () => import('./components/ReferenceItem.vue'))
    app.component('RefLink', () => import('./components/RefLink.vue'))
    app.component('Anim', () => import('./components/Anim.vue'))
    app.component('CheckpointProblem', () => import('./components/CheckpointProblem.vue'))
    app.component('CheckpointList', () => import('./components/CheckpointList.vue'))
    app.component('QuizProgressBackup', () => import('./components/QuizProgressBackup.vue'))
  },
} satisfies Theme
