#!/usr/bin/env python3
"""RPC check for omp 5b4fb78a55: switching models keeps the session's thinking level, clamped to the new model's
effort ladder; a model's thinking.defaultLevel applies only when no level is set (the previous model had no
thinking).

  thinking_carry_rpc.py <omc> <agent dir> <mock port>

Prints one line per step, then "thinking carry ok"; exits non-zero on the first failure."""
import json, os, subprocess, sys, threading, time

omc, agent, port = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(agent, exist_ok=True)
with open(os.path.join(agent, "models.yml"), "w") as f:
    f.write("""providers:
  mock:
    baseUrl: http://127.0.0.1:%s/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: think-a
        contextWindow: 100000
        maxTokens: 4096
        reasoning: true
        thinking:
          mode: effort
          efforts: [low, medium, high, xhigh]
      - id: think-b
        contextWindow: 100000
        maxTokens: 4096
        reasoning: true
        thinking:
          mode: effort
          efforts: [low, medium, high]
          defaultLevel: low
      - id: plain
        contextWindow: 100000
        maxTokens: 4096
        reasoning: false
      - id: think-c
        contextWindow: 100000
        maxTokens: 4096
        reasoning: true
        thinking:
          mode: effort
          efforts: [low, medium, high]
          defaultLevel: medium
""" % port)
env = dict(os.environ, OMC_CODING_AGENT_DIR=agent)
p = subprocess.Popen([omc, "--model", "mock/think-a", "--thinking", "xhigh", "--mode", "rpc"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
frames = []
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            frames.append(m)


threading.Thread(target=reader, daemon=True).start()


def call(o, n=[0]):
    n[0] += 1
    o["id"] = "r%d" % n[0]
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()
    end = time.time() + 15
    while time.time() < end:
        with lock:
            for f in frames:
                if f.get("type") == "response" and f.get("id") == o["id"]:
                    return f
        time.sleep(0.05)
    print("FAIL no response to", o)
    p.kill()
    sys.exit(1)


def level():
    return (call({"type": "get_state"}).get("data") or {}).get("thinkingLevel")


steps = [(None, "xhigh"), ("think-b", "high"), ("think-a", "high"), ("plain", None), ("think-c", "medium")]
for model, want in steps:
    if model:
        r = call({"type": "set_model", "provider": "mock", "modelId": model})
        if not r.get("success"):
            print("FAIL set_model", model, r)
            p.kill()
            sys.exit(1)
    got = level()
    print(model or "start", got)
    if got != want:
        print("FAIL expected", want)
        p.kill()
        sys.exit(1)
p.stdin.close()
p.wait(timeout=10)
print("thinking carry ok")
