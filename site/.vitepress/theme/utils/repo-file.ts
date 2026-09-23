// 按仓库路径取文件文本:本地站点资源 → GitHub raw 兜底。
// 检查点数据(problem.md / quiz.json / answer.md)都走这里:
// dev 由 config/index.ts 的中间件直供,生产由 build.ts 拷进 dist/checkpoints,
// 兜底保证任何环境(比如本地直接开 dist 静态服务但漏拷)都能拿到。

import { withBase } from 'vitepress'

const RAW_BASE = 'https://raw.githubusercontent.com/Charliechen114514/Cinux-Book/main'

/** 检查点数据的统一访问前缀(dev 中间件 / build.ts 收尾拷贝,两边同构) */
export const CHECKPOINTS_BASE = 'checkpoints'

export async function fetchRepoText(path: string): Promise<string> {
  const normalized = path.replace(/^\/+/, '')
  const local = await tryFetch(withBase(`/${normalized}`))
  if (local !== null) return local
  const raw = await tryFetch(`${RAW_BASE}/${normalized}`)
  if (raw !== null) return raw
  throw new Error(`无法读取 ${normalized}(本地与 GitHub raw 都失败)`)
}

async function tryFetch(url: string): Promise<string | null> {
  try {
    const response = await fetch(url)
    if (!response.ok) return null
    return await response.text()
  } catch {
    return null
  }
}
