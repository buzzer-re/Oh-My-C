#!/usr/bin/env python3
"""RPC mode exits 1 once stdout can no longer take a frame (omp rpc-mode.ts RpcOutputWriter onFailure: the session is
disposed, then process.exit(1)), even while stdin stays open. usage: rpc_output_fail.py <omc>
Prints "rpc output fail ok" or what went wrong."""
import json, os, subprocess, sys, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc", "--no-session"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
first = p.stdout.readline()  # the ready frame went out
p.stdout.close()             # the client goes away: further frames hit a broken pipe
try:
    p.stdin.write((json.dumps({"id": "1", "type": "get_state"}) + "\n").encode())
    p.stdin.flush()
except BrokenPipeError:
    pass
try:
    rc = p.wait(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()
    print("FAIL: still running 10 s after its output broke (first frame %r)" % first[:80])
    sys.exit(1)
print("rpc output fail ok" if rc == 1 else "FAIL: exit code %r" % rc)
