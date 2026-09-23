<script setup lang="ts">
const facts = [
  { value: '17', label: '卷主线教程', note: '从引导扇区到网络栈' },
  { value: '700+', label: '篇技术文档', note: '正文、实验与排错记录' },
  { value: '42', label: '源码里程碑', note: '章节与 Git tag 对齐' },
  { value: 'x86_64', label: '真实系统栈', note: 'C++17 · QEMU · Ring 3' },
]
</script>

<template>
  <section class="system-facts" aria-label="教程规模">
    <dl class="system-facts__inner">
      <div v-for="fact in facts" :key="fact.label" class="system-fact">
        <dt>{{ fact.value }}</dt>
        <dd>
          <strong>{{ fact.label }}</strong>
          <span>{{ fact.note }}</span>
        </dd>
      </div>
    </dl>
  </section>
</template>

<style scoped>
.system-facts {
  max-width: 1152px;
  margin: 0 auto;
  padding: 0 24px 58px;
}
.system-facts__inner {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  margin: 0;
  padding: 0;
  overflow: hidden;
  border-block: 1px solid var(--vp-c-divider);
}
.system-fact {
  position: relative;
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  align-items: center;
  gap: 13px;
  min-width: 0;
  padding: 20px 18px;
}
.system-fact:not(:last-child)::after {
  position: absolute;
  top: 18px;
  right: 0;
  bottom: 18px;
  width: 1px;
  background: var(--vp-c-divider);
  content: '';
}
.system-fact dt {
  color: var(--vp-c-brand-1);
  font: 750 24px/1 var(--vp-font-family-mono);
  letter-spacing: -0.06em;
}
.system-fact dd { min-width: 0; margin: 0; }
.system-fact strong,
.system-fact span { display: block; }
.system-fact strong { color: var(--vp-c-text-1); font-size: 13px; line-height: 1.35; }
.system-fact span { margin-top: 3px; overflow: hidden; color: var(--vp-c-text-3); font-size: 11px; line-height: 1.35; text-overflow: ellipsis; white-space: nowrap; }

@media (max-width: 820px) {
  .system-facts { padding-bottom: 40px; }
  .system-facts__inner { grid-template-columns: repeat(2, 1fr); }
  .system-fact:nth-child(2)::after { display: none; }
  .system-fact:nth-child(-n + 2) { border-bottom: 1px solid var(--vp-c-divider); }
}
@media (max-width: 479px) {
  .system-facts { padding-inline: 18px; }
  .system-fact { gap: 10px; padding: 16px 10px; }
  .system-fact dt { font-size: 20px; }
  .system-fact span { display: none; }
}
</style>
