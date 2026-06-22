import { execFileSync } from 'node:child_process'

// VitePress config 在 Node 运行,deploy CI 已 fetch-depth: 0,可零依赖拿到 git 信号。
// 版本展示的唯一真相源是 git tag(package.json version 是停滞占位值,不可用)。
// 非 git 环境 / 无 tag 时各字段回退,不抛错。

function git(args: string[]): string {
  try {
    return execFileSync('git', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim()
  } catch {
    return '' // 非 git 仓库 / 无 tag → 回退
  }
}

export interface BuildInfo {
  /** git describe 结果,如 035_multi_terminal 或 035_multi_terminal-34-geef96c0(-dirty 表示有未提交改动) */
  version: string
  /** 7 位短 SHA */
  sha: string
  /** 构建日期 YYYY-MM-DD */
  date: string
}

export function getBuildInfo(): BuildInfo {
  return {
    version: git(['describe', '--tags', '--always', '--dirty']) || 'dev',
    sha: git(['rev-parse', '--short=7', 'HEAD']),
    date: new Date().toISOString().substring(0, 10),
  }
}
