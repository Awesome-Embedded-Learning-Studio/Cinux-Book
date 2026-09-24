<template>
  <!-- Teleport 到 body:盖在整页最上层,不被正文 stacking context 裁剪。 -->
  <Teleport to="body">
    <div
      v-if="open"
      class="media-lightbox"
      role="dialog"
      aria-modal="true"
      :aria-label="label || '放大查看'"
    >
      <div class="media-lightbox__toolbar" role="toolbar" aria-label="缩放控制">
        <button
          ref="firstBtnRef"
          type="button"
          class="media-lightbox__btn"
          title="放大"
          @click="zoomIn"
        >
          放大
        </button>
        <button
          type="button"
          class="media-lightbox__btn"
          title="缩小"
          @click="zoomOut"
        >
          缩小
        </button>
        <button
          type="button"
          class="media-lightbox__btn"
          title="复位 / 适应窗口"
          @click="reset"
        >
          复位
        </button>
        <span class="media-lightbox__hint">滚轮缩放 · 拖拽平移 · 双指缩放</span>
        <button
          type="button"
          class="media-lightbox__btn media-lightbox__close"
          title="关闭 (Esc)"
          aria-label="关闭"
          @click="close"
        >
          ✕
        </button>
      </div>

      <!-- @click.self:点舞台空白区关闭;点图本身(panzoom 接管拖拽)不关闭。 -->
      <div
        ref="stageRef"
        class="media-lightbox__stage"
        @click.self="close"
      >
        <div ref="targetRef" class="media-lightbox__target" />
      </div>

      <p v-if="label" class="media-lightbox__cap">{{ label }}</p>
    </div>
  </Teleport>
</template>

<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref } from 'vue'
import { registerLightboxOpener, type LightboxPayload } from '../lightbox'

// panzoom 只在 mountMedia 里动态 import,不进 SSR bundle,也不进首屏 chunk。
// 这里用本地最小类型,避免静态 import 类型把库拽进来。
interface PanzoomInstance {
  zoomIn: (opts?: unknown) => void
  zoomOut: (opts?: unknown) => void
  reset: (opts?: unknown) => void
  zoomWithWheel: (event: WheelEvent) => void
  destroy: () => void
}

const open = ref(false)
const label = ref('')
const stageRef = ref<HTMLElement | null>(null)
const targetRef = ref<HTMLElement | null>(null)
const firstBtnRef = ref<HTMLElement | null>(null)

let panzoom: PanzoomInstance | null = null
let payload: LightboxPayload | null = null
let wheelHandler: ((e: WheelEvent) => void) | null = null
let keyHandler: ((e: KeyboardEvent) => void) | null = null
let prevOverflow = ''

// 挂载即注册 opener;figure-zoom / mermaid-client 点放大会触发 openDialog。
const unregister = registerLightboxOpener((p) => {
  payload = p
  label.value = p.label
  openDialog()
})

function openDialog() {
  open.value = true
  prevOverflow = document.body.style.overflow
  document.body.style.overflow = 'hidden'

  keyHandler = (e: KeyboardEvent) => {
    if (!open.value) return
    if (e.key === 'Escape') {
      e.preventDefault()
      close()
    } else if (e.key === 'Tab') {
      trapFocus(e)
    }
  }
  window.addEventListener('keydown', keyHandler)

  // 等 Teleport 内容挂到 DOM 后再 clone 节点 + 挂 panzoom。
  void nextTick(() => {
    void mountMedia().then(() => firstBtnRef.value?.focus())
  })
}

/** 取节点的内禀尺寸:SVG 读 viewBox,img 读 naturalWidth。 */
function intrinsicSize(node: SVGElement | HTMLImageElement): { w: number; h: number } {
  if (node instanceof HTMLImageElement) {
    return { w: node.naturalWidth || node.clientWidth, h: node.naturalHeight || node.clientHeight }
  }
  // 注意 svg.viewBox.baseVal 在某些浏览器的 detached clone 上会返回 0
  // → 直接解析 viewBox 属性字符串最稳("minX minY W H")。
  const vbAttr = node.getAttribute('viewBox')
  if (vbAttr) {
    const p = vbAttr.trim().split(/[\s,]+/).map(Number)
    if (p.length >= 4 && Number.isFinite(p[2]) && Number.isFinite(p[3])) {
      return { w: p[2], h: p[3] }
    }
  }
  return { w: 0, h: 0 }
}

async function mountMedia() {
  if (!payload || !stageRef.value || !targetRef.value) return

  const node = payload.node.cloneNode(true) as SVGElement | HTMLImageElement
  targetRef.value.innerHTML = ''
  targetRef.value.appendChild(node)

  // 关键:必须给 clone 显式像素宽高,否则 SVG 要么塌成 300x150、要么按 mermaid 的
  // width="100%" 撑成内禀尺寸(巨大、只剩左上角);img 则会被 CSS max-width 钳住。
  node.removeAttribute('style')
  const { w, h } = intrinsicSize(payload.node)

  if (w > 0 && h > 0) {
    // 居中(flex 已处理)+ 占舞台 85%,留出舒服边距;min(...,1) 大图缩到适应、小图不放大。
    const maxW = Math.max(160, stageRef.value.clientWidth * 0.85)
    const maxH = Math.max(160, stageRef.value.clientHeight * 0.85)
    const scale = Math.min(maxW / w, maxH / h, 1)
    node.setAttribute('width', String(Math.round(w * scale)))
    node.setAttribute('height', String(Math.round(h * scale)))
  }
  ;(node as HTMLElement).style.display = 'block'
  // 插图是位图,放大时保留像素边界比模糊插值更利于看清截图里的文字
  if (node instanceof HTMLImageElement) {
    node.style.imageRendering = 'auto'
    node.draggable = false
  }

  const { default: createPanzoom } = await import('@panzoom/panzoom')
  // targetRef(包图的 div)做 panzoom 目标:CSS transform 挂 HTML div,
  // 绕开 SVG 坐标系/viewBox/foreignObject 一切争议。
  panzoom = createPanzoom(targetRef.value, {
    maxScale: 8,
    minScale: 0.3, // 允许缩到比 fit 更小(minScale:1 时缩小按钮被钳住、点了没反应)
    step: 0.25,
    cursor: 'grab',
  })

  wheelHandler = (e: WheelEvent) => panzoom?.zoomWithWheel(e)
  stageRef.value.addEventListener('wheel', wheelHandler, { passive: false })
}

function trapFocus(e: KeyboardEvent) {
  const root = stageRef.value?.parentElement
  if (!root) return
  const focusables = Array.from(
    root.querySelectorAll<HTMLElement>('button, [href], input, [tabindex]:not([tabindex="-1"])'),
  ).filter((el) => el.offsetParent !== null)
  if (focusables.length === 0) return
  const first = focusables[0]
  const last = focusables[focusables.length - 1]
  if (e.shiftKey && document.activeElement === first) {
    e.preventDefault()
    last.focus()
  } else if (!e.shiftKey && document.activeElement === last) {
    e.preventDefault()
    first.focus()
  }
}

function close() {
  if (!open.value) return
  open.value = false
  teardown()
  payload?.trigger?.focus?.()
  payload = null
  label.value = ''
}

function teardown() {
  if (wheelHandler && stageRef.value) {
    stageRef.value.removeEventListener('wheel', wheelHandler)
  }
  wheelHandler = null
  panzoom?.destroy()
  panzoom = null
  if (keyHandler) {
    window.removeEventListener('keydown', keyHandler)
    keyHandler = null
  }
  document.body.style.overflow = prevOverflow
  if (targetRef.value) targetRef.value.innerHTML = ''
}

function zoomIn() {
  panzoom?.zoomIn()
}
function zoomOut() {
  panzoom?.zoomOut()
}
function reset() {
  panzoom?.reset()
}

onBeforeUnmount(() => {
  unregister()
  if (open.value) teardown()
})
</script>
