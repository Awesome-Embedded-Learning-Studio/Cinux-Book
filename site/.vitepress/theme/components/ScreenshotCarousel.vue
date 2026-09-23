<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { withBase } from 'vitepress'
import imgGui from '../../../../assets/README/gui.webp'
import imgCli from '../../../../assets/README/cli.webp'
import imgBoot from '../../../../assets/README/boot.webp'
import imgMulti from '../../../../assets/README/multi-terminal.webp'
import imgParallel from '../../../../assets/README/parallel.webp'
import imgFs from '../../../../assets/README/filesystem.webp'

const slides = [
  { img: imgBoot, code: 'BOOT', title: '从 0x7C00 点亮第一屏', note: '实模式、GDT、分页与 Long Mode', href: '/book/01-boot/' },
  { img: imgParallel, code: 'TASK', title: '让进程真正并发运行', note: '上下文切换、独立地址空间与调度', href: '/book/10-multitasking/' },
  { img: imgFs, code: 'VFS', title: '把磁盘接入统一文件世界', note: '块设备、VFS、Ext2 与路径解析', href: '/book/08-filesystem/' },
  { img: imgCli, code: 'RING3', title: '从内核走进用户空间', note: '系统调用、ELF 加载与 Shell', href: '/book/07-userland/' },
  { img: imgGui, code: 'GUI', title: '最终长成自己的桌面', note: '帧缓冲、窗口管理与输入系统', href: '/book/09-gui/' },
  { img: imgMulti, code: 'TTY', title: '多终端同时工作', note: '会话、终端与 fork / exec', href: '/book/10-multitasking/' },
]

const active = ref(0)
const current = computed(() => slides[active.value])
let timer: ReturnType<typeof setInterval> | undefined
function stop() { if (timer) clearInterval(timer); timer = undefined }
function start() { stop(); timer = setInterval(() => { active.value = (active.value + 1) % slides.length }, 6500) }
function select(index: number) { active.value = index; start() }
onMounted(start)
onBeforeUnmount(stop)
</script>

<template>
  <section class="system-showcase" @mouseenter="stop" @mouseleave="start">
    <header class="system-showcase__head">
      <div>
        <span class="showcase-kicker">RUNNING SYSTEM / NOT A TOY KERNEL</span>
        <h2>你要构建的，不只是一句 Hello World</h2>
      </div>
      <p>教程中的每个画面都来自同一套 Cinux 源码。选择一个系统切面，直接进入对应章节。</p>
    </header>

    <div class="system-showcase__body">
      <a class="showcase-screen" :href="withBase(current.href)">
        <img :key="current.img" :src="current.img" :alt="current.title" />
        <span class="showcase-screen__scan" aria-hidden="true" />
        <span class="showcase-screen__badge">CINUX / {{ current.code }}</span>
        <span class="showcase-screen__caption">
          <strong>{{ current.title }}</strong>
          <small>{{ current.note }}</small>
        </span>
      </a>

      <div class="showcase-index" role="tablist" aria-label="系统功能预览">
        <button
          v-for="(slide, index) in slides"
          :key="slide.code"
          type="button"
          role="tab"
          :aria-selected="index === active"
          :class="{ 'is-active': index === active }"
          @click="select(index)"
        >
          <span class="showcase-index__num">0{{ index + 1 }}</span>
          <span class="showcase-index__copy"><b>{{ slide.code }}</b><small>{{ slide.title }}</small></span>
          <span class="showcase-index__tick" aria-hidden="true" />
        </button>
      </div>
    </div>
  </section>
</template>

<style scoped>
.system-showcase { max-width: 1152px; margin: 0 auto; padding: 58px 24px 70px; }
.system-showcase__head { display: grid; grid-template-columns: 1.2fr 0.8fr; align-items: end; gap: 48px; margin-bottom: 26px; }
.showcase-kicker { color: var(--vp-c-brand-1); font: 700 11px/1 var(--vp-font-family-mono); letter-spacing: 0.13em; }
.system-showcase__head h2 { margin: 12px 0 0; color: var(--vp-c-text-1); font-size: clamp(25px, 3vw, 38px); line-height: 1.18; letter-spacing: -0.04em; }
.system-showcase__head p { margin: 0; color: var(--vp-c-text-2); font-size: 14px; line-height: 1.8; }
.system-showcase__body { display: grid; grid-template-columns: minmax(0, 1.65fr) minmax(260px, 0.75fr); gap: 14px; }
.showcase-screen { position: relative; display: block; min-width: 0; overflow: hidden; aspect-ratio: 16 / 10; color: white; background: #07130f; border: 1px solid var(--vp-c-divider); border-radius: 18px 5px 18px 5px; box-shadow: 0 22px 55px rgba(8, 37, 30, 0.16); }
.showcase-screen img { display: block; width: 100%; height: 100%; object-fit: cover; animation: screen-enter 0.36s ease-out both; }
.showcase-screen::after { position: absolute; inset: 42% 0 0; background: linear-gradient(transparent, rgba(1, 10, 8, 0.84)); content: ''; }
.showcase-screen__scan { position: absolute; inset: 0; z-index: 1; pointer-events: none; background: repeating-linear-gradient(to bottom, transparent 0 3px, rgba(141, 255, 231, 0.035) 4px); }
.showcase-screen__badge { position: absolute; top: 16px; left: 16px; z-index: 2; padding: 7px 9px; color: #8ff1db; background: rgba(3, 20, 16, 0.76); border: 1px solid rgba(143, 241, 219, 0.34); border-radius: 7px 2px 7px 2px; backdrop-filter: blur(8px); font: 700 10px/1 var(--vp-font-family-mono); letter-spacing: 0.11em; }
.showcase-screen__caption { position: absolute; right: 22px; bottom: 20px; left: 22px; z-index: 2; }
.showcase-screen__caption strong,
.showcase-screen__caption small { display: block; }
.showcase-screen__caption strong { font-size: clamp(18px, 2.2vw, 28px); line-height: 1.25; }
.showcase-screen__caption small { margin-top: 5px; color: rgba(230, 255, 249, 0.72); font-size: 12px; }
.showcase-index { display: grid; grid-template-rows: repeat(6, 1fr); min-width: 0; overflow: hidden; border: 1px solid var(--vp-c-divider); border-radius: 5px 18px 5px 18px; background: var(--vp-c-bg-soft); }
.showcase-index button { position: relative; display: grid; grid-template-columns: 28px minmax(0, 1fr) 3px; align-items: center; gap: 10px; min-width: 0; padding: 10px 13px; color: var(--vp-c-text-3); text-align: left; background: transparent; border: 0; border-bottom: 1px solid var(--vp-c-divider); cursor: pointer; }
.showcase-index button:last-child { border-bottom: 0; }
.showcase-index button:hover,
.showcase-index button.is-active { color: var(--vp-c-text-1); background: var(--vp-c-bg-elv); }
.showcase-index__num { font: 600 10px/1 var(--vp-font-family-mono); }
.showcase-index__copy { min-width: 0; }
.showcase-index__copy b,
.showcase-index__copy small { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.showcase-index__copy b { color: inherit; font: 700 10px/1.3 var(--vp-font-family-mono); letter-spacing: 0.08em; }
.showcase-index__copy small { margin-top: 3px; color: var(--vp-c-text-3); font-size: 11px; }
.showcase-index__tick { width: 3px; height: 18px; background: transparent; border-radius: 99px; }
.is-active .showcase-index__tick { background: var(--vp-c-brand-1); box-shadow: 0 0 12px var(--vp-c-brand-1); }
@keyframes screen-enter { from { opacity: 0.55; transform: scale(1.015); } }

@media (max-width: 820px) {
  .system-showcase { padding-block: 46px 56px; }
  .system-showcase__head { grid-template-columns: 1fr; gap: 14px; }
  .system-showcase__body { grid-template-columns: 1fr; }
  .showcase-index { display: flex; overflow-x: auto; border-radius: 8px; scrollbar-width: none; }
  .showcase-index::-webkit-scrollbar { display: none; }
  .showcase-index button { flex: 0 0 145px; grid-template-columns: 24px minmax(0, 1fr); border-right: 1px solid var(--vp-c-divider); border-bottom: 0; }
  .showcase-index__tick { position: absolute; right: 10px; bottom: 5px; left: 10px; width: auto; height: 2px; }
}
@media (max-width: 639px) {
  .system-showcase { padding: 38px 18px 46px; }
  .system-showcase__head h2 { font-size: 27px; }
  .showcase-screen { aspect-ratio: 4 / 3; border-radius: 13px 4px 13px 4px; }
  .showcase-screen__caption { right: 15px; bottom: 14px; left: 15px; }
}
@media (prefers-reduced-motion: reduce) { .showcase-screen img { animation: none; } }
</style>
