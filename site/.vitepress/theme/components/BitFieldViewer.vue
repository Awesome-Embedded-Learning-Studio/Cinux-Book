<script setup lang="ts">
import { computed, ref } from 'vue'

interface BitFieldDef {
  bits: number | [number, number]
  name: string
  desc: string
}

interface BitFieldVariant {
  label: string
  value: number
}

const props = withDefaults(defineProps<{
  caption?: string
  fields: BitFieldDef[]
  variants: BitFieldVariant[]
  width?: number
}>(), {
  caption: undefined,
  width: 8,
})

const activeVariant = ref(0)
const activeField = ref<number | null>(null)

function normalizeBits(bits: number | [number, number]): [number, number] {
  return Array.isArray(bits) ? bits : [bits, bits]
}

const laidOut = computed(() => props.fields
  .map((field, index) => ({ field, index, range: normalizeBits(field.bits) }))
  .sort((a, b) => b.range[0] - a.range[0]))

const currentValue = computed(() => props.variants[activeVariant.value]?.value ?? 0)

function bitsText(high: number, low: number): string {
  let text = ''
  for (let bit = high; bit >= low; bit--)
    text += ((currentValue.value >>> bit) & 1).toString()
  return text
}

function toggleField(index: number) {
  activeField.value = activeField.value === index ? null : index
}
</script>

<template>
  <figure class="bitfield">
    <figcaption v-if="caption" class="bitfield__caption">{{ caption }}</figcaption>

    <div v-if="variants.length > 1" class="bitfield__variants" role="tablist">
      <button
        v-for="(variant, index) in variants"
        :key="index"
        type="button"
        class="bitfield__variant"
        :class="{ 'is-active': activeVariant === index }"
        :aria-pressed="activeVariant === index"
        @click="activeVariant = index"
      >
        {{ variant.label }}
      </button>
    </div>

    <div class="bitfield__strip" :aria-label="`bit ${width - 1} to bit 0`">
      <template v-for="entry in laidOut" :key="entry.index">
        <button
          v-if="entry.range[0] === entry.range[1]"
          type="button"
          class="bitfield__cell"
          :class="{ 'is-active': activeField === entry.index }"
          :aria-label="`${entry.field.name}: ${entry.field.desc}`"
          @click="toggleField(entry.index)"
          @pointerenter="activeField = entry.index"
        >
          <span class="bitfield__cell-bits">{{ bitsText(entry.range[0], entry.range[1]) }}</span>
          <span class="bitfield__cell-name">{{ entry.field.name }}</span>
          <span class="bitfield__cell-index">{{ entry.range[0] }}</span>
        </button>
        <button
          v-else
          type="button"
          class="bitfield__group"
          :class="{ 'is-active': activeField === entry.index }"
          :aria-label="`${entry.field.name} [${entry.range[0]}:${entry.range[1]}]: ${entry.field.desc}`"
          @click="toggleField(entry.index)"
          @pointerenter="activeField = entry.index"
        >
          <span class="bitfield__group-label">{{ entry.field.name }} <i>[{{ entry.range[0] }}:{{ entry.range[1] }}]</i></span>
          <span class="bitfield__group-cells">
            <span
              v-for="bit in entry.range[0] - entry.range[1] + 1"
              :key="bit"
              class="bitfield__group-cell"
            >{{ bitsText(entry.range[1] + bit - 1, entry.range[1] + bit - 1) }}</span>
          </span>
        </button>
      </template>
    </div>

    <ul class="bitfield__legend">
      <li
        v-for="entry in laidOut"
        :key="entry.index"
        class="bitfield__legend-item"
        :class="{ 'is-active': activeField === entry.index }"
        @pointerenter="activeField = entry.index"
        @pointerleave="activeField = null"
      >
        <button type="button" class="bitfield__legend-head" @click="toggleField(entry.index)">
          <code class="bitfield__legend-name">{{ entry.field.name }}</code>
          <span class="bitfield__legend-range">bit {{ entry.range[0] === entry.range[1] ? entry.range[0] : `${entry.range[0]}–${entry.range[1]}` }}</span>
          <code class="bitfield__legend-value">{{ bitsText(entry.range[0], entry.range[1]) }}</code>
        </button>
        <p class="bitfield__legend-desc">{{ entry.field.desc }}</p>
      </li>
    </ul>
  </figure>
</template>

<style scoped>
.bitfield {
  margin: 20px 0 26px;
  padding: 16px 18px 14px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 12px;
  background: var(--vp-c-bg-soft);
}

.bitfield__caption {
  margin-bottom: 12px;
  color: var(--vp-c-text-2);
  font-size: 13px;
  font-weight: 600;
  text-align: center;
}

.bitfield__variants {
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
  gap: 8px;
  margin-bottom: 14px;
}

.bitfield__variant {
  padding: 5px 14px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 999px;
  background: transparent;
  color: var(--vp-c-text-2);
  font-family: var(--vp-font-family-mono);
  font-size: 12px;
  cursor: pointer;
  transition: border-color .18s ease, background-color .18s ease, color .18s ease;
}

.bitfield__variant:hover {
  border-color: color-mix(in srgb, var(--vp-c-brand-1) 40%, var(--vp-c-divider));
}

.bitfield__variant.is-active {
  border-color: var(--vp-c-brand-1);
  background: var(--vp-c-brand-soft);
  color: var(--vp-c-brand-1);
  font-weight: 650;
}

.bitfield__strip {
  display: flex;
  justify-content: center;
  align-items: stretch;
  gap: 3px;
}

.bitfield__cell,
.bitfield__group {
  display: grid;
  min-width: 44px;
  padding: 0;
  border: 1px solid var(--vp-c-divider);
  border-radius: 8px;
  background: var(--vp-c-bg);
  cursor: pointer;
  transition: border-color .18s ease, background-color .18s ease;
}

.bitfield__cell {
  grid-template-rows: auto auto auto;
  justify-items: center;
  gap: 1px;
  padding: 7px 6px 5px;
}

.bitfield__cell-bits {
  color: var(--vp-c-text-1);
  font-family: var(--vp-font-family-mono);
  font-size: 15px;
  font-weight: 700;
  line-height: 1.1;
}

.bitfield__cell-name {
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 10.5px;
  font-weight: 650;
}

.bitfield__cell-index {
  color: var(--vp-c-text-3);
  font-family: var(--vp-font-family-mono);
  font-size: 9px;
}

.bitfield__group {
  grid-template-rows: auto auto;
  gap: 4px;
  padding: 7px 6px;
  place-items: center;
}

.bitfield__group-label {
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 10.5px;
  font-weight: 650;
  text-align: center;
}

.bitfield__group-label i {
  color: var(--vp-c-text-3);
  font-size: 9px;
  font-style: normal;
}

.bitfield__group-cells {
  display: flex;
  gap: 2px;
}

.bitfield__group-cell {
  display: grid;
  min-width: 20px;
  padding: 3px 2px;
  place-items: center;
  border-radius: 4px;
  background: color-mix(in srgb, var(--vp-c-default-soft) 60%, transparent);
  color: var(--vp-c-text-1);
  font-family: var(--vp-font-family-mono);
  font-size: 13px;
  font-weight: 700;
}

.bitfield__cell:hover,
.bitfield__group:hover,
.bitfield__cell.is-active,
.bitfield__group.is-active {
  border-color: var(--vp-c-brand-1);
  background: var(--vp-c-brand-soft);
}

.bitfield__cell.is-active .bitfield__cell-bits,
.bitfield__group.is-active .bitfield__group-cell {
  color: var(--vp-c-brand-1);
}

.bitfield__legend {
  margin: 14px 0 0;
  padding: 0;
  list-style: none;
  display: grid;
  gap: 6px;
}

.bitfield__legend-item {
  padding: 6px 10px;
  border-radius: 8px;
  transition: background-color .18s ease;
}

.bitfield__legend-item.is-active {
  background: var(--vp-c-brand-soft);
}

.bitfield__legend-head {
  display: flex;
  align-items: baseline;
  gap: 10px;
  width: 100%;
  padding: 0;
  border: 0;
  background: transparent;
  cursor: pointer;
  text-align: left;
}

.bitfield__legend-name {
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 12px;
  font-weight: 700;
}

.bitfield__legend-range {
  flex: 1;
  color: var(--vp-c-text-3);
  font-family: var(--vp-font-family-mono);
  font-size: 11px;
}

.bitfield__legend-value {
  padding: 1px 7px;
  border-radius: 5px;
  background: var(--vp-c-default-soft);
  color: var(--vp-c-text-1);
  font-family: var(--vp-font-family-mono);
  font-size: 12px;
  font-weight: 700;
  letter-spacing: .12em;
}

.bitfield__legend-desc {
  margin: 3px 0 0;
  color: var(--vp-c-text-2);
  font-size: 13px;
  line-height: 1.65;
}

@media (max-width: 639px) {
  .bitfield {
    padding: 12px 10px 10px;
  }

  .bitfield__strip {
    overflow-x: auto;
    justify-content: flex-start;
  }

  .bitfield__legend-head {
    flex-wrap: wrap;
    gap: 6px;
  }

  .bitfield__legend-range {
    flex-basis: 100%;
    order: 3;
  }
}
</style>
