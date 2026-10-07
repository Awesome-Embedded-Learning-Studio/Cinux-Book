#!/usr/bin/env bash
# Rejects '(void)x' casts: parameters get [[maybe_unused]], discarded
# calls become bare calls. Exit 1 names every offender.
status=0
for file in "$@"; do
    if grep -nP '\(void\)\s*[A-Za-z_]' "$file" >/dev/null; then
        echo "no-void-cast: $file — use [[maybe_unused]] or a bare call instead of '(void)x':"
        grep -nP '\(void\)\s*[A-Za-z_]' "$file"
        status=1
    fi
done
exit "$status"
