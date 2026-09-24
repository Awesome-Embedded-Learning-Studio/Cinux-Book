<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { CHECKPOINTS_BASE, fetchRepoText } from '../utils/repo-file'
import {
  normalizeFillAnswer,
  parseCheckpointData,
  type CheckpointProblemData,
} from '../utils/quiz-data'
import { QUIZ_PROGRESS_EVENT, useQuizState } from '../composables/useQuizProgress'
import { renderMarkdown } from '../utils/md-render'

const props = defineProps<{
  /** checkpoints/ 下的题目目录,如 "01-boot/mbr-limits" */
  src: string
  /** 可选编号(卷内序号),纯展示 */
  no?: number | string
}>()

// ── 懒加载:滚进视口才拉数据 ───────────────────────────────────
const rootEl = ref<HTMLElement | null>(null)
const inView = ref(false)
let observer: IntersectionObserver | null = null

const phase = ref<'idle' | 'loading' | 'ready' | 'error'>('idle')
const errorMsg = ref('')
const problemHtml = ref('')
const answerHtml = ref('')
const data = ref<CheckpointProblemData | null>(null)
const title = ref('')

// ── 进度(localStorage,跨会话保留;status 只升不降) ────────────
const problemId = `checkpoint:${props.src}`
const { record, update, reset } = useQuizState(problemId)
const status = computed(() => record.value?.status ?? 'untouched')
const hintsUsed = computed(() => record.value?.hintsUsed ?? 0)
const skipped = computed(() => record.value?.skipped === true)
const hints = ref<string[]>([])

// reveal 型题目:参考答案直接来自 quiz.json,预先渲染成 html
const inlineAnswerHtml = computed(() => {
  if (data.value?.type === 'reveal') {
    try { return renderMarkdown(data.value.answer, {}).html } catch { return '' }
  }
  return ''
})

const statusText = computed(() => ({
  untouched: '未作答',
  attempted: '尝试过',
  revealed: '看过答案',
  passed: '已通过',
})[status.value])
const statusClass = computed(() => ({
  untouched: 'is-untouched',
  attempted: 'is-attempted',
  revealed: 'is-revealed',
  passed: 'is-passed',
})[status.value])

async function load() {
  phase.value = 'loading'
  try {
    const [problemMd, quizJson] = await Promise.all([
      fetchRepoText(`${CHECKPOINTS_BASE}/${props.src}/problem.md`),
      fetchRepoText(`${CHECKPOINTS_BASE}/${props.src}/quiz.json`),
    ])
    const parsed = JSON.parse(quizJson) as { title?: string, hints?: string[] }
    title.value = parsed.title ?? ''
    hints.value = parsed.hints ?? []
    data.value = parseCheckpointData(quizJson)
    const rendered = renderMarkdown(problemMd, {})
    problemHtml.value = rendered.html
    phase.value = 'ready'
  } catch (err) {
    errorMsg.value = err instanceof Error ? err.message : String(err)
    phase.value = 'error'
  }
}

async function ensureAnswerHtml() {
  if (answerHtml.value) return
  try {
    const md = await fetchRepoText(`${CHECKPOINTS_BASE}/${props.src}/answer.md`)
    answerHtml.value = renderMarkdown(md, {}).html
  } catch {
    answerHtml.value = ''
  }
}

onMounted(() => {
  if (!rootEl.value) return
  observer = new IntersectionObserver((entries) => {
    if (entries.some(e => e.isIntersecting)) {
      inView.value = true
      observer?.disconnect()
      observer = null
      load()
    }
  }, { rootMargin: '320px' })
  observer.observe(rootEl.value)
})
onBeforeUnmount(() => observer?.disconnect())

// ── 交互:按题型分支 ──────────────────────────────────────────
const choicePicked = ref<number | null>(null)
const choiceWrong = ref(false)
const fillInput = ref('')
const fillChecked = ref<null | boolean>(null)
const bugMarks = ref<number[]>([])
const bugChecked = ref<null | boolean>(null)
const answerOpen = ref(false)

function pickChoice(i: number) {
  if (status.value === 'passed') return
  choicePicked.value = i
  choiceWrong.value = false
}

function checkChoice() {
  const d = data.value
  if (d?.type !== 'choice' || choicePicked.value === null) return
  if (choicePicked.value === d.answer) {
    update({ status: 'passed' })
    window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
  } else {
    choiceWrong.value = true
    update({ status: 'attempted' })
    window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
  }
}

function checkFill() {
  const d = data.value
  if (d?.type !== 'fill' || !fillInput.value.trim()) return
  const ok = normalizeFillAnswer(fillInput.value) === normalizeFillAnswer(d.answer)
  fillChecked.value = ok
  update({ status: ok ? 'passed' : 'attempted' })
  window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
}

function toggleBug(i: number) {
  if (status.value === 'passed') return
  bugMarks.value = bugMarks.value.includes(i)
    ? bugMarks.value.filter(x => x !== i)
    : [...bugMarks.value, i]
}

function checkBug() {
  const d = data.value
  if (d?.type !== 'find-bug' || bugMarks.value.length === 0) return
  const a = [...bugMarks.value].sort((x, y) => x - y)
  const b = [...d.bugLines].sort((x, y) => x - y)
  const ok = a.length === b.length && a.every((v, i) => v === b[i])
  bugChecked.value = ok
  update({ status: ok ? 'passed' : 'attempted' })
  window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
}

async function openAnswer() {
  answerOpen.value = true
  await ensureAnswerHtml()
  if (status.value !== 'passed') {
    update({ status: 'revealed' })
    window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
  }
}

function selfEval(ok: boolean) {
  update({ status: ok ? 'passed' : 'attempted' })
  window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
}

function unlockHint() {
  update({ hintsUsed: Math.min(hintsUsed.value + 1, hints.value.length) })
}

function toggleSkip() {
  update({ skipped: !skipped.value })
}

function resetAll() {
  reset()
  choicePicked.value = null
  choiceWrong.value = false
  fillInput.value = ''
  fillChecked.value = null
  bugMarks.value = []
  bugChecked.value = null
  answerOpen.value = false
  window.dispatchEvent(new CustomEvent(QUIZ_PROGRESS_EVENT))
}
</script>

<template>
  <div ref="rootEl" class="checkpoint" :class="[statusClass, { 'is-skipped': skipped }]">
    <div class="checkpoint__bar">
      <span class="checkpoint__badge">检查点<template v-if="no"> · {{ no }}</template></span>
      <span v-if="title" class="checkpoint__title">{{ title }}</span>
      <span class="checkpoint__spacer" />
      <span class="checkpoint__status">{{ statusText }}</span>
    </div>

    <!-- 懒加载占位:高度稳定,不抖版 -->
    <div v-if="phase === 'idle' || phase === 'loading'" class="checkpoint__ph">
      {{ phase === 'loading' ? '加载检查点…' : '检查点待加载' }}
    </div>

    <div v-else-if="phase === 'error'" class="checkpoint__error">
      检查点数据加载失败:{{ errorMsg }}
      <button class="checkpoint__ghostbtn" @click="load()">重试</button>
    </div>

    <template v-else>
      <!-- 题面 -->
      <div class="checkpoint__problem md-render" v-html="problemHtml" />

      <!-- 提示 -->
      <div v-if="hints.length > 0 && hintsUsed > 0" class="checkpoint__hints">
        <div v-for="i in hintsUsed" :key="i" class="checkpoint__hint">提示 {{ i }}:{{ hints[i - 1] }}</div>
      </div>

      <!-- 选择题 -->
      <div v-if="data?.type === 'choice'" class="checkpoint__body">
        <button
          v-for="(opt, i) in data.options"
          :key="i"
          class="checkpoint__opt"
          :class="{
            'is-picked': choicePicked === i,
            'is-answer': answerOpen && i === data.answer,
          }"
          :disabled="status === 'passed'"
          @click="pickChoice(i)"
        >{{ opt }}</button>
        <p v-if="choiceWrong" class="checkpoint__feedback is-bad">不对,再想想。</p>
        <div class="checkpoint__actions">
          <button class="checkpoint__btn" :disabled="choicePicked === null" @click="checkChoice()">检查</button>
          <button v-if="hints.length > hintsUsed" class="checkpoint__ghostbtn" @click="unlockHint()">解锁提示</button>
        </div>
      </div>

      <!-- 填空 -->
      <div v-else-if="data?.type === 'fill'" class="checkpoint__body">
        <textarea
          v-model="fillInput"
          class="checkpoint__fill"
          :placeholder="data.placeholder ?? '把答案写在这里…'"
          rows="3"
          :disabled="status === 'passed'"
        />
        <p v-if="fillChecked === false" class="checkpoint__feedback is-bad">还不对,注意空格和大小写细节。</p>
        <p v-if="fillChecked" class="checkpoint__feedback is-ok">正确!</p>
        <div class="checkpoint__actions">
          <button class="checkpoint__btn" :disabled="!fillInput.trim()" @click="checkFill()">{{ data.action ?? '检查' }}</button>
          <button v-if="hints.length > hintsUsed" class="checkpoint__ghostbtn" @click="unlockHint()">解锁提示</button>
        </div>
      </div>

      <!-- 找错行 -->
      <div v-else-if="data?.type === 'find-bug'" class="checkpoint__body">
        <div class="checkpoint__code">
          <template v-for="(line, i) in data.code" :key="i">
            <button
              class="checkpoint__line"
              :class="{
                'is-marked': bugMarks.includes(i),
                'is-buggy': answerOpen && data.bugLines.includes(i),
              }"
              :disabled="status === 'passed'"
              @click="toggleBug(i)"
            ><span class="checkpoint__lineno">{{ i + 1 }}</span><span class="checkpoint__codetext">{{ line }}</span></button>
          </template>
        </div>
        <p v-if="bugChecked === false" class="checkpoint__feedback is-bad">标注的行不对,bug 藏在别处。</p>
        <div class="checkpoint__actions">
          <button class="checkpoint__btn" :disabled="bugMarks.length === 0" @click="checkBug()">检查</button>
          <button v-if="hints.length > hintsUsed" class="checkpoint__ghostbtn" @click="unlockHint()">解锁提示</button>
        </div>
      </div>

      <!-- 先想后看 -->
      <div v-else-if="data?.type === 'reveal'" class="checkpoint__body">
        <div v-if="!answerOpen" class="checkpoint__actions">
          <button class="checkpoint__btn" @click="openAnswer()">想好了,看答案</button>
          <button v-if="hints.length > hintsUsed" class="checkpoint__ghostbtn" @click="unlockHint()">解锁提示</button>
        </div>
        <template v-else>
          <div class="checkpoint__answer md-render" v-html="answerHtml || inlineAnswerHtml" />
          <div class="checkpoint__actions">
            <span class="checkpoint__selftext">自评:</span>
            <button class="checkpoint__btn" @click="selfEval(true)">我答对了</button>
            <button class="checkpoint__ghostbtn" @click="selfEval(false)">没答对</button>
          </div>
        </template>
      </div>

      <!-- 答案(非 reveal 型) -->
      <div v-if="data?.type !== 'reveal' && answerOpen" class="checkpoint__answer md-render" v-html="answerHtml" />

      <div class="checkpoint__foot">
        <button
          v-if="data?.type !== 'reveal' && status !== 'passed'"
          class="checkpoint__ghostbtn"
          @click="openAnswer()"
        >看答案</button>
        <button class="checkpoint__ghostbtn" @click="toggleSkip()">{{ skipped ? '不再跳过' : '先跳过' }}</button>
        <button class="checkpoint__ghostbtn" @click="resetAll()">重做</button>
      </div>
    </template>
  </div>
</template>
