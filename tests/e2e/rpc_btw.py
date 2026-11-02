#!/usr/bin/env python3
"""RPC /btw e2e (omp rpc-btw.ts protocol): btw -> {record} response, btw_delta / btw_record frames, follow-ups by
recordId, btw_cancel, get_btw_history, and the on-disk sidecar (<session>/btw-history/entry-<id>.json).
Prints "btw ok" when every check passes, else the first failure."""
import glob, json, os, subprocess, sys, threading, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
frames, lock = [], threading.Lock()


def reader():
    for line in p.stdout:
        with lock:
            frames.append(json.loads(line))


threading.Thread(target=reader, daemon=True).start()


def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()


def wait_for(pred, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for f in frames:
                if pred(f):
                    return f
        time.sleep(0.05)
    return None


def response(id_):
    return wait_for(lambda f: f.get("type") == "response" and f.get("id") == id_)


def fail(msg):
    print("FAIL", msg)
    p.kill()
    sys.exit(1)


send({"id": "p", "type": "prompt", "message": "say:hello rpc"})
if not wait_for(lambda f: f.get("type") == "prompt_result"):
    fail("prompt did not finish")

send({"id": "b1", "type": "btw", "question": "  what now  "})
r = response("b1")
if not r or not r["success"]:
    fail(f"btw response {r}")
rec = r["data"]["record"]
if rec["status"] != "running" or rec["question"] != "what now" or rec["answer"] != "" or "title" in rec:
    fail(f"btw record {rec}")
keys = list(rec)
if keys != ["question", "answer", "status", "createdAt", "updatedAt", "id", "leafId"]:
    fail(f"record keys {keys}")
rid = rec["id"]
done = wait_for(lambda f: f.get("type") == "btw_record" and f["record"]["id"] == rid and f["record"]["status"] == "complete")
if not done:
    fail("no complete btw_record")
deltas = "".join(f["delta"] for f in frames if f.get("type") == "btw_delta" and f.get("recordId") == rid)
if done["record"]["answer"] != "**Side answer** for `what now`:\n- first point\n- second point" or not deltas.startswith("**Side answer**"):
    fail(f"answer {done['record']['answer']!r} deltas {deltas!r}")

# follow-up in the same topic
send({"id": "b2", "type": "btw", "question": "and then", "recordId": rid})
r = response("b2")
if not r or not r["success"] or len(r["data"]["record"].get("followUps", [])) != 1:
    fail(f"follow-up {r}")
if not wait_for(lambda f: f.get("type") == "btw_record" and f["record"]["id"] == rid and f["record"].get("followUps", [{}])[-1].get("status") == "complete"):
    fail("follow-up did not complete")

# busy / unknown topic / empty question
send({"id": "b3", "type": "btw", "question": "slow:3:taking a while"})
r = response("b3")
if not r or not r["success"]:
    fail(f"slow btw {r}")
slow_id = r["data"]["record"]["id"]
send({"id": "b4", "type": "btw", "question": "another"})
r = response("b4")
if not r or r["success"] or "still running" not in r["error"]:
    fail(f"busy btw {r}")
send({"id": "c1", "type": "btw_cancel", "recordId": "nope"})
r = response("c1")
if not r or r["data"] != {"cancelled": False}:
    fail(f"cancel other {r}")
send({"id": "c2", "type": "btw_cancel"})
r = response("c2")
if not r or r["data"] != {"cancelled": True}:
    fail(f"cancel {r}")
if not wait_for(lambda f: f.get("type") == "btw_record" and f["record"]["id"] == slow_id and f["record"]["status"] == "cancelled"):
    fail("no cancelled btw_record")
send({"id": "b5", "type": "btw", "question": "x", "recordId": "missing"})
r = response("b5")
if not r or r["success"] or r["error"] != "Unknown /btw topic: missing":
    fail(f"unknown topic {r}")
send({"id": "b6", "type": "btw", "question": "   "})
r = response("b6")
if not r or r["success"] or r["error"] != "btw requires a non-empty question":
    fail(f"empty question {r}")

send({"id": "h", "type": "get_btw_history"})
r = response("h")
recs = r and r["success"] and r["data"]["records"]
if not recs or [x["id"] for x in recs] != [slow_id, rid] or recs[1]["followUps"][0]["question"] != "and then":
    fail(f"history {r}")

# the sidecar on disk
files = glob.glob(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "sessions", "*", "*", "btw-history", "entry-*.json"))
names = sorted(os.path.basename(f) for f in files)
if f"entry-{rid}.json" not in names or f"entry-{slow_id}.json" not in names:
    fail(f"sidecar files {names}")
disk = json.load(open([f for f in files if rid in f][0]))
if disk["status"] != "complete" or disk["followUps"][0]["status"] != "complete":
    fail(f"sidecar record {disk}")
p.stdin.close()
p.wait(timeout=10)
print("btw ok")
