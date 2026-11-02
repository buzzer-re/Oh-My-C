#!/bin/sh
# Scripted stand-in for $VISUAL/$EDITOR (Ctrl+G external editor checks): logs the temp file's extension and
# content to $FAKE_EDITOR_LOG, then appends a line to the file. FAKE_EDITOR_EXIT makes it exit non-zero.
f="$1"
case "$f" in
    *.omp.md) ext=.omp.md ;;
    *.todo.md) ext=.todo.md ;;
    *) ext=".${f##*.}" ;;
esac
if [ -n "${FAKE_EDITOR_LOG:-}" ]; then
    { printf 'ext=%s\n' "$ext"; cat "$f"; printf '\n--\n'; } >> "$FAKE_EDITOR_LOG"
fi
body=$(cat "$f")
printf '%s\nedited by fake editor\n' "$body" > "$f"
exit "${FAKE_EDITOR_EXIT:-0}"
