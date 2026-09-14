#!/bin/bash
# Managed by Terraform (infra/modules/broker-gce) -- do not edit on the
# instance; edit this template and `terraform apply` instead.
#
# Brings up the identical EMQX rule-engine shape relay/docker-compose.yml +
# tools/emqx_setup.py already prove locally (same pinned image, same
# connector/action/rule for pager/+/{up,status,loc} -> POST /webhooks/mqtt),
# plus TLS via certbot for the MQTT listener -- docs/SERVER_PLAN.md §9.5(a).
set -euo pipefail
exec > >(tee /var/log/pager-broker-startup.log) 2>&1

echo "=== pager broker-gce startup: $(date -u) ==="

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y docker.io certbot curl jq

systemctl enable --now docker

# --- Fetch secrets from Secret Manager via the instance's own service
# account (metadata-server ADC token) -- never baked into this template. ---
fetch_token() {
  curl -sf -H "Metadata-Flavor: Google" \
    "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token" \
    | jq -r .access_token
}

fetch_secret() {
  # $1 = secret_id
  local token
  token="$(fetch_token)"
  curl -sf -H "Authorization: Bearer $${token}" \
    "https://secretmanager.googleapis.com/v1/projects/${project_id}/secrets/$1/versions/latest:access" \
    | jq -r '.payload.data' | base64 -d
}

BROKER_API_KEY="$(fetch_secret "${broker_api_key_secret_id}")"
BROKER_API_SECRET="$(fetch_secret "${broker_api_secret_secret_id}")"
WEBHOOK_KEY="$(fetch_secret "${webhook_key_secret_id}")"

if [ -z "$${BROKER_API_KEY}" ] || [ -z "$${BROKER_API_SECRET}" ] || [ -z "$${WEBHOOK_KEY}" ]; then
  echo "FATAL: one or more secrets came back empty -- has infra/README.md's" \
       "'gcloud secrets versions add' step been done yet? Aborting so this" \
       "doesn't silently come up with a broken/mismatched credential." >&2
  exit 1
fi

mkdir -p /opt/pager-broker
# Same one-line "key:secret" shape as relay/emqx/bootstrap_api_keys.txt, so
# the relay's own BROKER_API_KEY/BROKER_API_SECRET (same Secret Manager
# values, wired into Cloud Run by infra/modules/relay-service) authenticate
# against this broker's REST publish API with no separate provisioning step.
printf '%s:%s\n' "$${BROKER_API_KEY}" "$${BROKER_API_SECRET}" > /opt/pager-broker/bootstrap_api_keys.txt
chmod 600 /opt/pager-broker/bootstrap_api_keys.txt

# --- TLS cert (Let's Encrypt, standalone HTTP-01 on :80) -----------------
# Requires ${broker_domain} to already resolve here -- see this module's
# variables.tf comment. Non-fatal on failure: EMQX still comes up on plain
# MQTT (1883) so the instance isn't a total loss while DNS/cert issues are
# sorted out by hand, but PROTOCOL.md's TLS-on-8883 expectation is unmet
# until this succeeds (rerun `certbot certonly --standalone` by hand later).
CERT_DIR="/etc/letsencrypt/live/${broker_domain}"
if [ ! -d "$${CERT_DIR}" ]; then
  systemctl stop docker || true
  certbot certonly --standalone --non-interactive --agree-tos \
    --email "${letsencrypt_email}" -d "${broker_domain}" || \
    echo "WARNING: certbot failed -- continuing without TLS, see comment above." >&2
  systemctl start docker
fi

EMQX_ENV_ARGS=(-e "EMQX_API_KEY__BOOTSTRAP_FILE=/opt/emqx/data/bootstrap_api_keys.txt")
if [ -d "$${CERT_DIR}" ]; then
  EMQX_ENV_ARGS+=(
    -v "/etc/letsencrypt:/etc/letsencrypt:ro"
    -e "EMQX_LISTENERS__SSL__DEFAULT__SSL_OPTIONS__CERTFILE=$${CERT_DIR}/fullchain.pem"
    -e "EMQX_LISTENERS__SSL__DEFAULT__SSL_OPTIONS__KEYFILE=$${CERT_DIR}/privkey.pem"
    -e "EMQX_LISTENERS__SSL__DEFAULT__BIND=0.0.0.0:8883"
  )
  # certbot's systemd timer renews the cert on this VM automatically;
  # EMQX needs a restart to pick up a renewed file (renewal is infrequent
  # enough -- every ~60 days -- that a short reconnect blip is acceptable
  # for a household deployment; devices reconnect per PROTOCOL.md §5.3).
  cat > /etc/cron.d/pager-broker-cert-renew <<'CRON'
15 3 * * * root docker restart pager-emqx >/dev/null 2>&1
CRON
fi

docker rm -f pager-emqx >/dev/null 2>&1 || true
docker run -d --name pager-emqx --restart unless-stopped \
  -p 1883:1883 -p 8883:8883 -p 18083:18083 \
  -v /opt/pager-broker/bootstrap_api_keys.txt:/opt/emqx/data/bootstrap_api_keys.txt:ro \
  "$${EMQX_ENV_ARGS[@]}" \
  "${emqx_image}"

# --- Wait for EMQX, then provision the rule engine ------------------------
# Deliberately the *same* rule id / connector name / action name / topic
# list / SQL / HTTP action shape as tools/emqx_setup.py, just re-implemented
# in curl (that script needs a Python interpreter on the box, and pulling
# the repo onto this VM is more moving parts than re-stating four REST
# calls that must stay in lockstep with that file's constants anyway --
# CONNECTOR_NAME=relay_webhook, ACTION_NAME=relay_webhook_action,
# RULE_ID=pager_to_relay, TOPICS=pager/+/up,status,loc). If you change
# tools/emqx_setup.py's shape, mirror the change here too.
for i in $(seq 1 60); do
  curl -sf "http://localhost:18083/api/v5/status" >/dev/null 2>&1 && break
  sleep 2
done

EMQX_TOKEN="$(curl -sf -X POST "http://localhost:18083/api/v5/login" \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"public"}' | jq -r .token)"

curl -sf -X POST "http://localhost:18083/api/v5/connectors" \
  -H "Authorization: Bearer $${EMQX_TOKEN}" -H "Content-Type: application/json" \
  -d "$(jq -n --arg url "${relay_service_url}" \
    '{type:"http", name:"relay_webhook", url:$url, enable:true}')" \
  || echo "connector upsert: already exists or failed, continuing"

curl -sf -X POST "http://localhost:18083/api/v5/actions" \
  -H "Authorization: Bearer $${EMQX_TOKEN}" -H "Content-Type: application/json" \
  -d "$(jq -n --arg key "$${WEBHOOK_KEY}" '{
    type: "http", name: "relay_webhook_action", connector: "relay_webhook", enable: true,
    parameters: {
      path: "/webhooks/mqtt", method: "post",
      headers: {"X-Relay-Webhook-Key": $key, "content-type": "application/json"},
      body: "$${.}"
    },
    resource_opts: {query_mode: "sync", request_ttl: "10s"}
  }')" \
  || echo "action upsert: already exists or failed, continuing"

curl -sf -X POST "http://localhost:18083/api/v5/rules" \
  -H "Authorization: Bearer $${EMQX_TOKEN}" -H "Content-Type: application/json" \
  -d '{
    "id": "pager_to_relay",
    "sql": "SELECT topic, payload, qos, clientid FROM \"pager/+/up\", \"pager/+/status\", \"pager/+/loc\"",
    "actions": ["http:relay_webhook_action"],
    "enable": true
  }' \
  || echo "rule upsert: already exists or failed, continuing"

echo "=== pager broker-gce startup complete: $(date -u) ==="
echo "NOTE: device-level MQTT credentials/ACLs (PROTOCOL.md §2) are still" \
     "not provisioned by this script -- that gap is tracked in BUILD_LOG.md" \
     "Phase 3 and closed by the admin API, the same as for EMQX Cloud" \
     "Serverless. See infra/README.md."
