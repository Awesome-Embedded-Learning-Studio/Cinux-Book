<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { useData } from 'vitepress'

const props = withDefaults(defineProps<{
  tag: string
  from?: string
  build?: string
}>(), {
  from: undefined,
  build: undefined,
})

type CommandKind = 'checkout' | 'build'

const { lang } = useData()
const isEnglish = computed(() => lang.value.startsWith('en'))
const checkoutCommand = computed(() => `git checkout ${props.tag}`)
const stationNumber = computed(() => props.tag.match(/\d+/)?.[0].padStart(2, '0') ?? '·')
const copied = ref<CommandKind | null>(null)
let copiedTimer: ReturnType<typeof setTimeout> | undefined

async function copyCommand(kind: CommandKind, command: string) {
  if (typeof navigator === 'undefined' || !navigator.clipboard)
    return

  await navigator.clipboard.writeText(command)
  copied.value = kind
  clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => {
    copied.value = null
  }, 1600)
}

onBeforeUnmount(() => clearTimeout(copiedTimer))
</script>

<template>
  <aside class="station-ticket" :aria-label="isEnglish ? 'Code snapshot' : '代码快照'">
    <div class="station-ticket__rail" aria-hidden="true">
      <span>{{ stationNumber }}</span>
      <i />
    </div>

    <div class="station-ticket__body">
      <header class="station-ticket__header">
        <div>
          <span class="station-ticket__kicker">CINUX / CODE SNAPSHOT</span>
          <strong>{{ isEnglish ? 'Coordinates for this station' : '这一站的代码坐标' }}</strong>
        </div>

        <div class="station-ticket__identity">
          <span class="station-ticket__ready"><i />{{ isEnglish ? 'READY' : '已就绪' }}</span>
          <code v-if="from" class="station-ticket__old-tag">{{ from }}</code>
          <span v-if="from" class="station-ticket__arrow" aria-hidden="true">→</span>
          <code class="station-ticket__tag">{{ tag }}</code>
        </div>
      </header>

      <div class="station-ticket__commands">
        <div class="station-ticket__command">
          <span class="station-ticket__prompt" aria-hidden="true">$</span>
          <div class="station-ticket__command-text">
            <span>CHECKOUT</span>
            <code>{{ checkoutCommand }}</code>
          </div>
          <button
            class="station-ticket__copy"
            type="button"
            :class="{ 'is-copied': copied === 'checkout' }"
            :aria-label="isEnglish ? 'Copy checkout command' : '复制检出命令'"
            @click="copyCommand('checkout', checkoutCommand)"
          >
            <svg v-if="copied !== 'checkout'" viewBox="0 0 16 16" aria-hidden="true">
              <rect x="5.5" y="5.5" width="7" height="7" rx="1.5" />
              <path d="M10.5 5.5V4A1.5 1.5 0 0 0 9 2.5H4A1.5 1.5 0 0 0 2.5 4v5A1.5 1.5 0 0 0 4 10.5h1.5" />
            </svg>
            <svg v-else viewBox="0 0 16 16" aria-hidden="true"><path d="m3.5 8 3 3 6-6" /></svg>
            <span>{{ copied === 'checkout' ? (isEnglish ? 'Copied' : '已复制') : (isEnglish ? 'Copy' : '复制') }}</span>
          </button>
        </div>

        <div v-if="build" class="station-ticket__command">
          <span class="station-ticket__prompt" aria-hidden="true">›</span>
          <div class="station-ticket__command-text">
            <span>VERIFY</span>
            <code>{{ build }}</code>
          </div>
          <button
            class="station-ticket__copy"
            type="button"
            :class="{ 'is-copied': copied === 'build' }"
            :aria-label="isEnglish ? 'Copy verification command' : '复制验证命令'"
            @click="copyCommand('build', build)"
          >
            <svg v-if="copied !== 'build'" viewBox="0 0 16 16" aria-hidden="true">
              <rect x="5.5" y="5.5" width="7" height="7" rx="1.5" />
              <path d="M10.5 5.5V4A1.5 1.5 0 0 0 9 2.5H4A1.5 1.5 0 0 0 2.5 4v5A1.5 1.5 0 0 0 4 10.5h1.5" />
            </svg>
            <svg v-else viewBox="0 0 16 16" aria-hidden="true"><path d="m3.5 8 3 3 6-6" /></svg>
            <span>{{ copied === 'build' ? (isEnglish ? 'Copied' : '已复制') : (isEnglish ? 'Copy' : '复制') }}</span>
          </button>
        </div>
      </div>
    </div>
  </aside>
</template>

<style scoped>
.station-ticket {
  position: relative;
  display: grid;
  grid-template-columns: 72px minmax(0, 1fr);
  margin: 22px 0 28px;
  overflow: hidden;
  border: 1px solid color-mix(in srgb, var(--vp-c-brand-1) 22%, var(--vp-c-divider));
  border-radius: 16px;
  background: var(--vp-c-bg-soft);
  box-shadow: 0 12px 30px rgb(38 34 25 / 4%);
}

.station-ticket__rail {
  position: relative;
  display: flex;
  justify-content: center;
  padding-top: 19px;
  background: color-mix(in srgb, var(--vp-c-brand-soft) 54%, transparent);
}

.station-ticket__rail span {
  position: relative;
  z-index: 1;
  display: grid;
  width: 38px;
  height: 38px;
  place-items: center;
  border: 1px solid color-mix(in srgb, var(--vp-c-brand-1) 34%, transparent);
  border-radius: 50%;
  background: var(--vp-c-brand-1);
  box-shadow: 0 0 0 7px color-mix(in srgb, var(--vp-c-brand-1) 9%, transparent);
  color: var(--vp-c-bg);
  font-family: var(--vp-font-family-mono);
  font-size: 11px;
  font-weight: 700;
}

.station-ticket__rail i {
  position: absolute;
  top: 59px;
  bottom: 18px;
  left: 50%;
  border-left: 1px dashed color-mix(in srgb, var(--vp-c-brand-1) 34%, transparent);
}

.station-ticket__body {
  min-width: 0;
  padding: 18px 20px 19px 4px;
}

.station-ticket__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 20px;
  margin-bottom: 15px;
}

.station-ticket__header > div:first-child {
  display: grid;
  gap: 3px;
}

.station-ticket__kicker {
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 9px;
  font-weight: 700;
  letter-spacing: .14em;
}

.station-ticket__header strong {
  color: var(--vp-c-text-1);
  font-size: 15px;
  line-height: 1.4;
}

.station-ticket__identity,
.station-ticket__ready,
.station-ticket__copy {
  display: flex;
  align-items: center;
}

.station-ticket__identity {
  gap: 7px;
  min-width: 0;
}

.station-ticket__ready {
  gap: 5px;
  margin-right: 3px;
  color: var(--vp-c-text-3);
  font-family: var(--vp-font-family-mono);
  font-size: 9px;
  font-weight: 700;
  letter-spacing: .1em;
}

.station-ticket__ready i {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--vp-c-brand-1);
  box-shadow: 0 0 0 4px color-mix(in srgb, var(--vp-c-brand-1) 10%, transparent);
}

.station-ticket__tag,
.station-ticket__old-tag {
  padding: 3px 8px;
  border-radius: 6px;
  background: var(--vp-c-brand-soft);
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 11px;
  font-weight: 650;
}

.station-ticket__old-tag {
  background: var(--vp-c-default-soft);
  color: var(--vp-c-text-3);
}

.station-ticket__arrow {
  color: var(--vp-c-text-3);
  font-size: 10px;
}

.station-ticket__commands {
  overflow: hidden;
  border: 1px solid var(--vp-c-divider);
  border-radius: 10px;
  background: color-mix(in srgb, var(--vp-c-bg) 72%, transparent);
}

.station-ticket__command {
  display: grid;
  grid-template-columns: 18px minmax(0, 1fr) auto;
  align-items: center;
  gap: 8px;
  min-width: 0;
  padding: 9px 10px 9px 12px;
}

.station-ticket__command + .station-ticket__command {
  border-top: 1px solid var(--vp-c-divider);
}

.station-ticket__prompt {
  color: var(--vp-c-brand-1);
  font-family: var(--vp-font-family-mono);
  font-size: 13px;
  font-weight: 700;
  text-align: center;
}

.station-ticket__command-text {
  display: flex;
  align-items: baseline;
  gap: 12px;
  min-width: 0;
}

.station-ticket__command-text span {
  flex: 0 0 70px;
  color: var(--vp-c-text-3);
  font-family: var(--vp-font-family-mono);
  font-size: 8.5px;
  font-weight: 700;
  letter-spacing: .11em;
}

.station-ticket__command-text code {
  min-width: 0;
  padding: 0;
  overflow: hidden;
  background: none;
  color: var(--vp-c-text-1);
  font-family: var(--vp-font-family-mono);
  font-size: 12px;
  line-height: 1.6;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.station-ticket__copy {
  gap: 5px;
  min-width: 55px;
  justify-content: center;
  padding: 4px 7px;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: var(--vp-c-text-3);
  font-size: 10.5px;
  line-height: 1;
  cursor: pointer;
  transition: color .18s ease, background-color .18s ease;
}

.station-ticket__copy:hover,
.station-ticket__copy.is-copied {
  background: var(--vp-c-brand-soft);
  color: var(--vp-c-brand-1);
}

.station-ticket__copy svg {
  width: 13px;
  height: 13px;
  fill: none;
  stroke: currentColor;
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: 1.35;
}

@media (max-width: 639px) {
  .station-ticket {
    grid-template-columns: 52px minmax(0, 1fr);
    margin: 18px 0 24px;
    border-radius: 14px;
  }

  .station-ticket__rail span {
    width: 32px;
    height: 32px;
  }

  .station-ticket__rail i {
    top: 53px;
  }

  .station-ticket__body {
    padding: 15px 12px 15px 0;
  }

  .station-ticket__header {
    align-items: flex-start;
    flex-direction: column;
    gap: 9px;
    margin-bottom: 12px;
  }

  .station-ticket__command {
    grid-template-columns: 14px minmax(0, 1fr) auto;
    padding: 9px 7px;
  }

  .station-ticket__command-text {
    display: grid;
    gap: 2px;
  }

  .station-ticket__command-text span {
    flex-basis: auto;
  }

  .station-ticket__command-text code {
    overflow-wrap: anywhere;
    text-overflow: clip;
    white-space: normal;
  }

  .station-ticket__copy span {
    display: none;
  }

  .station-ticket__copy {
    min-width: 28px;
    align-self: start;
    padding: 6px 4px;
  }
}
</style>
