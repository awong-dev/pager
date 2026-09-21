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
  than a pager that falls back to no validation. (`V02_DESIGN.md` §4 would be dead code on a
  Soracom SIM.)
- **Credentials can leave the pager too.** Beam can inject the broker username/password keyed on the
  IMSI, so the setup bundle shrinks to the device id and the HMAC key.

What changes in the security model:

- Confidentiality past the radio rests on the carrier, the interconnect and Soracom. Today it
  rests on TLS to EMQX, and EMQX itself already sees everything in clear. So the honest delta is
  "three more parties can read pages and location fixes".
- The **HMAC signature and counter become the only integrity protection** between the pager and
  Soracom. Keep them.
- The clean fix is `DEVICE_PLAN.md` §2.2's option E: **AEAD-encrypt bodies and location fixes under
  the device key**, pager↔relay. About 28 bytes per message. Then Soracom, the carrier *and* EMQX
  all see ciphertext, which is better confidentiality than today's design, with no certificates on
  the pager at all. **This should be a condition of moving to Soracom.**
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

## Without a broker at all

EMQX is in the Beam design above only because Beam's **MQTT** entry point is a proxy, not a
broker: something behind it has to hold the pager's subscription and push a page down the open
connection. That is the least-change option. Soracom also has the two pieces needed to drop the
broker entirely:

- **Uplink: Beam's UDP→HTTPS (or HTTP→HTTPS) entry point.** The pager sends one datagram to Beam;
  Beam POSTs it to an HTTPS URL of ours, i.e. **straight to the relay on Cloud Run**, adding the
  SIM's IMSI and a signature header proving it came through Soracom, and returns the HTTP
  response to the pager as the reply datagram. The relay's ingest is already an HTTP endpoint fed
  by a webhook, so this is close to a drop-in for `/webhooks/mqtt`. (Beam, not Funk: Funk targets
  Lambda/Cloud Functions with their IAM; the relay is a plain HTTPS service.)
- **Downlink: Remote Command, `sendDownlinkUdp`.** The relay calls Soracom's API
  (`POST /v1/sims/{simId}/downlink/udp`) and Soracom delivers a UDP datagram to the pager over its
  private network. No VPG needed, addressed by SIM id so the pager's IP can change. It is
  **fire-and-forget**: no delivery confirmation and no device response comes back.

What that buys: no broker, no MQTT, no TCP, no TLS, **no keepalive and no reconnects at all**. The
radio is used only for real traffic, which is the best possible case for both the data budget
(roughly 0.2-0.3 MB/month nominal) and the battery. The old reason for insisting on the modem's
built-in MQTT client was that it sends keepalives without waking the ESP32; with UDP there is no
keepalive to send, so that reason goes away.

What it costs:

- **Reliability moves into our protocol.** UDP can drop, duplicate and reorder. The pieces already
  exist (ids, dedup, `shown`/`read` acks, the relay re-publishing unacked messages) but the relay's
  retry is tied to the pager's online edge today; it would need a timer (retry an unacked page
  after N seconds, back off, give up) because there is no connection whose loss signals anything.
- **No presence.** No broker means no connect/disconnect edge and no Last Will. "Online" becomes
  "sent a heartbeat recently", plus what Soracom's session API reports for the SIM.
- **The idle session.** Soracom ends a data session after about an hour idle. The hourly status
  heartbeat already in the design keeps it up; if it drops, downlinks fail until the pager next
  sends something.
- **The same unverified sleep question as today** (`PROTOCOL.md` §8.3, M5), in a new form: the modem
  must hold an open UDP socket, be paged for an incoming datagram during eDRX, and raise a ring
  that wakes the ESP32. Never tested on this modem.
- **A rewrite, not a port.** Firmware transport (`net.cpp`: UDP socket, send, retries, no MQTT
  engine), relay transport (Soracom API client with token refresh instead of the broker publish;
  a Beam endpoint instead of the webhook; topics become a field or a URL path), the test client
  and the local docker stack, which would need a fake Soracom.
- **Total lock-in.** Today's design runs on any SIM and any MQTT broker. This one runs only on
  Soracom, in firmware and relay both.
- `UNVERIFIED`: Remote Command's price and rate limits (not on the pages read), its behaviour when
  the pager is in an eDRX sleep, and datagram size limits (ours are ≤ 640 bytes).

Security is as in the Beam section, and the same condition applies: AEAD for bodies and fixes,
because everything is plaintext through Soracom. The HMAC stays.

**Verdict.** Architecturally this is the best fit a pager could ask for, and it would delete more
code than it adds. It is also the riskiest change on the table: it discards a transport that was
proven end to end, for one whose key behaviour (a paged UDP downlink waking a
sleeping pager) nobody has seen work. Treat it as the long-term direction to *test towards*, not a
switch to make now. The trial below is ordered so each step is cheap and can end the experiment.

## Funk or Beam for the uplink

"Funk cannot push" is true, and it is equally true of Beam's UDP entry point: in a broker-free
design **the downlink is Remote Command either way**, and the only question is which service
carries the *uplink*. For that job Funk over UDP is sensible, and on authentication it is the
better of the two.

How Funk authenticates, which is not quite what one would assume:

- For **AWS Lambda and Azure Functions**, Funk holds real cloud credentials in Soracom's credential
  store and invokes the function through the provider's IAM. That is true server-to-server IAM.
- For **Google Cloud, it does not use Google IAM.** Soracom's configuration guide says the
  credentials setting "is not required" for Google Cloud Functions: Funk simply POSTs to the
  function's HTTPS URL. The function therefore has to allow unauthenticated invocation at the IAM
  level, and authenticates the caller itself from the **`X-Soracom-Token` header: a JWT signed by
  Soracom** whose `ctx` carries the IMSI, SIM id, operator id and source protocol.
- That is still a clear improvement on `WEBHOOK_KEY`. It is asymmetric (we verify with Soracom's
  public key and hold no secret that can leak or pick up a stray newline), it is per request, and
  it binds each message to a SIM identity that the cellular network authenticated. Beam's
  UDP→HTTPS entry point offers only a header signed with a pre-shared key, i.e. the same kind of
  shared secret we have today.
- So the layering the owner describes is right: Soracom's signed token says *which SIM* sent this
  and that it came through Soracom; our HMAC and counter say *which pager* wrote it and that it is
  fresh; AEAD (if added) keeps Soracom from reading it.

Funk versus Beam for the uplink, on Google Cloud:

| | Funk (UDP) | Beam (UDP→HTTPS) |
|---|---|---|
| Caller authentication | Soracom-signed JWT with SIM context | header signed with a pre-shared key |
| Secrets we must hold | none (a public key) | one shared key |
| Reply to the pager | status code, optionally the function's response | HTTP status and body |
| Price | first 50,000/month free, then $0.18 per 10,000 | first 100,000/month free, then $0.09 per 10,000 |
| At our ~6,000 requests/month | free | free |
| Target | documented as a `cloudfunctions.net` URL | any HTTPS URL |

`UNVERIFIED`: whether Funk accepts a Cloud Run (`run.app`) URL as the Google target, since the
relay is a Cloud Run service and not a Cloud Function. If it does not, a ten-line Cloud Function
that verifies the token and forwards to the relay closes the gap (and *that* hop can use Google
IAM properly). Also unverified: payload and response size limits on the UDP entry point, and how
binary payloads are wrapped (the options are JSON, text or binary; our envelopes are CBOR).

**For a broker-free design, use Funk over UDP for the uplink.** What argues against switching now
has nothing to do with Funk. It is everything in the previous section:
the downlink (`sendDownlinkUdp` waking a sleeping pager) has never been seen to work, reliability
and presence move into our own protocol, device SMS to a phone is impossible on a Soracom SIM,
the rewrite is large, and the lock-in is total. Those are reasons to run the trial first, not
reasons to avoid the destination.

## Recommendation

Soracom is worth a trial, and the broker-free shape (Funk over UDP up, Remote Command down) is
the one to aim for, with "Beam's MQTT entry point in front of EMQX" as the low-risk fallback if
the downlink experiment fails. Order of work if pursued:

1. Buy one plan-US SIM. With the **existing** firmware (TLS straight to EMQX, no Beam) check
   registration time, granted eDRX and paging latency. If eDRX is not granted, stop.
2. Point the debug build's `mqtttest` at `beam.soracom.io:1883` with no TLS profile and confirm
   publish, subscribe and the 1200 s keepalive through Beam to EMQX.
3. Broker-free probe: open a UDP socket on the modem, put the pager in its normal eDRX sleep, and
   call `sendDownlinkUdp` from a laptop. Measure delivery rate, latency against the eDRX cycle,
   and whether the ESP32 wakes. Send a datagram to Funk's UDP entry point, verify the
   `X-Soracom-Token` at the receiving end, and read the reply on the pager. This one experiment decides between "Beam in front of EMQX" and "no broker".
4. Only then: add AEAD bodies (option E), a build-time transport switch, and rework device SMS to
   go via Soracom and the relay. If step 3 passed, the transport is `udp`; if not, `beam-mqtt`
   with EMQX kept behind it.

Sources: [Soracom pricing and fee schedule](https://developers.soracom.io/en/docs/reference/fees/),
[Beam MQTT entry point](https://developers.soracom.io/en/docs/beam/mqtt/),
[Beam overview](https://developers.soracom.io/en/docs/beam/),
[Funk overview](https://developers.soracom.io/en/docs/funk/),
[Funk configuration](https://developers.soracom.io/en/docs/funk/configuration/),
[Beam UDP→HTTPS entry point](https://developers.soracom.io/en/docs/beam/udp-http/),
[Downlink API](https://developers.soracom.io/en/docs/air/downlink-api/),
[Remote Command UDP usage](https://developers.soracom.io/en/docs/remote-command/udp-usage/),
[SMS and USSD functionality](https://developers.soracom.io/en/docs/air/sms-ussd/),
[Do Soracom IoT SIM cards support SMS?](https://support.soracom.io/hc/en-us/articles/44639414111129-Do-Soracom-IoT-SIM-Cards-Support-SMS),
[Air SIM session timeout](https://support.soracom.io/hc/en-us/articles/235781348-Will-my-Air-SIM-session-timeout-after-a-certain-amount-of-time).
