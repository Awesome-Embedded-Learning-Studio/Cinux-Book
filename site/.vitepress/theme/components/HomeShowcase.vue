<script setup lang="ts">
/**
 * 首页实拍区。
 *
 * 全页唯一的深色通栏：首页从上到下是 亮(hero) → 亮(入口) → 暗(本区) → 亮(路线)，
 * 靠明暗交替造出节奏。截图本身是浅色桌面配深色窗口，放在暖黑底上边缘更干净。
 *
 * 左边录屏、右边桌面，讲的是同一块屏的「过程 / 结果」。两张并排，不上下堆：
 * 堆起来是同一个桌面拍两遍，占掉两屏还看不出差别。
 *
 * 录屏不设 autoplay 属性:进视口才调 play(),离屏就 pause()。首屏少拉 530 KB，
 * 也让循环播放不至于在读者滚走以后还自己转。prefers-reduced-motion 打开时
 * 一律不自动播,给控制条让读者自己点。
 */
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { openLightbox } from '../lightbox'
import filmSrc from '../../../../assets/README/boot_cli.mp4'
import posterSrc from '../../../../assets/README/boot_cli_poster.webp'
import imgDesktop from '../../../../assets/README/welcome.webp'

const shot = ref<HTMLImageElement | null>(null)
const film = ref<HTMLVideoElement | null>(null)
let observer: IntersectionObserver | undefined

async function zoom() {
  const node = shot.value
  if (!node) return
  // 懒加载的图可能还没解码完就点上了,等它落定再开灯箱,否则克隆出来是张空图。
  if (!node.complete) {
    await new Promise<void>((resolve) => {
      node.addEventListener('load', () => resolve(), { once: true })
      node.addEventListener('error', () => resolve(), { once: true })
    })
  }
  openLightbox({ node, label: 'Vesper 桌面：About 与 Terminal 两个窗口', trigger: node })
}

onMounted(() => {
  const el = film.value
  if (!el || typeof IntersectionObserver === 'undefined') return

  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    el.controls = true
    return
  }

  observer = new IntersectionObserver(
    (entries) => {
      const entry = entries[0]
      if (!entry) return
      if (entry.isIntersecting) void el.play().catch(() => {})
      else el.pause()
    },
    { threshold: 0.3 },
  )
  observer.observe(el)
})

onBeforeUnmount(() => observer?.disconnect())
</script>

<template>
  <section class="proof" aria-labelledby="proof-title">
    <div class="proof__inner">
      <header class="proof__head">
        <h2 id="proof-title">跑起来是这样</h2>
        <p>
          左边二十秒，从敲下命令到桌面亮起来；右边是进去之后的样子。两样都是在 QEMU 里真跑出来的。
        </p>
      </header>

      <div class="proof__pair">
        <figure class="proof__item">
          <video
            ref="film"
            class="proof__film"
            :src="filmSrc"
            :poster="posterSrc"
            width="1000"
            height="624"
            muted
            loop
            playsinline
            preload="none"
            disablepictureinpicture
            aria-label="从 run_gui 到桌面的二十秒录屏"
          />
          <figcaption>
            <code>cmake --build build --target run_gui -j</code> 之后录的二十秒：终端、开机日志、桌面。
          </figcaption>
        </figure>

        <figure class="proof__item">
          <button type="button" class="proof__frame" title="放大查看" @click="zoom">
            <img
              ref="shot"
              :src="imgDesktop"
              width="1000"
              height="670"
              loading="lazy"
              decoding="async"
              alt="Vesper 桌面：About 与 Terminal 两个窗口"
            />
          </button>
          <figcaption>About 与 Terminal 两个窗口，左边一列桌面图标。点图放大。</figcaption>
        </figure>
      </div>

      <div class="proof__notes">
        <p>
          About 窗口里那句 <em>A desktop on a bare framebuffer</em> 是它的自我介绍：桌面直接画在帧缓冲上，双击开图标，按住标题栏拖窗口。
        </p>
        <p>
          Terminal 里敲的 <code>echo</code> 回显当场就出来，光标停在 <code>$</code> 后头等下一行。
        </p>
      </div>
    </div>
  </section>
</template>

<style scoped>
.proof {
  margin-top: 64px;
  padding: 56px 0 60px;
  background: #16120d;
  border-block: 1px solid #2b241a;
}
.dark .proof { background: #120f0b; }

.proof__inner { max-width: 1152px; margin: 0 auto; padding: 0 24px; }

.proof__head { max-width: 44em; margin-bottom: 28px; }
.proof__head h2 {
  margin: 0;
  color: #f5efe2;
  font-size: clamp(22px, 2.4vw, 30px);
  font-weight: 700;
  line-height: 1.26;
  letter-spacing: -0.03em;
}
.proof__head p {
  margin: 12px 0 0;
  color: #a89d89;
  font-size: 13.5px;
  line-height: 1.8;
}
.proof__head code,
.proof__notes code,
.proof__item figcaption code {
  padding: 1px 5px;
  color: #d9cfba;
  background: rgba(255, 255, 255, 0.06);
  border-radius: 4px;
  font-family: var(--vp-font-family-mono);
  font-size: 0.92em;
}

.proof__pair {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 24px;
  align-items: start;
}

.proof__item { margin: 0; min-width: 0; }

.proof__film,
.proof__frame {
  display: block;
  width: 100%;
  overflow: hidden;
  background: #0d0a07;
  border: 1px solid #2f281e;
  border-radius: 10px;
}
.proof__film { height: auto; }

.proof__frame {
  padding: 0;
  cursor: zoom-in;
  transition: border-color 0.18s ease;
}
.proof__frame:hover,
.proof__frame:focus-visible { border-color: #4a3f2e; }
.proof__frame img { display: block; width: 100%; height: auto; }

.proof__item figcaption {
  margin-top: 11px;
  color: #7d7362;
  font: 11.5px/1.7 var(--vp-font-family-mono);
  letter-spacing: 0.01em;
}

.proof__notes {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px 32px;
  margin-top: 26px;
  padding-top: 20px;
  border-top: 1px solid #2b241a;
}
.proof__notes p {
  margin: 0;
  color: #a89d89;
  font-size: 13px;
  line-height: 1.85;
}
.proof__notes em { color: #cdc2ab; font-style: normal; }

@media (max-width: 900px) {
  .proof { margin-top: 48px; padding: 44px 0 48px; }
  .proof__pair { grid-template-columns: 1fr; gap: 20px; }
  .proof__notes { grid-template-columns: 1fr; gap: 14px; }
}
@media (max-width: 639px) {
  .proof__inner { padding-inline: 18px; }
  .proof__head { margin-bottom: 20px; }
}
</style>
