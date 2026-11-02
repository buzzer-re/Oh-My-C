#!/usr/bin/env python3
"""RPC event filter for the queue e2e checks: user message_start texts in order, then the assistant message count."""
import json, sys

users, assistants = [], 0
for line in sys.stdin:
    try:
        m = json.loads(line)
    except ValueError:
        continue
    msg = m.get("message") or {}
    if m.get("type") == "message_start" and msg.get("role") == "user":
        c = msg.get("content")
        users.append(c if isinstance(c, str) else "".join(b.get("text", "") for b in c if b.get("type") == "text"))
    if m.get("type") == "message_end" and msg.get("role") == "assistant":
        assistants += 1
print(",".join(users), "/", assistants)
