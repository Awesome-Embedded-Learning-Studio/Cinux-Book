#!/usr/bin/env python3
"""CinuxOS v1.0.0 → Cinux-Book patch-replay harness (Stage 0 脚手架).

把 CinuxOS 的 first-parent commit 链逐个打 patch 到 Cinux-Book,实现 1:1 源码回迁。
机制与纪律见 meta/v1.0.0-migration-roadmap.md(§0.5 + 附录)。

Usage:
  python3 tools/replay.py list [--range A..B]      # 列 replay 工作清单(带序号/SHA/skip 标)
  python3 tools/replay.py next                     # 下一个待 apply 的步
  python3 tools/replay.py apply <n|SHA> [--dry]    # 打第 n 步(或 SHA)的 patch
  python3 tools/replay.py mark-skip <n> <reason>   # 标记某步 skip(CI/docs/release 等)

环境:
  CINUXOS_REPO:CinuxOS 仓路径,默认 ~/CinuxOS。
  CINUXOS_RANGE:replay 范围,默认 cde30d1..v1.0.0。

replay 只取「代码」路径(kernel/boot/user/libs/third_party/cmake/tools/test/scripts +
CMakeLists/.gitmodules),自动滤掉 Book 自留的 document/ assets/ .github/ README/site/pnpm-*。
"""
from __future__ import annotations
import json, os, subprocess, sys
from pathlib import Path

CINUXOS = Path(os.environ.get("CINUXOS_REPO", str(Path.home() / "CinuxOS")))
RANGE = os.environ.get("CINUXOS_RANGE", "cde30d1..v1.0.0")
BOOK = Path.cwd()
STATE = BOOK / "meta" / "replay-state.json"

# 只 replay 这些路径(代码/构建)。Book 自留目录(document/assets/.github/README/site/pnpm-*)不取。
INCLUDE = ["kernel", "boot", "user", "libs", "third_party", "cmake", "tools", "test",
           "scripts", "CMakeLists.txt", ".gitmodules"]

# 附录里的 skip 清单(纯 CI/docs/release/format,不单成 tag)。value=reason。
SKIP_DEFAULT: dict[int, str] = {
    1: "migrate CI (.github)",
    3: "style: auto-format",
    7: "ci: clone submodules",
    10: "docs(ai) plan/directives", 12: "docs(ai) CODING-TASTE",
    14: "docs(ai) DEVLOG", 15: "docs(ai) M0 收尾",
    16: "merge clang-format", 18: "ci format 禁用",
    # NOTE: step 17 (d0d7b56 "host 单测跟进 InodeOps→ErrorOr") is a REAL code step
    # (4 test/*.cpp get .value()/!.ok()), NOT skip-able. It was wrongly skip-flagged
    # here once, which left test_shell_redirect/test_pipe/test_sys_pipe/test_ext2_inode_ops
    # broken from the ErrorOr step on; caught up during 054b. Do not re-skip.
    19: "merge origin/main", 20: "docs(ai) DIRECTIVES",
    51: "docs sync f13/f5m5", 52: "tidy debt",
    73: "docs 5 弧审核", 78: "docs notes",
    82: "ci setup-cinux", 86: "perf (可选)", 87: "docs gcc-finale",
    93: "docs roadmap sync",
    99: "hotfix readme", 100: "fix(ci)", 101: "fix(ci)", 103: "fix(ci,cmake)",
    104: "rc1 revise", 105: "cmake-one-command", 106: "release-finalize",
}


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout


def steps() -> list[tuple[int, str, str, str]]:
    """返回 [(n, sha, date, subject)],时间正序。"""
    out = git(CINUXOS, "log", "--first-parent", "--reverse", "--format=%H|%ad|%s",
              "--date=short", RANGE)
    rows = []
    for i, line in enumerate(out.splitlines(), 1):
        sha, date, subj = line.split("|", 2)
        rows.append((i, sha, date, subj))
    return rows


def load_state() -> dict:
    if STATE.exists():
        return json.loads(STATE.read_text())
    return {"applied": [], "skipped": {}, "tag_for_step": {}}


def save_state(st: dict) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(st, indent=2, ensure_ascii=False))


def skip_reason(n: int) -> str | None:
    return SKIP_DEFAULT.get(n) or load_state().get("skipped", {}).get(str(n))


def cmd_list(args):
    rng = f" (--range {args[0]})" if args else ""
    print(f"# replay worklist{rng}  range={RANGE}  cinuxos={CINUXOS}")
    st = load_state()
    applied = set(st["applied"])
    for n, sha, date, subj in steps():
        tag = ""
        if str(n) in st.get("tag_for_step", {}):
            tag = f"  → {st['tag_for_step'][str(n)]}"
        if sha in applied:
            mark = "✅"
        elif skip_reason(n):
            mark = "⏭ "
        else:
            mark = "⬜"
        print(f"{mark} {n:>3} {sha[:7]} {date} {subj[:60]}{tag}")


def cmd_next(args):
    st = load_state()
    applied = set(st["applied"])
    for n, sha, date, subj in steps():
        if sha in applied or skip_reason(n):
            continue
        print(f"下一步 #{n} {sha[:7]} {date} {subj}")
        return
    print("replay 链已走完。")


def cmd_apply(args):
    if not args:
        sys.exit("用法:apply <n|SHA> [--dry]")
    target, dry = args[0], "--dry" in args
    all_steps = steps()
    if target.isdigit():
        n = int(target); sha = all_steps[n - 1][1]
    else:
        sha = target; n = next((i for i, s in enumerate(all_steps, 1) if s[1] == sha), 0)
    subj = next(s[3] for s in all_steps if s[1] == sha)
    print(f"==> apply #{n} {sha[:7]} {subj}")
    patch = git(CINUXOS, "format-patch", "-1", "--stdout", sha, "--", *INCLUDE)
    if not patch.strip():
        print("  patch 为空(只碰了被滤掉的 document/CI 等)→ 视为 skip。")
        st = load_state(); st["skipped"][str(n)] = "empty after filter"; save_state(st)
        return
    if dry:
        print(patch); return
    # 写临时 patch 文件,git am
    pf = Path("/tmp/replay_step.patch"); pf.write_text(patch)
    r = subprocess.run(["git", "am", "--3way", str(pf)], capture_output=True, text=True)
    print(r.stdout.strip() or r.stderr.strip())
    if r.returncode != 0:
        print(f"!! git am 失败(退出 {r.returncode})。排查:git am --abort;查前置步漏 / 4 个 CMake。")
        sys.exit(1)
    st = load_state(); st["applied"].append(sha); save_state(st)
    print(f"✅ applied #{n}。记入 {STATE.name}。下一步:cmake --build build 验证绿。")


def cmd_mark_skip(args):
    if len(args) < 2:
        sys.exit("用法:mark-skip <n> <reason>")
    n, reason = args[0], " ".join(args[1:])
    st = load_state(); st["skipped"][str(n)] = reason; save_state(st)
    print(f"marked #{n} skip: {reason}")


def cmd_notes(args):
    """找某步命中的 CinuxOS dev note(309 篇,按日期 ±2 天 + 弧关键词)。note 是抓手不是福音。"""
    if not args:
        sys.exit("用法:notes <n|SHA>  # 找该步命中的 CinuxOS dev note")
    target = args[0]
    all_steps = steps()
    if target.isdigit():
        n = int(target)
        if n < 1 or n > len(all_steps):
            sys.exit(f"步号超界(1..{len(all_steps)})")
        sha, date, subj = all_steps[n - 1][1], all_steps[n - 1][2], all_steps[n - 1][3]
    else:
        m = next((s for s in all_steps if s[1] == target), None)
        if not m:
            sys.exit("步不存在")
        n, sha, date, subj = m
    print(f"# step #{n} {sha[:7]} {date} {subj}")
    notes_dir = CINUXOS / "document" / "notes"
    from datetime import date as dt_date
    import re
    try:
        d = dt_date.fromisoformat(date)
    except ValueError:
        d = None
    by_date = []
    if d:
        for f in sorted(notes_dir.glob("*.md")):
            try:
                fd = dt_date.fromisoformat(f.name[:10])
            except ValueError:
                continue
            if abs((fd - d).days) <= 2:
                by_date.append(f)
    # 弧关键词补搜(subject 或 PR 里的 f<N>-m<M>)
    arc_tokens = set(re.findall(r'f\d+', subj.lower()))
    by_arc = []
    if arc_tokens:
        for f in sorted(notes_dir.glob("*.md")):
            if any(tok in f.name.lower() for tok in arc_tokens) and f not in by_date:
                by_arc.append(f)
    if by_date:
        print(f"## 命中 dev note(日期 ±2 天,{len(by_date)} 篇):")
        for f in by_date:
            print(f"  ~/CinuxOS/document/notes/{f.name}")
    if by_arc:
        print(f"## 弧关键词命中({', '.join(arc_tokens)},{len(by_arc)} 篇):")
        for f in by_arc:
            print(f"  ~/CinuxOS/document/notes/{f.name}")
    if not by_date and not by_arc:
        print("## 无命中 note —— 直接看源码(`git -C ~/CinuxOS show <sha>`).")
        print("## 提示:CinuxOS notes 从 2026-05-26 起密集;更早的步(fork 当日)可能无 note。")


CMDS = {"list": cmd_list, "next": cmd_next, "apply": cmd_apply,
        "mark-skip": cmd_mark_skip, "notes": cmd_notes}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        sys.exit(__doc__)
    CMDS[sys.argv[1]](sys.argv[2:])
