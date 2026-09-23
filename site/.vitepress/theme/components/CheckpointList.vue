<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { CHECKPOINTS_BASE, fetchRepoText } from '../utils/repo-file'
import {
  loadQuizRecord,
  QUIZ_PROGRESS_EVENT,
  type QuizRecord,
} from '../composables/useQuizProgress'
import CheckpointProblem from './CheckpointProblem.vue'

const props = defineProps<{
  /** 检查点条目:src 为 checkpoints/ 下目录;title 缺省时从 quiz.json 读 */
  items: Array<{ src: string, title?: string }>
  /** 列表标题 */
  heading?: string
}>()

interface Row {
  src: string
  title: string
}

const rows = ref<Row[]>([])
const loaded = ref(false)

onMounted(async () => {
  rows.value = await Promise.all(props.items.map(async (it, i) => {
    if (it.title) return { src: it.src, title: it.title }
    try {
      const json = await fetchRepoText(`${CHECKPOINTS_BASE}/${it.src}/quiz.json`)
      const parsed = JSON.parse(json) as { title?: string }
      return { src: it.src, title: parsed.title ?? `检查点 ${i + 1}` }
    } catch {
      return { src: it.src, title: `检查点 ${i + 1}` }
    }
  }))
  loaded.value = true
  refreshStatuses()
})

const statuses = ref<Record<string, QuizRecord | null>>({})
function refreshStatuses() {
  const next: Record<string, QuizRecord | null> = {}
  for (const r of rows.value) next[r.src] = loadQuizRecord(`checkpoint:${r.src}`)
  statuses.value = next
}
const onProgressEvent = () => refreshStatuses()
onMounted(() => window.addEventListener(QUIZ_PROGRESS_EVENT, onProgressEvent))
onBeforeUnmount(() => window.removeEventListener(QUIZ_PROGRESS_EVENT, onProgressEvent))

const passedCount = computed(() =>
  Object.values(statuses.value).filter(r => r?.status === 'passed').length
)
const touchedCount = computed(() =>
  Object.values(statuses.value).filter(r => r && r.status !== 'untouched').length
)
</script>

<template>
  <section class="cplist">
    <header class="cplist__head">
      <h3 class="cplist__title">{{ heading ?? '本章检查点' }}</h3>
      <span class="cplist__progress">
        {{ passedCount }}/{{ rows.length }} 通过
        <template v-if="touchedCount > passedCount"> · {{ touchedCount - passedCount }} 进行中</template>
      </span>
    </header>
    <CheckpointProblem
      v-for="(r, i) in rows"
      :key="r.src"
      :src="r.src"
      :no="i + 1"
    />
  </section>
</template>
