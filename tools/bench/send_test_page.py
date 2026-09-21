"""Send one test page to a pager through the relay's own publish path (signing,
counter, encoding), run locally against production. Owner-authorised for
test-pager. Credentials are read from Secret Manager into this process's
environment only; nothing is printed or written."""
import os, subprocess, sys, time
def secret(name):
    return subprocess.run(["gcloud","secrets","versions","access","latest","--secret",name,"--project","kid-pager"],
                          capture_output=True, text=True, check=True).stdout.strip()
os.environ["GOOGLE_CLOUD_PROJECT"]="kid-pager"
os.environ["BROKER_API_URL"]="https://s1289801.ala.us-east-1.emqxsl.com:8443/api/v5"
os.environ["BROKER_API_KEY"]=secret("BROKER_API_KEY")
os.environ["BROKER_API_SECRET"]=secret("BROKER_API_SECRET")
os.environ.setdefault("WEBHOOK_KEY","unused-here")
for k in ("FIRESTORE_EMULATOR_HOST","FIREBASE_AUTH_EMULATOR_HOST","DEV_MODE"):
    os.environ.pop(k, None)
sys.path.insert(0, "/Users/albert/src/pager/relay")
from app.config import Settings
from app.broker import BrokerClient
from app.ids import new_message_id
device, body = sys.argv[1], sys.argv[2]
obj={"v":1,"id":new_message_id(),"ts":int(time.time()),"from":"claude","body":body,"ack":None}
ok=BrokerClient(Settings.from_env()).publish_down(device, obj)
print(time.strftime("%H:%M:%S", time.gmtime()), "sent" if ok else "FAILED", obj["id"], repr(body))
