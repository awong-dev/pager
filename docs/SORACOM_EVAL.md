# Evaluation: Soracom as the LTE provider, with Beam or Funk (2026-09-20)

Status: **evaluation only, nothing decided or built.** Prices are Soracom's published "Global"
USD list prices read on 2026-09-20 from <https://developers.soracom.io/en/docs/reference/fees/>;
re-check before buying. Traffic figures are this project's own, `PROTOCOL.md` §7.

## What the two services are

- **Beam (MQTT entry point).** The pager speaks **plain MQTT to `beam.soracom.io:1883`**. Beam, in
  Soracom's cloud, opens the real **MQTTS** connection to our broker, validates its certificate
  with a normal root store, and can inject the username/password or a client certificate, with
  `#{imsi}` / `#{imei}` placeholders. Publish *and* subscribe both work, so pages still arrive as
  pushes. Version 201912 passes QoS and protocol features through; keepalive must be 0 or 5-1200 s;
  Beam drops the device after 1.5 × keepalive of silence; **Beam keeps no session state**.
- **Funk.** Request/response only: the pager sends HTTP/TCP/UDP to `funk.soracom.io`, Soracom
  invokes a cloud function (Lambda, Azure Functions, Google Cloud Functions) with the SIM's
  identity attached, and returns the function's reply. **There is no server push.**

"Plaintext up to Soracom" means: LTE's own air-interface encryption on the radio leg, then the
carrier's core and the roaming interconnect to Soracom's packet gateway in clear, then Soracom.
It never crosses the public internet, but the visited carrier, the interconnect provider and
Soracom can all read it.

## Cost at this project's traffic

| | Today (Google Fi data SIM, TLS on the pager) | Soracom plan-US + Beam |
|---|---|---|
| Line fee | none | included in the bundle |
| Data, nominal | 1.1-1.7 MB/month | **about 0.5 MB/month** |
| Data, pessimistic | 5.4-7.1 MB/month | about 1.7-3 MB/month |
| Data cost | about $0.02-0.07 at $10/GB | **$0.60** (1 MB bundle) to **$0.76** (3 MB) or **$1.30** (10 MB); overage $0.07-0.09/MB |
| Beam requests | n/a | about 6,000/month nominal, maybe 30,000 pessimistic: **free** (first 100,000/month; then $0.09 per 10,000) |
| SMS from the pager | carrier dependent | $0.10 each, **but only to Soracom's own endpoint** (see below) |
| SMS to the pager | carrier dependent | $0.005 each via Soracom's API |

Why the data drops: the dominant term in §7.3 is the ~5 kB TLS handshake on every reconnect (20 of
38 kB/day nominal, 120 of 179 kB pessimistic). A plain MQTT connect is about 0.4 kB, and every
record loses its 29-byte TLS framing. Beam's 1200 s keepalive ceiling (ours is 1800 s) adds 24
pings a day at about 42 bytes each, which is noise, and PINGREQs are not billed in version 201912.
The handshake saving is also radio time, so it helps the battery more than the bill.

Funk's first 50,000 requests a month are free, then $0.18 per 10,000. Money is not what rules
Funk out (next section).

**Either way this is about a dollar a month. Cost should not drive the decision.**

## Design differences with Beam

What gets simpler:

- **All TLS leaves the pager.** No TLS profiles, no CA slot, no plaintext-fallback hazard (the bug
  that cost the first bring-up), no placeholder certificate, no 5 kB handshakes. The modem's MQTT
  engine has already been seen speaking plaintext MQTT correctly.
- **The whole CA trust plan disappears**: no pinning, no fallback, no broken padlock, no CA push, no
  fetch-by-URL. Server authentication is done by Beam with a real root store, which is *stronger*
  than a pager that falls back to no validation. (The v0.2 work in `V02_DESIGN.md` §4 would be
  dead code on a Beam deployment; keep it for non-Soracom SIMs or drop it.)
- **Credentials can leave the pager too.** Beam can inject the broker username/password keyed on the
  IMSI, so the setup bundle shrinks to the device id and the HMAC key.

What changes in the security model:

- Confidentiality past the radio rests on the carrier, the interconnect and Soracom. Today it
  rests on TLS to EMQX, and EMQX itself already sees everything in clear. So the honest delta is
  "three more parties can read pages and location fixes".
- The **HMAC signature and counter become the only integrity protection** between the pager and
  Soracom. Keep them. (This also settles last night's question about dropping the counter: not on
  a Beam design.)
- The clean fix is `DEVICE_PLAN.md` §2.2's option E: **AEAD-encrypt bodies and location fixes under
  the device key**, pager↔relay. About 28 bytes per message. Then Soracom, the carrier *and* EMQX
  all see ciphertext, which is better confidentiality than today's design, with no certificates on
  the pager at all. **I would make this a condition of moving to Beam.**
- Identity becomes the SIM. A SIM moved into another device gets that pager's MQTT session; the
  HMAC key still stops it forging or reading (with AEAD) anything.

What breaks or needs care:

- **Device-direct SMS to a parent's phone does not work on a Soracom SIM.** Mobile-originated SMS
  can only go to Soracom's own endpoint; texting an ordinary phone number is not supported. The
  feature specified in `V02_DESIGN.md` §6 would have to become "pager → SMS → Soracom → (Beam's
  SMS→HTTP entry point) → relay → Twilio → parent". That still works when the data session is
  down, and the audit log comes for free, but it depends on the relay, which is what the
  direct-SMS design was meant to avoid. Inbound texts from ordinary phones are reported as
  supported; verify.
- **eDRX is the thing to test first.** The pager's latency and battery model assume a granted
  20.48 s eDRX cycle. On Google Fi (T-Mobile) it is granted as requested. On the one AT&T-hosted
  SIM tried so far the network overrode it and registration took about two minutes. Soracom's US
  plans roam on AT&T and T-Mobile; which one a pager lands on, and what it grants to a roaming
  LTE-M device, is unknown until a Soracom SIM is in the pager.
- Keepalive drops to ≤ 1200 s; Soracom also ends an idle data session after about an hour, which
  the keepalive prevents.
- Beam keeps no session state. The design already assumes a clean session and re-publishes unacked
  messages on the online edge, so nothing changes.
- The modem library cannot set an MQTT Last Will today; that is unchanged.
- Lock-in: the pager's firmware would hard-code `beam.soracom.io`. Keep the TLS path buildable so a
  non-Soracom SIM remains an option.

## Funk

Not suitable as the pager's main path. A pager is a downlink device and Funk cannot push. The
workarounds are polling (an RRC connection every few seconds defeats eDRX and the battery budget)
or using a $0.005 MT-SMS as a doorbell for every page (about $3-4 a month at 25 pages a day, plus
SMS latency and a second delivery mechanism to make reliable). It would also mean replacing the
modem's built-in MQTT client, the broker and the webhook with a new request/response protocol on
the ESP32. Funk, or Beam's UDP→HTTPS entry point, could be a cheap uplink for status and location
reports later; that is an optimisation, not an architecture.

## Recommendation

Beam is worth a trial; Funk is not. Order of work if pursued:

1. Buy one plan-US SIM. With the **existing** firmware (TLS straight to EMQX, no Beam) check
   registration time, granted eDRX and paging latency. If eDRX is not granted, stop.
2. Point the debug build's `mqtttest` at `beam.soracom.io:1883` with no TLS profile and confirm
   publish, subscribe and the 1200 s keepalive through Beam to EMQX.
3. Only then: add AEAD bodies (option E), a build-time transport switch (`tls` | `beam`), Beam
   credential injection, and rework device SMS to go via Soracom and the relay.

Sources: [Soracom pricing and fee schedule](https://developers.soracom.io/en/docs/reference/fees/),
[Beam MQTT entry point](https://developers.soracom.io/en/docs/beam/mqtt/),
[Beam overview](https://developers.soracom.io/en/docs/beam/),
[Funk overview](https://developers.soracom.io/en/docs/funk/),
[SMS and USSD functionality](https://developers.soracom.io/en/docs/air/sms-ussd/),
[Do Soracom IoT SIM cards support SMS?](https://support.soracom.io/hc/en-us/articles/44639414111129-Do-Soracom-IoT-SIM-Cards-Support-SMS),
[Air SIM session timeout](https://support.soracom.io/hc/en-us/articles/235781348-Will-my-Air-SIM-session-timeout-after-a-certain-amount-of-time).
