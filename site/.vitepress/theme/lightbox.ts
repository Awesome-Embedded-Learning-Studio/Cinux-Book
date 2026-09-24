// MediaLightbox 的 opener 桥接。
//
// 命令式挂载的放大按钮(figure-zoom.ts,非 Vue 组件)需要触发一个挂在 Layout 上的
// Vue 模态组件。这里用模块级单例函数:组件挂载时 registerLightboxOpener 注册回调,
// 点放大时调 openLightbox。不引入 vue 的 reactive,避免 SSR 阶段对 DOM 引用做代理。
//
// 泛化说明:原先只接 mermaid 的 SVGElement。插图同样需要放大(截图类内容尤其),
// 因此 node 收 SVG 与 img 两类,由组件按各自的内禀尺寸算初始缩放。

export interface LightboxPayload {
  /** 已渲染好的图元素(内联 SVG 或 img),模态里会 cloneNode,不动原图 */
  node: SVGElement | HTMLImageElement
  /** 无障碍描述:流程图传源码,插图传图注/alt */
  label: string
  /** 触发按钮,模态关闭后焦点还回这里 */
  trigger: HTMLElement
}

type Opener = (payload: LightboxPayload) => void

let opener: Opener | null = null

/** MediaLightbox 组件挂载时调用,返回卸载函数。 */
export function registerLightboxOpener(fn: Opener): () => void {
  opener = fn
  return () => {
    if (opener === fn) opener = null
  }
}

/** 放大按钮点击时调用。组件未挂载时静默 no-op。 */
export function openLightbox(payload: LightboxPayload): void {
  opener?.(payload)
}
