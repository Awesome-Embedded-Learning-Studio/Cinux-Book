<script setup lang="ts">
/**
 * 首页系统实拍区。
 *
 * 全页唯一的深色通栏：首页从上到下是 亮(hero) → 亮(入口) → 暗(本区) → 亮(路线)，
 * 靠明暗交替造出节奏，避免三四个浅色区块叠成一条没有起伏的长卷。
 * 截图本身是深色的内核画面，放进深底也比放在白底上更自然。
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { withBase } from 'vitepress'
import imgGui from '../../../../assets/README/gui.webp'
import imgCli from '../../../../assets/README/cli.webp'
import imgBoot from '../../../../assets/README/boot.webp'
import imgMulti from '../../../../assets/README/multi-terminal.webp'
import imgParallel from '../../../../assets/README/parallel.webp'
import imgFs from '../../../../assets/README/filesystem.webp'

const slides = [
  { img: imgBoot, vol: '01', title: '从 0x7C00 点亮第一屏', note: '实模式 → 保护模式 → 长模式', href: '/book/01-boot/' },
  { img: imgParallel, vol: '10', title: '让进程真正并发', note: '上下文切换与独立地址空间', href: '/book/10-multitasking/' },
  { img: imgFs, vol: '08', title: '把磁盘接入文件世界', note: 'VFS 抽象与 ext2 读写', href: '/book/08-filesystem/' },
  { img: imgCli, vol: '07', title: '走进用户态', note: '系统调用、ELF 加载与 Shell', href: '/book/07-userland/' },
  { img: imgGui, vol: '09', title: '长成自己的桌面', note: '帧缓冲、窗口管理与输入', href: '/book/09-gui/' },
  { img: imgMulti, vol: '10', title: '多终端同时工作', note: '会话、TTY 与 fork / exec', href: '/book/10-multitasking/' },
]

const active = ref(0)
const current = computed(() => slides[active.value])
let timer: ReturnType<typeof setInterval> | undefined

function stop() { if (timer) { clearInterval(timer); timer = undefined } }
function start() { stop(); timer = setInterval(() => { active.value = (active.value + 1) % slides.length }, 6500) }
function select(i: number) { active.value = i; start() }

onMounted(start)
onBeforeUnmount(stop)
</script>

<template>
  <section class="stage" @mouseenter="stop" @mouseleave="start">
    <div class="stage__inner">
      <header class="stage__head">
        <h2>这些画面，都来自同一套源码</h2>
        <p>不是模拟器截图，也不是概念图 —— 是教程里一步步写出来的内核在 QEMU 上跑出来的结果。</p>
      </header>

      <div class="stage__body">
        <a class="shot" :href="withBase(current.href)">
          <img :key="current.img" :src="current.img" :alt="current.title" />
          <span class="shot__meta">
            <span class="shot__vol">VOL {{ current.vol }}</span>
            <strong>{{ current.title }}</strong>
            <small>{{ current.note }}</small>
          </span>
        </a>

        <div class="picker" role="tablist" aria-label="系统画面">
          <button
            v-for="(s, i) in slides"
            :key="i"
            type="button"
            role="tab"
            :aria-selected="i === active"
            :class="{ 'is-on': i === active }"
            @click="select(i)"
          >
            <span class="picker__vol">{{ s.vol }}</span>
            <span class="picker__title">{{ s.title }}</span>
          </button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.stage {
  margin-top: 76px;
  padding: 62px 0 66px;
  background: #16120d;
  border-block: 1px solid #2b241a;
}
.dark .stage { background: #120f0b; }

.stage__inner { max-width: 1152px; margin: 0 auto; padding: 0 24px; }

.stage__head { max-width: 40em; margin-bottom: 30px; }
.stage__head h2 {
  margin: 0;
  color: #f5efe2;
  font-size: clamp(23px, 2.7vw, 34px);
  font-weight: 700;
  line-height: 1.24;
  letter-spacing: -0.035em;
}
.stage__head p {
  margin: 13px 0 0;
  color: #a89d89;
  font-size: 13.5px;
  line-height: 1.8;
}

.stage__body { display: grid; grid-template-columns: minmax(0, 1fr) 244px; gap: 14px; }

.shot {
  position: relative;
  display: block;
  min-width: 0;
  overflow: hidden;
  aspect-ratio: 16 / 9;
  background: #0d0a07;
  border: 1px solid #2f281e;
  border-radius: 12px;
  text-decoration: none;
}
.shot img {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
  animation: shot-in 0.4s ease-out both;
}
.shot::after {
  position: absolute;
  inset: 45% 0 0;
  background: linear-gradient(transparent, rgba(13, 10, 7, 0.92));
  content: '';
}
.shot__meta { position: absolute; right: 24px; bottom: 22px; left: 24px; z-index: 1; }
.shot__vol {
  display: inline-block;
  margin-bottom: 9px;
  color: #4fd9bd;
  font: 700 10.5px/1 var(--vp-font-family-mono);
  letter-spacing: 0.14em;
}
.shot__meta strong,
.shot__meta small { display: block; color: #fff; }
.shot__meta strong { font-size: clamp(17px, 2vw, 25px); font-weight: 700; line-height: 1.26; letter-spacing: -0.02em; }
.shot__meta small { margin-top: 6px; color: rgba(238, 231, 216, 0.72); font-size: 12.5px; }

.picker { display: grid; grid-template-rows: repeat(6, 1fr); gap: 4px; min-width: 0; }
.picker button {
  display: flex;
  align-items: center;
  gap: 11px;
  min-width: 0;
  padding: 9px 12px;
  color: #968b77;
  text-align: left;
  background: transparent;
  border: 1px solid transparent;
  border-radius: 8px;
  cursor: pointer;
  transition: color 0.16s ease, background 0.16s ease, border-color 0.16s ease;
}
.picker button:hover { color: #d9cfba; background: rgba(255, 255, 255, 0.035); }
.picker button.is-on {
  color: #f5efe2;
  background: rgba(79, 217, 189, 0.1);
  border-color: rgba(79, 217, 189, 0.32);
}
.picker__vol { flex-shrink: 0; font: 700 10px/1 var(--vp-font-family-mono); opacity: 0.7; }
.picker__title { overflow: hidden; font-size: 12.5px; line-height: 1.4; text-overflow: ellipsis; white-space: nowrap; }

@keyframes shot-in { from { opacity: 0.5; } }

@media (max-width: 900px) {
  .stage { margin-top: 56px; padding: 48px 0 52px; }
  .stage__body { grid-template-columns: 1fr; }
  .picker { display: flex; overflow-x: auto; scrollbar-width: none; }
  .picker::-webkit-scrollbar { display: none; }
  .picker button { flex: 0 0 auto; }
  .picker__title { max-width: 9em; }
}
@media (max-width: 639px) {
  .stage__inner { padding-inline: 18px; }
  .stage__head { margin-bottom: 22px; }
  .shot { aspect-ratio: 4 / 3; }
  .shot__meta { right: 16px; bottom: 15px; left: 16px; }
}
@media (prefers-reduced-motion: reduce) { .shot img { animation: none; } }
</style>
