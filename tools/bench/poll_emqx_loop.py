"""r2: extended EMQX client poll for the keepalive investigation (Test A).
Polls the EMQX clients API every 30 s for N minutes, logging the fields that
matter for diagnosing whether the modem's PINGREQ is reaching the broker:
connected, connected_at, keepalive, recv_pkt, send_pkt, recv_cnt, recv_msg,
recv_oct, send_oct. Credentials come from Secret Manager into this process's
environment only; nothing beyond the summary lines is printed or written."""
import os, subprocess, sys, time
import httpx

def secret(name):
    return subprocess.run(["gcloud","secrets","versions","access","latest","--secret",name,"--project","kid-pager"],
                          capture_output=True, text=True, check=True).stdout.strip()

BASE = "https://s1289801.ala.us-east-1.emqxsl.com:8443/api/v5"
key = secret("BROKER_API_KEY")
sec = secret("BROKER_API_SECRET")
clientid = sys.argv[1] if len(sys.argv) > 1 else "test-pager"
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 40
interval = 30
n = int((minutes * 60) // interval)
for i in range(n):
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")
    try:
        resp = httpx.get(f"{BASE}/clients/{clientid}", auth=(key, sec), timeout=10)
        if resp.status_code == 404:
            print(ts, "ABSENT (404)", flush=True)
        elif resp.status_code == 200:
            d = resp.json()
            print(ts,
                  "PRESENT connected=%s connected_at=%s keepalive=%s recv_pkt=%s send_pkt=%s recv_cnt=%s send_cnt=%s recv_msg=%s recv_oct=%s send_oct=%s" % (
                      d.get("connected"), d.get("connected_at"), d.get("keepalive"),
                      d.get("recv_pkt"), d.get("send_pkt"), d.get("recv_cnt"), d.get("send_cnt"),
                      d.get("recv_msg"), d.get("recv_oct"), d.get("send_oct")),
                  flush=True)
        else:
            print(ts, "STATUS", resp.status_code, resp.text[:150], flush=True)
    except Exception as e:
        print(ts, "REQUEST-FAILED", repr(e), flush=True)
    time.sleep(interval)
print("poll loop done", flush=True)
