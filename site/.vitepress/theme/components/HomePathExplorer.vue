<script setup lang="ts">
/**
 * 首页 17 卷路线区（页尾）。
 *
 * 定位是「一张可读的目录」而非又一个营销区块，所以：
 * - 只留一行朴素抬头，不再用 kicker + 大标题的组合（前面区块已用过）；
 * - 阶段用行式布局而不是卡片网格 —— 读者在这里的动作是「找到一卷点进去」，
 *   密集的行比带边框的卡片更快扫读；
 * - 不使用切角、投影与等宽装饰，把签名视觉留给首屏的启动面板。
 */
import { withBase } from 'vitepress'

interface Volume { id: string; title: string; note: string; href: string }
interface Phase { id: string; title: string; summary: string; tone: string; volumes: Volume[] }

const phases: Phase[] = [
  {
    id: 'I', title: '点亮机器', summary: '从第一条指令到能跑 C++ 的内核', tone: 'boot',
    volumes: [
      { id: '01', title: '引导扇区', note: '实模式 → 保护模式 → 长模式', href: '/book/01-boot/' },
      { id: '02', title: '最小内核', note: '入口、PMM、中断', href: '/book/02-mini-kernel/' },
      { id: '03', title: '大内核', note: 'GDT/IDT、PIC/PIT、帧缓冲', href: '/book/03-big-kernel/' },
      { id: '04', title: '开发环境', note: 'kallsyms、验证基建', href: '/book/04-developer/' },
    ],
  },
  {
    id: 'II', title: '让内核运转', summary: '内存、进程、用户态与文件系统', tone: 'core',
    volumes: [
      { id: '05', title: '内存管理', note: 'PMM、VMM、内核堆', href: '/book/05-memory/' },
      { id: '06', title: '进程管理', note: '上下文切换、抢占调度', href: '/book/06-process/' },
      { id: '07', title: '用户态', note: 'ring3、syscall、shell', href: '/book/07-userland/' },
      { id: '08', title: '文件系统', note: 'AHCI、VFS、ext2', href: '/book/08-filesystem/' },
    ],
  },
  {
    id: 'III', title: '长成完整系统', summary: '能看、能交互、能存储', tone: 'system',
    volumes: [
      { id: '09', title: '图形界面', note: '画布、窗口管理器、桌面', href: '/book/09-gui/' },
      { id: '10', title: '多任务', note: 'fork/exec、多终端', href: '/book/10-multitasking/' },
      { id: '11', title: '基础设施', note: 'RingBuffer、lockdep', href: '/book/11-foundation/' },
      { id: '12', title: '存储设备', note: 'AHCI DMA、NVMe/VirtIO', href: '/book/12-storage/' },
    ],
  },
  {
    id: 'IV', title: '进入深水区', summary: '并发、安全与网络', tone: 'deep',
    volumes: [
      { id: '13', title: '内存增强', note: 'mmap、PageCache、buddy/slab', href: '/book/13-memory-advanced/' },
      { id: '14', title: '进程增强', note: '信号、futex、调度类', href: '/book/14-process-advanced/' },
      { id: '15', title: '多核 SMP', note: 'APIC、per-CPU、多核调度', href: '/book/15-smp/' },
      { id: '16', title: '系统安全', note: 'NX/SMEP/SMAP、ASLR', href: '/book/16-security/' },
      { id: '17', title: '网络栈', note: 'e1000、IPv4、UDP/TCP', href: '/book/17-net/' },
    ],
  },
]
</script>

<template>
  <section id="roadmap" class="route">
    <header class="route__head">
      <h2>十七卷，一条路线</h2>
      <p>按顺序读完是一台完整的机器；也可以从任意一卷切入，每卷自成体系。</p>
    </header>

    <div class="route__phases">
      <article v-for="phase in phases" :key="phase.id" class="phase" :class="`phase--${phase.tone}`">
        <div class="phase__label">
          <span class="phase__num">{{ phase.id }}</span>
          <h3>{{ phase.title }}</h3>
          <p>{{ phase.summary }}</p>
        </div>
        <ul class="phase__vols">
          <li v-for="vol in phase.volumes" :key="vol.id">
            <a :href="withBase(vol.href)">
              <span class="vol__id">{{ vol.id }}</span>
              <span class="vol__title">{{ vol.title }}</span>
              <span class="vol__note">{{ vol.note }}</span>
            </a>
          </li>
        </ul>
      </article>
    </div>

    <footer class="route__foot">
      <a :href="withBase('/primer/')">需要补基础 · 前置卷</a>
      <a :href="withBase('/tags')">按主题检索 · 标签索引</a>
      <a :href="withBase('/notes/')">想看原始记录 · 开发笔记</a>
    </footer>
  </section>
</template>

<style scoped>
.route { max-width: 1152px; margin: 0 auto; padding: 72px 24px 20px; }

.route__head { margin-bottom: 8px; }
.route__head h2 {
  margin: 0;
  color: var(--vp-c-text-1);
  font-size: clamp(22px, 2.5vw, 31px);
  font-weight: 700;
  line-height: 1.26;
  letter-spacing: -0.035em;
}
.route__head p { max-width: 42em; margin: 11px 0 0; color: var(--vp-c-text-2); font-size: 13.5px; line-height: 1.8; }

.phase {
  --tone: var(--vp-c-brand-1);
  display: grid;
  grid-template-columns: 230px minmax(0, 1fr);
  gap: 26px;
  padding: 26px 0;
  border-bottom: 1px solid var(--vp-c-divider);
}
.phase:first-child { border-top: 1px solid var(--vp-c-divider); margin-top: 26px; }
.phase--core { --tone: #1490a1; }
.phase--system { --tone: #b06a1f; }
.phase--deep { --tone: #7659af; }

.phase__label { min-width: 0; }
.phase__num {
  color: var(--tone);
  font: 700 11px/1 var(--vp-font-family-mono);
  letter-spacing: 0.1em;
}
.phase__label h3 { margin: 9px 0 4px; color: var(--vp-c-text-1); font-size: 17px; font-weight: 650; line-height: 1.35; }
.phase__label p { margin: 0; color: var(--vp-c-text-3); font-size: 12.5px; line-height: 1.6; }

.phase__vols { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 2px 18px; margin: 0; padding: 0; list-style: none; }
.phase__vols a {
  display: grid;
  grid-template-columns: 26px minmax(0, auto) minmax(0, 1fr);
  align-items: baseline;
  gap: 10px;
  min-width: 0;
  padding: 7px 9px 7px 0;
  color: inherit;
  border-radius: 6px;
  text-decoration: none;
  transition: background 0.15s ease;
}
.phase__vols a:hover { background: var(--vp-c-bg-alt); }
.vol__id { color: var(--tone); font: 600 11px/1.4 var(--vp-font-family-mono); }
.vol__title { color: var(--vp-c-text-1); font-size: 13.5px; font-weight: 600; white-space: nowrap; }
.vol__note { overflow: hidden; color: var(--vp-c-text-3); font-size: 11.5px; text-overflow: ellipsis; white-space: nowrap; }
.phase__vols a:hover .vol__title { color: var(--tone); }

.route__foot { display: flex; flex-wrap: wrap; gap: 8px 26px; padding-top: 26px; }
.route__foot a {
  color: var(--vp-c-text-2);
  font-size: 12.5px;
  text-decoration: none;
  border-bottom: 1px solid transparent;
  transition: color 0.15s ease, border-color 0.15s ease;
}
.route__foot a:hover { color: var(--vp-c-brand-1); border-bottom-color: var(--vp-c-brand-1); }

@media (max-width: 860px) {
  .route { padding-top: 54px; }
  .phase { grid-template-columns: 1fr; gap: 14px; }
  .phase__label { display: grid; grid-template-columns: auto minmax(0, 1fr); align-items: center; gap: 4px 12px; }
  .phase__label h3 { margin: 0; }
  .phase__label p { grid-column: 1 / -1; }
}
@media (max-width: 639px) {
  .route { padding: 42px 18px 16px; }
  .phase__vols { grid-template-columns: 1fr; }
  .vol__note { display: none; }
  .phase__vols a { grid-template-columns: 26px minmax(0, 1fr); padding: 8px 0; }
}
</style>
