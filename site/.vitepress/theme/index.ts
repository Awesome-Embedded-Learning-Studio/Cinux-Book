import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import type { Theme } from 'vitepress'
import './custom.css'
import './article-doc.css'
import './article-code.css'
import './article-quote.css'
import './article-figure.css'
import './article-nav.css'
import './tags.css'

// Cinux 原有组件
import DocNavCards from './components/DocNavCards.vue'
// v2 重走:轮播/卷地图暂离首页(见 home-features 挂载点注释),组件保留待改造
// import ScreenshotCarousel from './components/ScreenshotCarousel.vue'
import HomeLaunchpad from './components/HomeLaunchpad.vue'
// 对齐移植:阅读体验
import FontSizeSwitcher from './components/FontSizeSwitcher.vue'
import NavSpinner from './components/NavSpinner.vue'
import ReadingProgress from './components/ReadingProgress.vue'
import ResizableSidebar from './components/ResizableSidebar.vue'
import MediaLightbox from './components/MediaLightbox.vue'
// 对齐移植:首页
import HomeHeroVisual from './components/HomeHeroVisual.vue'
import HomePathExplorer from './components/HomePathExplorer.vue'
import ChapterNav from './components/ChapterNav.vue'
import ChapterLink from './components/ChapterLink.vue'
import JourneyRoadmap from './components/JourneyRoadmap.vue'
import StationPanel from './components/StationPanel.vue'
// 对齐移植:内容系统
import DocTags from './components/DocTags.vue'
import { setupMermaid } from './mermaid-client'
import { setupFigureZoom } from './figure-zoom'

export default {
  extends: DefaultTheme,
  Layout() {
    return h(DefaultTheme.Layout, null, {
      // 全局:路由切换 spinner + 阅读进度条 + 可拖拽侧栏 + 图片/流程图灯箱
      'layout-top': () => [
        h(NavSpinner),
        h(ReadingProgress),
        h(ResizableSidebar),
        h(MediaLightbox),
      ],
      // 文档页底部(上下页导航之前):主题标签徽章
      'doc-footer-before': () => h(DocTags),
      // 上下页导航之后:Cinux 原有的分区导航卡片
      'doc-after': () => h(DocNavCards),
      // 首页 hero 右侧视觉
      'home-hero-image': () => h(HomeHeroVisual),
      // 首页正文:入口控制台 + 可拖拽、可缩放的系统构建路线。
      // 默认 VPFeatures 隐藏,避免同一批入口被重复展示。
      'home-features-before': () => [
        h(HomeLaunchpad),
        // h(ScreenshotCarousel),
      ],
      'home-features-after': () => h(HomePathExplorer),
      // 字号切换器:桌面顶栏 + 移动端抽屉各一份
      'nav-bar-content-after': () => h(FontSizeSwitcher),
      'nav-screen-content-after': () => h(FontSizeSwitcher),
    })
  },
  setup() {
    // mermaid 图在客户端懒加载渲染,切路由后重渲新图;
    // 灯箱/路由钩子在组件内部自注册
    setupMermaid()
    // 插图挂放大按钮(流程图的按钮由 mermaid-client 渲染完后挂,共用同一实现)
    setupFigureZoom()
  },
  enhanceApp({ app }) {
    // md 正文中可全局使用的组件(escape-cpp-templates 的 VUE_COMPONENTS
    // 白名单与之保持同步)
    app.component('ChapterNav', ChapterNav)
    app.component('ChapterLink', ChapterLink)
    app.component('JourneyRoadmap', JourneyRoadmap)
    app.component('StationPanel', StationPanel)
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
