<script setup lang="ts">
/**
 * 首页底部的教程索引。
 *
 * 三十一站平铺在入口区正下方时，把实拍和路线图一起挤到了第二屏以后；
 * 挪到页尾当索引，读者想挑一站再往下翻，不想挑就直接从上面的入口走了。
 *
 * 页尾不重复放「开始读」这类按钮：每张卡本身就是链接，多一行提示只加高度。
 */
import { withBase } from 'vitepress'
import stationTable from '../../stations.json'

const stations = stationTable.stations
  .filter((s: { slug?: string }) => s.slug)
  .map((s: { id: string, slug: string, title: string, note: string, status: string }) => ({
    id: s.id,
    title: s.title,
    note: s.note,
    href: `/journey/${s.slug}/`,
    latest: s.status === 'open',
  }))
</script>

<template>
  <section class="stations" aria-labelledby="stations-title">
    <div class="stations__inner">
      <header class="stations__head">
        <div class="stations__title">
          <h2 id="stations-title">教程</h2>
          <span class="stations__count">
            已开放 {{ stations.length }} 站 · 沿真实开发史一站一站走
          </span>
        </div>
        <a class="stations__cta" :href="withBase('/journey/')">
          从第一站开始<i aria-hidden="true">→</i>
        </a>
      </header>

      <div class="stations__grid">
        <a
          v-for="station in stations"
          :key="station.id"
          class="stations__item"
          :class="{ 'is-latest': station.latest }"
          :href="withBase(station.href)"
        >
          <span class="stations__item-top">
            <strong>{{ station.title }}</strong>
            <span class="stations__id">{{ station.id }}</span>
          </span>
          <span class="stations__note">{{ station.note }}</span>
        </a>
      </div>
    </div>
  </section>
</template>

<style scoped>
.stations {
  max-width: 1152px;
  margin: 0 auto;
  padding: 0 24px 8px;
}

.stations__head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 16px;
  padding-top: 28px;
  border-top: 1px solid var(--vp-c-divider);
}
.stations__title { display: flex; align-items: baseline; gap: 11px; min-width: 0; }
.stations__title h2 {
  margin: 0;
  color: var(--vp-c-text-1);
  font-size: 16px;
  font-weight: 700;
  letter-spacing: -0.01em;
}
.stations__count {
  color: var(--vp-c-text-3);
  font: 500 11.5px/1 var(--vp-font-family-mono);
  letter-spacing: 0.04em;
}
.stations__cta {
  flex-shrink: 0;
  color: var(--vp-c-brand-1);
  font-size: 13px;
  font-weight: 650;
  text-decoration: none;
}
.stations__cta i { display: inline-block; margin-left: 5px; transition: transform 0.18s ease; }
.stations__cta:hover i { transform: translateX(3px); }

.stations__grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}
.stations__item {
  display: flex;
  flex-direction: column;
  gap: 8px;
  min-width: 0;
  padding: 13px 15px;
  color: inherit;
  background: var(--vp-c-bg-alt);
  border: 1px solid var(--vp-c-divider);
  border-radius: 10px;
  text-decoration: none;
  transition: border-color 0.18s ease, background 0.18s ease;
}
.stations__item:hover {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 50%, var(--vp-c-divider));
  background: var(--vp-c-bg-elv);
}
.stations__item.is-latest {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 35%, var(--vp-c-divider));
}
.stations__item-top {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 10px;
}
.stations__item-top strong {
  overflow: hidden;
  color: var(--vp-c-text-1);
  font-size: 14px;
  font-weight: 650;
  line-height: 1.4;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.stations__id {
  flex-shrink: 0;
  color: var(--vp-c-text-3);
  font: 700 10px/1 var(--vp-font-family-mono);
}
.stations__note {
  color: var(--vp-c-text-3);
  font-size: 12px;
  line-height: 1.6;
}

@media (max-width: 900px) {
  .stations__grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 639px) {
  .stations { padding-inline: 18px; }
  .stations__head { flex-wrap: wrap; gap: 8px; }
  .stations__grid { grid-template-columns: 1fr; }
}
</style>
