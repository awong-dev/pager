"""Poll the EMQX clients API for one device once a minute for N minutes,
printing a timestamped PRESENT/ABSENT line each time (flushed immediately so
a tee'd/redirected log is readable while the loop runs). Credentials come
from Secret Manager into this process's environment only; nothing beyond the
summary lines is printed or written."""
import os, subprocess, sys, time
import httpx

def secret(name):
    return subprocess.run(["gcloud","secrets","versions","access","latest","--secret",name,"--project","kid-pager"],
                          capture_output=True, text=True, check=True).stdout.strip()

BASE = "https://s1289801.ala.us-east-1.emqxsl.com:8443/api/v5"
key = secret("BROKER_API_KEY")
sec = secret("BROKER_API_SECRET")
clientid = sys.argv[1] if len(sys.argv) > 1 else "test-pager"
minutes = int(sys.argv[2]) if len(sys.argv) > 2 else 60
interval = 60
n = (minutes * 60) // interval
for i in range(n):
    ts = time.strftime("%H:%M:%S")
    try:
        resp = httpx.get(f"{BASE}/clients/{clientid}", auth=(key, sec), timeout=10)
        if resp.status_code == 404:
            print(ts, "ABSENT (404)", flush=True)
        elif resp.status_code == 200:
            d = resp.json()
            print(ts, "PRESENT connected=%s connected_at=%s" % (d.get("connected"), d.get("connected_at")), flush=True)
        else:
            print(ts, "STATUS", resp.status_code, resp.text[:150], flush=True)
    except Exception as e:
        print(ts, "REQUEST-FAILED", repr(e), flush=True)
    time.sleep(interval)
print("poll loop done", flush=True)
