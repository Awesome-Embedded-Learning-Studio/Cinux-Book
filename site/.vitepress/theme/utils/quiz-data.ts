// 章节检查点数据模型(Cinux 改造版)。
// 上游 Tutorial_AwesomeModernCPP 的 QuizProblem 面向在线编译判题
// (judge-assert / judge-io 走编辑器+评测后端),OS 内核代码无法在线运行,
// 这里裁剪为纯客户端可判定的四种题型:
//   choice     单选(选项列表 + 正确下标)
//   fill       填空(normalize 后字符串比对)
//   find-bug   找错行(高亮行号选择)
//   reveal     先想后看(自评:我答对了/没答对)
// 数据三件套(checkpoints/<卷>/<题目>/):
//   problem.md  题面(md 渲染)
//   quiz.json   结构化题目
//   answer.md   参考答案(可选,点开即视为「已看答案」)

export interface ChoiceProblem {
  type: 'choice'
  /** 题干 */
  question: string
  options: string[]
  /** 正确选项下标(0-based) */
  answer: number
}

export interface FillProblem {
  type: 'fill'
  question: string
  answer: string
  /** 提交按钮文案(默认「检查」) */
  action?: string
  placeholder?: string
}

export interface FindBugProblem {
  type: 'find-bug'
  question: string
  code: string[]
  /** 有 bug 的行(0-based,可多行) */
  bugLines: number[]
}

export interface RevealProblem {
  type: 'reveal'
  question: string
  /** 点击展开的参考答案(md 渲染) */
  answer: string
}

export type CheckpointProblemData
  = | ChoiceProblem
    | FillProblem
    | FindBugProblem
    | RevealProblem

export function parseCheckpointData(json: string): CheckpointProblemData {
  const obj = JSON.parse(json)
  if (typeof obj !== 'object' || obj === null) {
    throw new Error('quiz.json 顶层必须是对象')
  }
  if (typeof obj.type !== 'string') {
    throw new Error('quiz.json 缺少 type 字段')
  }
  switch (obj.type) {
    case 'choice': {
      if (!Array.isArray(obj.options)) throw new Error('choice 题缺少 options 数组')
      if (typeof obj.answer !== 'number') throw new Error('choice 题缺少数字 answer')
      return { ...obj }
    }
    case 'fill': {
      if (typeof obj.answer !== 'string') throw new Error('fill 题缺少字符串 answer')
      return { ...obj }
    }
    case 'find-bug': {
      if (!Array.isArray(obj.code)) throw new Error('find-bug 题缺少 code 数组')
      if (!Array.isArray(obj.bugLines)) throw new Error('find-bug 题缺少 bugLines 数组')
      return { ...obj }
    }
    case 'reveal': {
      if (typeof obj.answer !== 'string') throw new Error('reveal 题缺少 answer')
      return { ...obj }
    }
    default:
      throw new Error(
        `未知题型 ${obj.type}(检查点只支持 choice/fill/find-bug/reveal,` +
        ' 在线编译类题型(judge-*)未随 Cinux 改造引入)'
      )
  }
}

/** fill 题的宽松比对:去行尾空白、去空行、压首尾空白 */
export function normalizeFillAnswer(s: string): string {
  return s
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map(l => l.replace(/\s+$/, ''))
    .join('\n')
    .replace(/^\n+|\n+$/g, '')
    .trim()
}
