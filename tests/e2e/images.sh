# Sourced by tests/e2e/run.sh: the model image pipeline end to end (read tool images normalized for the model,
# undecodable images rejected at ingress, the provider boundary's Anthropic per-image clamp, session blobs).
IPORT=$((PORT + 11))
MOCK_LOG="$T/img-prov.jsonl" python3 -I "$ROOT/tests/e2e/mock_providers.py" $IPORT & IPID=$!
for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$IPORT) 2>/dev/null && break; sleep 0.1; done
mkdir -p "$T/iagent" "$T/iwork"
cat > "$T/iagent/models.yml" <<YML
providers:
  mock:
    baseUrl: http://127.0.0.1:$PORT/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: vision-1
        input: [text, image]
        contextWindow: 100000
        maxTokens: 4096
  mockant:
    baseUrl: http://127.0.0.1:$IPORT
    apiKey: MOCK_KEY
    api: anthropic-messages
    models:
      - id: claude-mock
        input: [text, image]
        contextWindow: 200000
        maxTokens: 4096
YML
python3 -I - "$T/iwork" <<'PY'
import os, struct, sys, zlib
d = sys.argv[1]
def png(path, w, h, row):
    raw = b"".join(b"\x00" + row(y) for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           ch(b"IDAT", zlib.compress(raw, 1)) + ch(b"IEND", b""))
# a 3200x1800 UI-like screenshot (flat bands + noise strips) and a 2600x1500 noise image (> 10 MB base64)
png(os.path.join(d, "shot.png"), 3200, 1800, lambda y: bytes((x * 7 + y) & 255 if (y // 40) % 3 else 40 for x in range(3200 * 3)))
png(os.path.join(d, "noise.png"), 2600, 1500, lambda y: os.urandom(2600 * 3))
full = open(os.path.join(d, "shot.png"), "rb").read()
open(os.path.join(d, "broken.png"), "wb").write(full[: len(full) // 2])
open(os.path.join(d, "fake.png"), "wb").write(b"plain text in a .png\n")
PY
IENV=(env OMC_CODING_AGENT_DIR="$T/iagent" OMC_NO_TITLE=1)
icheck() { # name want cmd...  (runs in the image work dir)
    local name=$1 want=$2; shift 2
    local out; out=$(cd "$T/iwork" && timeout 60 "$@" </dev/null 2>&1)
    if [[ "$out" == *"$want"* ]]; then echo "   ok  $name"; else echo "   FAIL $name: got: ${out:0:400}"; fail=1; fi
}
# the checks that decode what omc sent need Pillow; without it (CI runners) they are skipped, not failed
if python3 -I -c 'import PIL' 2>/dev/null; then picheck() { icheck "$@"; }; else picheck() { echo "   skip $1 (no Pillow)"; }; fi
: > "$T/req.jsonl"
icheck "img read"          "plain answer"  "${IENV[@]}" "$B" --model mock/vision-1 --no-session -p "read:shot.png"
# the tool text and the image the model receives: decodable, 1568x882, within omp's 500 KiB budget
icheck "img read note"     "Read image file [image/png] (displayed as image/" python3 -I -c '
import json, sys
r = [x for x in (json.loads(l) for l in open(sys.argv[1])) if x.get("tools")][-1]
print([m["content"] for m in r["messages"] if m["role"] == "tool"][-1])' "$T/req.jsonl"
icheck "img read dims"     "[Image: original 3200x1800, displayed at 1568x882. Multiply coordinates by 2.04 to map to original image.]" \
    python3 -I -c 'import json, sys; r = [x for x in (json.loads(l) for l in open(sys.argv[1])) if x.get("tools")][-1]; print([m["content"] for m in r["messages"] if m["role"] == "tool"][-1])' "$T/req.jsonl"
picheck "img wire"          "1568x882 ok" python3 -I -c '
import json, sys, base64, io
from PIL import Image
r = [x for x in (json.loads(l) for l in open(sys.argv[1])) if x.get("tools")][-1]
u = [p["image_url"]["url"] for m in r["messages"] for p in (m["content"] if isinstance(m["content"], list) else []) if p.get("type") == "image_url"][-1]
raw = base64.b64decode(u.split(",", 1)[1]); im = Image.open(io.BytesIO(raw)); im.load()
print("%dx%d %s" % (im.size[0], im.size[1], "ok" if len(raw) <= 500 * 1024 else "too big"))' "$T/req.jsonl"
icheck "img undecodable"   "broken.png is not a decodable image/png image: Image: decode failed"  "${IENV[@]}" "$B" --model mock/vision-1 --no-session -p "read:broken.png"
icheck "img magic only"    "1:plain text in a .png"  "${IENV[@]}" "$B" --model mock/vision-1 --no-session -p "read:fake.png"
icheck "img text model"    "Image metadata:"  env OMC_CODING_AGENT_DIR="$T/agent" OMC_NO_TITLE=1 "$B" --model mock/mock-1 --no-session -p "read:$T/iwork/shot.png"
# images.autoResize off: the read result carries the raw 2600x1500 noise PNG (> 10 MB base64); the Anthropic
# provider clamps it under 90% of the 10 MiB per-image cap before the request leaves (prepareAnthropicImageContext)
printf 'images:\n  autoResize: false\n' > "$T/iagent/config.yml"
icheck "img anthropic"     "Tool said: Read image file [image/png]"  "${IENV[@]}" "$B" --model mockant/claude-mock --no-session -p "read:noise.png"
picheck "img anthropic cap" "clamped ok" python3 -I -c '
import json, sys, base64, io
from PIL import Image
rows = [json.loads(l) for l in open(sys.argv[1])]
b = [r["body"] for r in rows if "messages" in r.get("body", {})][-1]
imgs = [c["source"] for m in b["messages"] for p in m["content"] if isinstance(m["content"], list) and p.get("type") == "tool_result"
        for c in (p.get("content") or []) if c.get("type") == "image"]
s = imgs[-1]; n = len(s["data"]); im = Image.open(io.BytesIO(base64.b64decode(s["data"]))); im.load()
print("clamped ok" if n <= int(10 * 1024 * 1024 * 0.9) and max(im.size) <= 8000 else "too big %d" % n, im.size, s["media_type"])' "$T/img-prov.jsonl"
rm -f "$T/iagent/config.yml"
# a persisted session keeps images in the blob store and resolves them on resume
icheck "img blob persist"  "blob:sha256:"  bash -c "OMC_CODING_AGENT_DIR='$T/iagent' OMC_NO_TITLE=1 '$B' --model mock/vision-1 -p 'read:shot.png' >/dev/null; grep -ho 'blob:sha256:[0-9a-f]*' '$T'/iagent/sessions/*/*.jsonl | head -1"
icheck "img blob resume"   "image_url ok" bash -c ": > '$T/req.jsonl'; OMC_CODING_AGENT_DIR='$T/iagent' OMC_NO_TITLE=1 '$B' --model mock/vision-1 -c -p 'hi' >/dev/null; grep -q 'data:image/[a-z]*;base64,' '$T/req.jsonl' && echo 'image_url ok'"
kill $IPID 2>/dev/null
