#!/bin/bash
# Run one GitHub Actions job locally through act, using a throwaway checkout so
# actions/checkout never cleans the real worktree.
#
# Defaults are tuned for WSL + Docker Desktop + a host proxy on 127.0.0.1:7890:
# containers reach that proxy as host.docker.internal:7890.
#
# Usage:
#   scripts/act_ci_job.sh [job]
#
# Useful overrides:
#   ACT_PROXY=                         # disable proxy env injection
#   ACT_PROXY=http://host.docker.internal:7890
#   ACT_WORKDIR=build/act-gcc-smoke
#   ACT_IMAGE=catthehacker/ubuntu:act-latest
#   ACT_EVENT=pull_request             # default; use push for push-only repro
#   ACT_OFFLINE=0                      # allow act to fetch actions if cache is cold
#   ACT_PREPARE_ONLY=1                 # only sync + patch the temp worktree
#   ACT_CACHE_PORT=34568               # override if the local port is busy
#   ACT_ARTIFACT_PORT=34567

set -euo pipefail

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

JOB="${1:-gcc-smoke}"
EVENT="${ACT_EVENT:-pull_request}"
IMAGE="${ACT_IMAGE:-catthehacker/ubuntu:act-latest}"
DOCKER_HOSTNAME="${ACT_DOCKER_HOSTNAME:-host.docker.internal}"
PROXY="${ACT_PROXY:-http://$DOCKER_HOSTNAME:7890}"
OFFLINE="${ACT_OFFLINE:-1}"
PREPARE_ONLY="${ACT_PREPARE_ONLY:-0}"
CACHE_PORT="${ACT_CACHE_PORT:-34568}"
ARTIFACT_PORT="${ACT_ARTIFACT_PORT:-34567}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKDIR="${ACT_WORKDIR:-$REPO_ROOT/build/act-$JOB}"
EVENT_FILE="$WORKDIR/.act/event.json"

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[act-ci] $1 not found on PATH" >&2
        exit 1
    fi
}

case "$WORKDIR" in
    "$REPO_ROOT"/build/act-* | /tmp/cinux-act-*) ;;
    *)
        echo "[act-ci] refusing unsafe ACT_WORKDIR: $WORKDIR" >&2
        echo "[act-ci] use $REPO_ROOT/build/act-* or /tmp/cinux-act-*" >&2
        exit 1
        ;;
esac

need_cmd rsync
need_cmd git
need_cmd python3

if [ "$PREPARE_ONLY" = "0" ]; then
    need_cmd act
    need_cmd docker

    echo "[act-ci] act: $(act --version 2>/dev/null || echo unknown)"
    if ! docker version >/dev/null 2>&1; then
        echo "[act-ci] docker daemon is not reachable from this shell" >&2
        echo "[act-ci] check Docker Desktop is running and WSL integration is enabled" >&2
        exit 1
    fi
    echo "[act-ci] docker: $(docker version --format '{{.Server.Version}}' 2>/dev/null || echo reachable)"

    if [ "$OFFLINE" != "0" ] && ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "[act-ci] image missing locally: $IMAGE" >&2
        echo "[act-ci] run: docker pull $IMAGE" >&2
        echo "[act-ci] or set ACT_OFFLINE=0 to let act fetch what it needs" >&2
        exit 1
    fi

    if [ "$OFFLINE" != "0" ]; then
        echo "[act-ci] offline mode is on; local image/action cache will be reused"
    fi

    if [ -n "$PROXY" ] && command -v curl >/dev/null 2>&1; then
        host_proxy="${PROXY//$DOCKER_HOSTNAME/127.0.0.1}"
        if curl -I --max-time 5 -x "$host_proxy" http://archive.ubuntu.com/ubuntu/ >/dev/null 2>&1; then
            echo "[act-ci] proxy check ok: $host_proxy"
        else
            echo "[act-ci] warning: proxy check failed for $host_proxy; continuing" >&2
        fi
    fi
fi

if [ -e "$WORKDIR" ] && [ ! -w "$WORKDIR" ]; then
    if [ -n "${ACT_WORKDIR:-}" ]; then
        echo "[act-ci] ACT_WORKDIR is not writable: $WORKDIR" >&2
        echo "[act-ci] choose a new safe workdir or fix ownership" >&2
        exit 1
    fi
    WORKDIR="$(mktemp -d "/tmp/cinux-act-$JOB-XXXXXX")"
    EVENT_FILE="$WORKDIR/.act/event.json"
    echo "[act-ci] default workdir was not writable; using $WORKDIR"
else
    mkdir -p "$WORKDIR"
fi

echo "[act-ci] syncing worktree -> $WORKDIR"
rsync -a --delete \
    --exclude='/.git/' \
    --exclude='/.git' \
    --exclude='/build/' \
    --exclude='/.cache/' \
    --exclude='/cache.tzst' \
    --exclude='/manifest.txt' \
    "$REPO_ROOT/" "$WORKDIR/"

REF="${ACT_REF:-$(git -C "$REPO_ROOT" branch --show-current 2>/dev/null || true)}"
REF="${REF:-main}"
SHA="${ACT_SHA:-$(git -C "$REPO_ROOT" rev-parse HEAD)}"
REMOTE_URL="${ACT_REMOTE_URL:-$(git -C "$REPO_ROOT" config --get remote.origin.url || true)}"
REMOTE_URL="${REMOTE_URL:-local:$REPO_ROOT}"

python3 - "$WORKDIR" "$JOB" "$EVENT" "$REF" "$SHA" "$REMOTE_URL" "$REPO_ROOT" "${ACT_REPOSITORY:-}" "${ACT_PR_NUMBER:-0}" <<'PY'
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
job = sys.argv[2]
event = sys.argv[3]
ref = sys.argv[4]
sha = sys.argv[5]
remote_url = sys.argv[6]
repo_root = pathlib.Path(sys.argv[7])
repository_override = sys.argv[8]
pr_number = int(sys.argv[9])


def repo_full_name(url: str) -> str:
    if repository_override:
        return repository_override
    text = url.strip()
    text = re.sub(r"\.git$", "", text)
    if text.startswith("git@github.com:"):
        return text.split(":", 1)[1]
    match = re.search(r"github\.com[:/](.+/.+)$", text)
    if match:
        return match.group(1)
    return f"local/{repo_root.name}"


def insert_local_apt_update(path: pathlib.Path) -> None:
    if "Local act apt index refresh" in path.read_text():
        return

    lines = path.read_text().splitlines(keepends=True)
    out = []
    checkout_re = re.compile(r"^(\s*)-\s+uses:\s+actions/checkout@v\d+\s*$")
    i = 0

    while i < len(lines):
        match = checkout_re.match(lines[i])
        if not match:
            out.append(lines[i])
            i += 1
            continue

        step_indent = len(match.group(1))
        out.append(lines[i])
        i += 1

        while i < len(lines):
            if re.match(rf"^\s{{0,{step_indent}}}-\s+", lines[i]):
                break
            out.append(lines[i])
            i += 1

        indent = " " * step_indent
        out.extend([
            f"{indent}- name: Local act apt index refresh\n",
            f"{indent}  shell: bash\n",
            f"{indent}  run: |\n",
            f"{indent}    if [ -n \"${{HTTP_PROXY:-}}\" ]; then\n",
            f"{indent}      proxy=\"${{HTTP_PROXY}}\"\n",
            f"{indent}      sudo install -d /etc/apt/apt.conf.d\n",
            f"{indent}      printf 'Acquire::http::Proxy \"%s\";\\nAcquire::https::Proxy \"%s\";\\n' \"$proxy\" \"$proxy\" | sudo tee /etc/apt/apt.conf.d/99act-proxy >/dev/null\n",
            f"{indent}    fi\n",
            f"{indent}    sudo -E apt-get update\n",
        ])

    path.write_text("".join(out))


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def replace_upload_artifacts(path: pathlib.Path) -> None:
    text = path.read_text()
    if "Local act artifact fallback" in text:
        return

    lines = text.splitlines(keepends=True)
    out = []
    step_re = re.compile(r"^(\s*)-\s+name:\s*(.+?)\s*$")
    i = 0

    while i < len(lines):
        match = step_re.match(lines[i])
        if not match:
            out.append(lines[i])
            i += 1
            continue

        step_indent = len(match.group(1))
        step_lines = [lines[i]]
        i += 1
        while i < len(lines):
            line_indent = len(lines[i]) - len(lines[i].lstrip(" "))
            if re.match(rf"^\s{{0,{step_indent}}}-\s+", lines[i]):
                break
            if lines[i].strip() and line_indent <= step_indent and not lines[i].lstrip().startswith("#"):
                break
            step_lines.append(lines[i])
            i += 1

        if not any("uses: actions/upload-artifact@" in line for line in step_lines):
            out.extend(step_lines)
            continue

        original_name = match.group(2).strip()
        if_line = next((line for line in step_lines if re.match(r"^\s+if:\s+", line)), None)
        path_value = ""
        for line in step_lines:
            path_match = re.match(r"^\s+path:\s*(.+?)\s*$", line)
            if path_match:
                path_value = path_match.group(1).strip()
                break

        indent = " " * step_indent
        child_indent = indent + "  "
        run_indent = indent + "    "
        artifact_path = path_value or "."
        out.append(f"{indent}- name: Local act artifact fallback ({original_name})\n")
        if if_line is not None:
            out.append(f"{child_indent}{if_line.strip()}\n")
        out.extend([
            f"{child_indent}shell: bash\n",
            f"{child_indent}run: |\n",
            f"{run_indent}artifact_path={shell_quote(artifact_path)}\n",
            f"{run_indent}echo \"[act-ci] upload-artifact is skipped under local act\"\n",
            f"{run_indent}echo \"[act-ci] artifact path: $artifact_path\"\n",
            f"{run_indent}if [ -f \"$artifact_path\" ]; then\n",
            f"{run_indent}  tail -n 120 \"$artifact_path\"\n",
            f"{run_indent}else\n",
            f"{run_indent}  echo \"[act-ci] artifact file not found, nothing to show\"\n",
            f"{run_indent}fi\n",
        ])

    path.write_text("".join(out))


workflow = root / ".github/workflows/ci.yml"
insert_local_apt_update(workflow)
replace_upload_artifacts(workflow)

full_name = repo_full_name(remote_url)
owner, name = full_name.split("/", 1)
clone_url = f"https://github.com/{full_name}.git"

payload = {
    "ref": f"refs/heads/{ref}",
    "repository": {
        "full_name": full_name,
        "name": name,
        "clone_url": clone_url,
        "html_url": f"https://github.com/{full_name}",
        "owner": {"login": owner},
    },
    "pull_request": {
        "number": pr_number,
        "head": {
            "ref": ref,
            "sha": sha,
            "repo": {"full_name": full_name, "clone_url": clone_url},
        },
        "base": {
            "ref": "main",
            "repo": {"full_name": full_name, "clone_url": clone_url},
        },
    },
}

event_dir = root / ".act"
event_dir.mkdir(parents=True, exist_ok=True)
(event_dir / "event.json").write_text(json.dumps(payload, indent=2) + "\n")

print(f"[act-ci] prepared {event} event for {full_name}@{ref} ({sha[:12]})")
PY

if [ -e "$WORKDIR/.git" ]; then
    rm -rf "$WORKDIR/.git"
fi

git -C "$WORKDIR" init -q -b "$REF" 2>/dev/null || git -C "$WORKDIR" init -q -b main
git -C "$WORKDIR" remote add origin "$REMOTE_URL" 2>/dev/null || git -C "$WORKDIR" remote set-url origin "$REMOTE_URL"
cat >> "$WORKDIR/.git/info/exclude" <<'EOF'
/build/
/.cache/
/.act/cache/
/.act/artifacts/
/cache.tzst
/manifest.txt
EOF
git -C "$WORKDIR" add -A
git -C "$WORKDIR" -c user.email=act@local -c user.name=act \
    commit -q --allow-empty -m act-base >/dev/null

if [ "$PREPARE_ONLY" != "0" ]; then
    echo "[act-ci] prepared temp worktree at $WORKDIR"
    echo "[act-ci] event file: $EVENT_FILE"
    exit 0
fi

cmd=(
    act "$EVENT"
    -C "$WORKDIR"
    -e "$EVENT_FILE"
    -j "$JOB"
    -P "ubuntu-latest=$IMAGE"
    --cache-server-addr 0.0.0.0
    --cache-server-port "$CACHE_PORT"
    --cache-server-path "$WORKDIR/.act/cache"
    --cache-server-external-url "http://$DOCKER_HOSTNAME:$CACHE_PORT"
    --artifact-server-addr 0.0.0.0
    --artifact-server-port "$ARTIFACT_PORT"
    --artifact-server-path "$WORKDIR/.act/artifacts"
    --rm
)

if [ "$OFFLINE" != "0" ]; then
    cmd+=(--pull=false --action-offline-mode)
fi

if [ -n "$PROXY" ]; then
    cmd+=(
        --env "HTTP_PROXY=$PROXY"
        --env "HTTPS_PROXY=$PROXY"
        --env "http_proxy=$PROXY"
        --env "https_proxy=$PROXY"
        --env "NO_PROXY=localhost,127.0.0.1,::1,$DOCKER_HOSTNAME"
        --env "no_proxy=localhost,127.0.0.1,::1,$DOCKER_HOSTNAME"
    )
fi

echo "[act-ci] running job '$JOB'"
echo "[act-ci] workdir=$WORKDIR"
echo "[act-ci] image=$IMAGE offline=$OFFLINE proxy=${PROXY:-disabled}"
"${cmd[@]}"
