#!/usr/bin/env python3
"""Dev-only: compare the policy of models.yml custom models between omp and omc: both load the same models.yml (throwaway
HOME and agent dirs, nothing is requested from a model) and answer RPC get_available_models; every field of each custom
model is diffed (compat, thinking, identity, input, limits, cost, tokenizer, ...).
  custom_model_diff.py [--sample N] [--seed S] [--keep] [--show M]
The fixed matrix below covers local backends, hosts and API records; --sample adds N ids from omc's bundled catalog
(also mutated with proxy spellings) under a loopback and a remote custom provider, which exercises the taxonomy, the
rule cascade and the reference lookup at scale (keep N at or below 400: past that the answer exceeds the RPC transport
limit in both apps). --show prints up to M differing models (default 40).
Env: OMP_BIN (default omp), OMC_BIN (default build/omc), TMPDIR."""
import argparse, json, os, random, shutil, subprocess, sys, tempfile, time, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ap = argparse.ArgumentParser()
ap.add_argument("--sample", type=int, default=0)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--keep", action="store_true")
ap.add_argument("--show", type=int, default=40)
args = ap.parse_args()

Q = "http://127.0.0.1:9/v1"
FIXED = {
    "splash": (Q, "openai-completions", [
        {"id": "qwen3.6-27b", "reasoning": True, "input": ["text", "image"], "contextWindow": 131072, "maxTokens": 32768,
         "thinking": {"mode": "effort", "minLevel": "low", "maxLevel": "high"}},
        {"id": "gemma-3-27b-it"}, {"id": "my-model", "reasoning": True}, {"id": "deepseek-r1-distill-qwen-32b", "reasoning": True},
        {"id": "deepseek-v3.2", "reasoning": True}, {"id": "glm-4.6", "reasoning": True}, {"id": "kimi-k2.6", "reasoning": True},
        {"id": "gpt-oss-120b", "reasoning": True}, {"id": "Qwen/Qwen3.8-27B", "reasoning": True},
        {"id": "qwen3.6-27b-compat", "reasoning": True, "compat": {"supportsStore": True, "thinkingFormat": "openai", "junk": 1}},
        {"id": "unsloth/Qwen3.6-27B-GGUF:Q4_K_M", "reasoning": True}, {"id": "bonsai-27b"}, {"id": "bonsai-2-27b-Q8"},
        {"id": "claude-opus-4-8"}, {"id": "[Kiro] claude-opus-4-8"}, {"id": "gpt-5.4:cloud"}, {"id": "vendor/claude-sonnet-4-6-thinking"},
        {"id": "@modal/GLM-5-2-FP8"}, {"id": "gemini-3.1-pro-preview"}, {"id": "named", "name": "Acme: Fancy (latest)", "reasoning": True},
        {"id": "priced", "cost": {"input": 1, "output": 2, "cacheRead": 0, "cacheWrite": 0}, "contextWindow": 5000, "maxTokens": 100},
        {"id": "thinkless", "thinking": {"mode": "effort", "efforts": ["low"]}},
        {"id": "wt", "reasoning": True, "compat": {"whenThinking": {"supportsStore": False}}},
    ]),
    "lan": ("http://192.168.1.20:8080/v1", "openai-completions", [{"id": "qwen3-coder-30b", "reasoning": True}]),
    "remote": ("https://api.example.com/v1", "openai-completions", [{"id": "qwen3.6-27b", "reasoning": True}, {"id": "mistral-small"}, {"id": "grok-4.5"}]),
    "llama.cpp": (Q, "openai-completions", [{"id": "qwen3.6-27b", "reasoning": True}, {"id": "qwen3.8-27b", "reasoning": True}, {"id": "other"}]),
    "vllm": ("http://10.0.0.5:8000/v1", "openai-completions", [{"id": "Qwen/Qwen3.6-27B", "reasoning": True}]),
    "lm-studio": ("http://127.0.0.1:1234/v1", "openai-completions", [{"id": "qwen/qwen3.8-27b", "reasoning": True}]),
    "resp": ("https://proxy.example.com/v1", "openai-responses", [{"id": "gpt-5.6-terra", "reasoning": True}, {"id": "my-resp", "reasoning": True}]),
    "resploc": (Q, "openai-responses", [{"id": "local-resp", "reasoning": True}]),
    "ant": ("https://claude-proxy.example.com", "anthropic-messages", [{"id": "claude-opus-4-8"}, {"id": "claude-sonnet-5", "reasoning": True}, {"id": "not-claude", "reasoning": True}]),
    "antcf": ("https://gateway.ai.cloudflare.com/v1/a/b/anthropic", "anthropic-messages", [{"id": "claude-haiku-4-5"}]),
    "gog": ("https://generativelanguage.googleapis.com/v1beta", "google-generative-ai", [{"id": "gemini-3.1-pro-preview"}, {"id": "my-gem", "reasoning": True}]),
    "bed": ("https://bedrock-runtime.us-east-1.amazonaws.com", "bedrock-converse-stream", [{"id": "anthropic.claude-sonnet-5-v1:0"}, {"id": "my-bed", "reasoning": True}]),
    "orx": ("https://openrouter.ai/api/v1", "openai-completions", [{"id": "qwen/qwen3.6-27b", "reasoning": True}, {"id": "anthropic/claude-opus-4-8"}]),
    "provcompat": (Q, "openai-completions", [{"id": "pc", "compat": {"supportsStore": False}}]),
}


def bundled_ids():
    d = json.loads(zlib.decompress(open(os.path.join(ROOT, "assets/models.json.z"), "rb").read()))
    return sorted({m["id"] for ms in d["providers"].values() for m in ms.values()})


def build():
    provs = {k: {"baseUrl": v[0], "api": v[1], "apiKey": "MOCK_KEY", "models": [dict(m) for m in v[2]]} for k, v in FIXED.items()}
    provs["provcompat"]["compat"] = {"supportsDeveloperRole": True, "reasoningEffortMap": {"low": "minimal"}}
    provs["provcompat"]["disableStrictTools"] = True
    if args.sample:
        rnd = random.Random(args.seed)
        ids = rnd.sample(bundled_ids(), args.sample)
        loop, far = [], []
        for i, mid in enumerate(ids):
            spelled = [mid, f"[Tag] {mid}", f"vendor/{mid}", f"{mid}:cloud", f"{mid}-free", mid.upper(), f"{mid}-thinking", f"{mid}-high"][i % 8]
            (loop if i % 2 else far).append({"id": spelled, **({"reasoning": True} if i % 3 == 0 else {})})
        provs["sampleloop"] = {"baseUrl": Q, "api": "openai-completions", "apiKey": "MOCK_KEY", "models": loop}
        provs["samplefar"] = {"baseUrl": "https://api.example.com/v1", "api": "openai-completions", "apiKey": "MOCK_KEY", "models": far}
    return {"providers": provs}


def dump(binary, env_var, agent, home, work):
    env = dict(os.environ, HOME=home, MOCK_KEY="x", OMC_NO_LOCAL_DISCOVERY="1")
    env[env_var] = agent
    p = subprocess.Popen([binary, "--mode", "rpc"], cwd=work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    p.stdin.write(json.dumps({"id": "q", "type": "get_available_models"}) + "\n")
    p.stdin.flush()
    t0 = time.time()
    out = None
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("id") == "q":
            out = m.get("data")
            break
        if time.time() - t0 > 60:
            break
    p.kill()
    p.wait()
    ms = out["models"] if isinstance(out, dict) else out
    return ms or []


base = tempfile.mkdtemp(prefix="omc-cmd-")
cfg = build()
provs = set(cfg["providers"])
res = {}
for app, binary, var in (("omp", os.environ.get("OMP_BIN", "omp"), "PI_CODING_AGENT_DIR"),
                         ("omc", os.environ.get("OMC_BIN", os.path.join(ROOT, "build/omc")), "OMC_CODING_AGENT_DIR")):
    agent, home, work = (os.path.join(base, app, d) for d in ("agent", "home", "work"))
    for d in (agent, home, work):
        os.makedirs(d)
    subprocess.run(["git", "init", "-q", work], check=False)
    with open(os.path.join(agent, "models.json"), "w") as f:
        json.dump(cfg, f)
    with open(os.path.join(agent, "config.yml"), "w") as f:
        f.write("modelRoles:\n  default: splash/my-model\nstartup:\n  checkUpdate: false\nsetupVersion: 3\n")
    res[app] = {(m["provider"], m["id"]): m for m in dump(binary, var, agent, home, work) if m["provider"] in provs}

# compatConfig is compared through the resolved compat; int / tps reach omp's custom models only from its online catalog
# discovery (model-registry.ts #captureCatalogMetrics), which omc does not do
IGNORE = {"compatConfig", "int", "tps"}
a, b = res["omp"], res["omc"]
print(f"omp {len(a)} models, omc {len(b)} models")
same = 0
shown = 0
fields = {}
for k, ma in sorted(a.items()):
    mb = b.get(k)
    diffs = []
    if mb is None:
        diffs.append("missing in omc")
    else:
        ca, cb = ma.get("compat") or {}, mb.get("compat") or {}
        for x in sorted(set(ca) | set(cb)):
            if ca.get(x) != cb.get(x):
                diffs.append(f"compat.{x}: {ca.get(x)!r} | {cb.get(x)!r}")
                fields[f"compat.{x}"] = fields.get(f"compat.{x}", 0) + 1
        for x in sorted((set(ma) | set(mb)) - {"compat"} - IGNORE):
            if ma.get(x) != mb.get(x):
                diffs.append(f"{x}: {ma.get(x)!r} | {mb.get(x)!r}")
                fields[x] = fields.get(x, 0) + 1
    if not diffs:
        same += 1
    elif shown < args.show:
        shown += 1
        print(k)
        for d in diffs:
            print("   ", d[:220])
for k in sorted(set(b) - set(a)):
    print(k, "only in omc")
print(f"{same}/{len(a)} identical")
if fields:
    print("differing fields:", dict(sorted(fields.items(), key=lambda kv: -kv[1])))
if args.keep:
    print("kept", base)
else:
    shutil.rmtree(base, ignore_errors=True)
sys.exit(0 if same == len(a) and len(a) == len(b) else 1)
