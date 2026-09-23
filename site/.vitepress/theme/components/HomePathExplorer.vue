<script setup lang="ts">
import { withBase } from 'vitepress'

interface Volume {
  id: string
  title: string
  note: string
  href: string
}

interface Phase {
  id: string
  title: string
  summary: string
  accent: string
  volumes: Volume[]
}

const phases: Phase[] = [
  {
    id: '01', title: '点亮机器', summary: '从第一条指令进入 64 位内核', accent: 'boot',
    volumes: [
      { id: '01', title: '引导扇区', note: 'Real → Protected → Long', href: '/book/01-boot/' },
      { id: '02', title: '最小内核', note: '进入 kernel_main', href: '/book/02-mini-kernel/' },
      { id: '03', title: '大内核', note: '建立工程骨架', href: '/book/03-big-kernel/' },
      { id: '04', title: '开发环境', note: '工具链与调试闭环', href: '/book/04-developer/' },
    ],
  },
  {
    id: '02', title: '让内核运转', summary: '管理内存、任务、用户态与文件', accent: 'kernel',
    volumes: [
      { id: '05', title: '内存管理', note: 'PMM · VMM · Heap', href: '/book/05-memory/' },
      { id: '06', title: '进程管理', note: '上下文与调度', href: '/book/06-process/' },
      { id: '07', title: '用户态', note: 'Ring 3 · syscall · shell', href: '/book/07-userland/' },
      { id: '08', title: '文件系统', note: 'VFS · Ext2 · 路径解析', href: '/book/08-filesystem/' },
    ],
  },
  {
    id: '03', title: '长成完整系统', summary: '从可运行到可交互、可存储', accent: 'system',
    volumes: [
      { id: '09', title: '图形界面', note: '窗口、字体与输入', href: '/book/09-gui/' },
      { id: '10', title: '多任务', note: 'fork · exec · 多终端', href: '/book/10-multitasking/' },
      { id: '11', title: '内核基础', note: '通用机制回收整理', href: '/book/11-foundation/' },
      { id: '12', title: '存储设备', note: 'PCI · AHCI · 块设备', href: '/book/12-storage/' },
    ],
  },
  {
    id: '04', title: '进入深水区', summary: '并发、安全与网络把系统推向真实世界', accent: 'advanced',
    volumes: [
      { id: '13', title: '内存进阶', note: '缓存与高级分配', href: '/book/13-memory-advanced/' },
      { id: '14', title: '进程进阶', note: '时钟、队列与内核线程', href: '/book/14-process-advanced/' },
      { id: '15', title: '多核 SMP', note: 'AP · IPI · 并发', href: '/book/15-smp/' },
      { id: '16', title: '系统安全', note: '隔离与权限边界', href: '/book/16-security/' },
      { id: '17', title: '网络栈', note: 'Ethernet → Socket', href: '/book/17-net/' },
    ],
  },
]
</script>

<template>
  <section id="roadmap" class="kernel-roadmap">
    <header class="kernel-roadmap__head">
      <div>
        <span class="section-kicker">CURRICULUM / 17 VOLUMES</span>
        <h2>不是知识点清单，而是一台机器的生长顺序</h2>
      </div>
      <p>每一卷都对应可运行的源码阶段。沿主线读原理，切到实验册亲手验证，再从真实排错记录理解边界。</p>
    </header>

    <div class="phase-list">
      <article v-for="phase in phases" :key="phase.id" class="phase" :class="`phase--${phase.accent}`">
        <header class="phase__head">
          <span class="phase__id">PHASE {{ phase.id }}</span>
          <h3>{{ phase.title }}</h3>
          <p>{{ phase.summary }}</p>
        </header>
        <div class="phase__volumes">
          <a v-for="volume in phase.volumes" :key="volume.id" :href="withBase(volume.href)" class="volume-node">
            <span class="volume-node__id">{{ volume.id }}</span>
            <span class="volume-node__copy">
              <strong>{{ volume.title }}</strong>
              <small>{{ volume.note }}</small>
            </span>
            <span class="volume-node__arrow" aria-hidden="true">↗</span>
          </a>
        </div>
      </article>
    </div>

    <footer class="kernel-roadmap__footer">
      <a :href="withBase('/primer/')"><b>需要补基础？</b><span>先读前置卷</span></a>
      <a :href="withBase('/labs/')"><b>想立刻动手？</b><span>进入实验册</span></a>
      <a :href="withBase('/tags')"><b>按问题查找？</b><span>打开主题索引</span></a>
    </footer>
  </section>
</template>

<style scoped>
.kernel-roadmap { max-width: 1152px; margin: 0 auto; padding: 78px 24px 72px; }
.kernel-roadmap__head {
  display: grid;
  grid-template-columns: minmax(0, 1.2fr) minmax(300px, 0.8fr);
  gap: 48px;
  align-items: end;
  margin-bottom: 34px;
}
.section-kicker { color: var(--vp-c-brand-1); font: 700 11px/1 var(--vp-font-family-mono); letter-spacing: 0.14em; }
.kernel-roadmap__head h2 { max-width: 660px; margin: 12px 0 0; color: var(--vp-c-text-1); font-size: clamp(25px, 3.2vw, 40px); line-height: 1.18; letter-spacing: -0.04em; }
.kernel-roadmap__head > p { margin: 0; color: var(--vp-c-text-2); font-size: 14px; line-height: 1.85; }
.phase-list { border-top: 1px solid var(--vp-c-divider); }
.phase {
  --phase-color: var(--vp-c-brand-1);
  display: grid;
  grid-template-columns: 220px minmax(0, 1fr);
  gap: 28px;
  padding: 30px 0;
  border-bottom: 1px solid var(--vp-c-divider);
}
.phase--kernel { --phase-color: #168fa0; }
.phase--system { --phase-color: #b56b20; }
.phase--advanced { --phase-color: #7659af; }
.phase__head { padding: 5px 0 0 17px; border-left: 3px solid var(--phase-color); }
.phase__id { color: var(--phase-color); font: 700 10px/1 var(--vp-font-family-mono); letter-spacing: 0.13em; }
.phase__head h3 { margin: 8px 0 5px; color: var(--vp-c-text-1); font-size: 19px; line-height: 1.3; }
.phase__head p { margin: 0; color: var(--vp-c-text-3); font-size: 12px; line-height: 1.55; }
.phase__volumes { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
.volume-node {
  display: grid;
  grid-template-columns: 34px minmax(0, 1fr) auto;
  align-items: center;
  gap: 11px;
  min-width: 0;
  padding: 13px 14px;
  color: inherit;
  background: color-mix(in srgb, var(--vp-c-bg-soft) 72%, transparent);
  border: 1px solid transparent;
  border-radius: 10px 3px 10px 3px;
  text-decoration: none;
  transition: border-color 0.18s ease, background 0.18s ease, transform 0.18s ease;
}
.volume-node:hover { background: var(--vp-c-bg-elv); border-color: color-mix(in srgb, var(--phase-color) 48%, var(--vp-c-divider)); transform: translateX(3px); }
.volume-node__id { display: grid; width: 31px; height: 31px; place-items: center; color: var(--phase-color); border: 1px solid color-mix(in srgb, var(--phase-color) 38%, transparent); border-radius: 50%; font: 700 10px/1 var(--vp-font-family-mono); }
.volume-node__copy { min-width: 0; }
.volume-node__copy strong,
.volume-node__copy small { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.volume-node__copy strong { color: var(--vp-c-text-1); font-size: 13px; }
.volume-node__copy small { margin-top: 3px; color: var(--vp-c-text-3); font-size: 11px; }
.volume-node__arrow { color: var(--vp-c-text-3); font-size: 13px; transition: color 0.18s ease; }
.volume-node:hover .volume-node__arrow { color: var(--phase-color); }
.kernel-roadmap__footer { display: grid; grid-template-columns: repeat(3, 1fr); gap: 1px; margin-top: 28px; overflow: hidden; background: var(--vp-c-divider); border: 1px solid var(--vp-c-divider); border-radius: 13px 4px 13px 4px; }
.kernel-roadmap__footer a { display: flex; justify-content: space-between; gap: 12px; padding: 16px 18px; color: inherit; background: var(--vp-c-bg); text-decoration: none; }
.kernel-roadmap__footer a:hover { background: var(--vp-c-brand-soft); }
.kernel-roadmap__footer b { color: var(--vp-c-text-1); font-size: 12px; }
.kernel-roadmap__footer span { color: var(--vp-c-brand-1); font-size: 12px; }

@media (max-width: 820px) {
  .kernel-roadmap { padding-block: 56px; }
  .kernel-roadmap__head { grid-template-columns: 1fr; gap: 16px; }
  .phase { grid-template-columns: 1fr; gap: 18px; }
  .phase__head { display: grid; grid-template-columns: auto 1fr; align-items: center; gap: 8px 14px; }
  .phase__head h3 { margin: 0; }
  .phase__head p { grid-column: 1 / -1; }
}
@media (max-width: 639px) {
  .kernel-roadmap { padding: 46px 18px 52px; }
  .kernel-roadmap__head h2 { font-size: 27px; }
  .phase { padding: 24px 0; }
  .phase__volumes { grid-template-columns: 1fr; }
  .kernel-roadmap__footer { grid-template-columns: 1fr; }
}
</style>
