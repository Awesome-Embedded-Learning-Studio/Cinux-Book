<script setup lang="ts">
/**
 * Hero 下方的规格行。
 *
 * 刻意不做成独立区块：它是 hero 的补充说明，不是一个需要标题和边框的
 * 内容区。做成卡片网格会在首屏正下方再立一道横墙，把页面切得更碎。
 */
const specs = [
  { value: '17', unit: '卷', label: '主线教程' },
  { value: '700', unit: '+', label: '篇文档' },
  { value: '42', unit: '', label: '源码里程碑' },
  { value: 'C++17', unit: '', label: 'x86_64 · QEMU' },
]
</script>

<template>
  <div class="hero-specs">
    <div class="hero-specs__inner">
      <span v-for="spec in specs" :key="spec.label" class="spec">
        <b>{{ spec.value }}<i v-if="spec.unit">{{ spec.unit }}</i></b>
        <em>{{ spec.label }}</em>
      </span>
    </div>
  </div>
</template>

<style scoped>
.hero-specs {
  max-width: 1152px;
  margin: 0 auto;
  padding: 0 24px;
}
.hero-specs__inner {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: 10px 34px;
  padding-top: 26px;
  border-top: 1px solid var(--vp-c-divider);
}
.spec {
  display: inline-flex;
  align-items: baseline;
  gap: 7px;
  min-width: 0;
}
.spec b {
  color: var(--vp-c-text-1);
  font: 700 17px/1 var(--vp-font-family-mono);
  letter-spacing: -0.03em;
}
.spec i {
  margin-left: 1px;
  color: var(--vp-c-brand-1);
  font-style: normal;
  font-size: 0.8em;
}
.spec em {
  color: var(--vp-c-text-3);
  font-size: 12.5px;
  font-style: normal;
  white-space: nowrap;
}

@media (max-width: 639px) {
  .hero-specs { padding-inline: 18px; }
  .hero-specs__inner { gap: 9px 20px; padding-top: 20px; }
  .spec b { font-size: 15px; }
  .spec em { font-size: 11.5px; }
}
</style>
