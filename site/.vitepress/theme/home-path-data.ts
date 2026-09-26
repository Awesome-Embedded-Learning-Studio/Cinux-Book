/**
 * 首页「系统构建路线图」数据。
 *
 * 图只链接当前站内真实存在的入口；尚未重写完成的后续站点不制造空链接。
 * 节点坐标既用于 SSG 首屏，也会被 HomePathGraph 的客户端拖拽布局覆盖。
 */

export type PathNodeKind = 'root' | 'proj' | 'sup'
export type PathEdgeKind = 'solid' | 'dash' | 'dot'
export type PathSide = 'top' | 'right' | 'bottom' | 'left'
export type PathRouteCoord = number | 'from' | 'to'
export type PathStatus = 'done' | 'doing'
export type PathTier = 'core' | 'eng' | 'domain'

export interface Bi {
  cn: string
  en: string
}

export interface PathNode {
  id: string
  name: Bi
  sub: Bi
  x: number
  y: number
  w: number
  h: number
  kind: PathNodeKind
  status: PathStatus
  tier: PathTier
  href: string
  badge: string
}

export interface PathEdge {
  from: string
  to: string
  kind: PathEdgeKind
  route?: {
    from: PathSide
    to: PathSide
    via?: Array<{ x: PathRouteCoord; y: PathRouteCoord }>
  }
}

export interface PathBand {
  label: Bi
  top: number
  bottom: number
}

export const VB_W = 1330
export const VB_H = 720
export const HOME_GRAPH_REVISION = 1

export const HOME_PATH_BANDS: PathBand[] = [
  { label: { cn: 'B0 · 选择起点', en: 'B0 · Choose a start' }, top: 16, bottom: 114 },
  { label: { cn: 'B1 · 备齐前置知识', en: 'B1 · Prepare' }, top: 142, bottom: 270 },
  { label: { cn: 'B2 · 踏上主线', en: 'B2 · Main journey' }, top: 298, bottom: 426 },
  { label: { cn: 'B3 · 查证、排错与工程记录', en: 'B3 · Support' }, top: 454, bottom: 704 },
]

export const HOME_PATH_NODES: PathNode[] = [
  {
    id: 'start', name: { cn: '从这里出发', en: 'Start here' },
    sub: { cn: '按你的底子选择路线', en: 'Choose your route' },
    x: 665, y: 64, w: 190, h: 66, kind: 'root', status: 'done', tier: 'core',
    href: '/journey/', badge: 'GO',
  },
  {
    id: 'toolchain', name: { cn: '工具链', en: 'Toolchain' },
    sub: { cn: '编译 · 链接 · QEMU', en: 'Build · link · QEMU' },
    x: 250, y: 206, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'core',
    href: '/primer/01-toolchain/', badge: 'P1',
  },
  {
    id: 'assembly', name: { cn: '汇编基础', en: 'Assembly' },
    sub: { cn: 'GAS · 寻址 · 系统指令', en: 'GAS · addressing' },
    x: 545, y: 206, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'core',
    href: '/primer/02-assembly/', badge: 'P2',
  },
  {
    id: 'cpp', name: { cn: '受约束的 C++', en: 'Freestanding C++' },
    sub: { cn: 'C 核心 · freestanding', en: 'C core · freestanding' },
    x: 840, y: 206, w: 180, h: 58, kind: 'proj', status: 'done', tier: 'core',
    href: '/primer/03-cpp/', badge: 'P3',
  },
  {
    id: 'tamcpp', name: { cn: '现代 C++ 补给站', en: 'Modern C++' },
    sub: { cn: '兄弟站点 TAMCPP', en: 'TAMCPP companion' },
    x: 1110, y: 206, w: 170, h: 46, kind: 'sup', status: 'done', tier: 'domain',
    href: 'https://awesome-embedded-learning-studio.github.io/Tutorial_AwesomeModernCPP/', badge: 'C++',
  },
  {
    id: 'journey', name: { cn: 'Cinux 构建之旅', en: 'Cinux Journey' },
    sub: { cn: '跟着真实开发史重走', en: 'Rebuild the real history' },
    x: 430, y: 362, w: 190, h: 66, kind: 'proj', status: 'doing', tier: 'eng',
    href: '/journey/', badge: 'MAIN',
  },
  {
    id: 'armory', name: { cn: '00 · 武器库', en: '00 · Armory' },
    sub: { cn: 'Result · format · assert · test', en: 'Core utilities' },
    x: 760, y: 362, w: 210, h: 66, kind: 'proj', status: 'doing', tier: 'eng',
    href: '/journey/00-armory/', badge: '00',
  },
  {
    id: 'next', name: { cn: '下一站装填中', en: 'Next stop loading' },
    sub: { cn: 'Bootloader 即将发车', en: 'Bootloader is next' },
    x: 1065, y: 362, w: 175, h: 46, kind: 'sup', status: 'doing', tier: 'eng',
    href: '/journey/', badge: 'NEXT',
  },
  {
    id: 'reference', name: { cn: '子系统参考', en: 'Reference' },
    sub: { cn: '寄存器 · 接口 · 边界', en: 'Registers · APIs' },
    x: 260, y: 530, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'domain',
    href: '/reference/', badge: 'REF',
  },
  {
    id: 'debug', name: { cn: '排错笔记', en: 'Debug notes' },
    sub: { cn: '真实故障 · 定位 · 收敛', en: 'Real failure cases' },
    x: 550, y: 530, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'domain',
    href: '/debug-notes/', badge: 'DBG',
  },
  {
    id: 'notes', name: { cn: '原始笔记', en: 'Raw notes' },
    sub: { cn: '开发现场与演化痕迹', en: 'Development log' },
    x: 840, y: 530, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'domain',
    href: '/notes/', badge: 'LOG',
  },
  {
    id: 'ci', name: { cn: '开发流程', en: 'Engineering' },
    sub: { cn: 'CI · 验证 · 工程约定', en: 'CI · verification' },
    x: 1110, y: 530, w: 170, h: 58, kind: 'proj', status: 'done', tier: 'domain',
    href: '/ci/', badge: 'CI',
  },
  {
    id: 'tags', name: { cn: '标签索引', en: 'Tags' },
    sub: { cn: '按主题横向检索', en: 'Browse by topic' },
    x: 665, y: 650, w: 160, h: 46, kind: 'sup', status: 'done', tier: 'domain',
    href: '/tags', badge: 'IDX',
  },
]

export const HOME_PATH_EDGES: PathEdge[] = [
  { from: 'start', to: 'toolchain', kind: 'solid', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 128 }, { x: 'to', y: 128 }] } },
  { from: 'toolchain', to: 'assembly', kind: 'solid', route: { from: 'right', to: 'left' } },
  { from: 'assembly', to: 'cpp', kind: 'solid', route: { from: 'right', to: 'left' } },
  { from: 'cpp', to: 'journey', kind: 'solid', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 284 }, { x: 'to', y: 284 }] } },
  { from: 'journey', to: 'armory', kind: 'solid', route: { from: 'right', to: 'left' } },
  { from: 'armory', to: 'next', kind: 'solid', route: { from: 'right', to: 'left' } },

  { from: 'start', to: 'journey', kind: 'dash', route: { from: 'left', to: 'top', via: [{ x: 120, y: 'from' }, { x: 120, y: 284 }, { x: 'to', y: 284 }] } },
  { from: 'cpp', to: 'tamcpp', kind: 'dash', route: { from: 'right', to: 'left' } },

  { from: 'armory', to: 'reference', kind: 'dot', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 440 }, { x: 'to', y: 440 }] } },
  { from: 'armory', to: 'debug', kind: 'dot', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 446 }, { x: 'to', y: 446 }] } },
  { from: 'armory', to: 'notes', kind: 'dot', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 452 }, { x: 'to', y: 452 }] } },
  { from: 'armory', to: 'ci', kind: 'dot', route: { from: 'bottom', to: 'top', via: [{ x: 'from', y: 458 }, { x: 'to', y: 458 }] } },
  { from: 'reference', to: 'tags', kind: 'dot', route: { from: 'bottom', to: 'left', via: [{ x: 'from', y: 650 }] } },
  { from: 'debug', to: 'tags', kind: 'dot', route: { from: 'bottom', to: 'left', via: [{ x: 'from', y: 650 }] } },
  { from: 'notes', to: 'tags', kind: 'dot', route: { from: 'bottom', to: 'right', via: [{ x: 'from', y: 650 }] } },
  { from: 'ci', to: 'tags', kind: 'dot', route: { from: 'bottom', to: 'right', via: [{ x: 'from', y: 650 }] } },
]
