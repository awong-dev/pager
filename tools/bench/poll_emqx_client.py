"""One-shot check of a device's presence on the EMQX clients API. Prints a
single line: time, connected true/false/absent, and a couple of useful
fields if present. Credentials come from Secret Manager into this process's
environment only; nothing is printed or written beyond the summary line."""
import os, subprocess, sys, time
import httpx

def secret(name):
    return subprocess.run(["gcloud","secrets","versions","access","latest","--secret",name,"--project","kid-pager"],
                          capture_output=True, text=True, check=True).stdout.strip()

BASE = "https://s1289801.ala.us-east-1.emqxsl.com:8443/api/v5"
key = secret("BROKER_API_KEY")
sec = secret("BROKER_API_SECRET")
clientid = sys.argv[1] if len(sys.argv) > 1 else "test-pager"
try:
    resp = httpx.get(f"{BASE}/clients/{clientid}", auth=(key, sec), timeout=10)
except httpx.HTTPError as e:
    print(time.strftime("%H:%M:%S"), "REQUEST-FAILED", repr(e))
    sys.exit(1)
ts = time.strftime("%H:%M:%S")
if resp.status_code == 404:
    print(ts, "ABSENT (404)")
elif resp.status_code == 200:
    d = resp.json()
    print(ts, "PRESENT connected=%s connected_at=%s keepalive=%s" % (
        d.get("connected"), d.get("connected_at"), d.get("keepalive")))
else:
    print(ts, "STATUS", resp.status_code, resp.text[:200])
