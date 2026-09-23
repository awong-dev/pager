# RCA 2: URCs lost / held at the light-sleep boundary

Follows `docs/RCA_SLEEP_PUBLISH.md` (patches 1.11/1.12, which this run shows did NOT fire).
Evidence: `build/bench-logs/phase1-savedreport.log`, `phase1-sleeptest.log`, `phase1-boot.log`,
`phase1-relay-raw{,2}.json`, DEBUG build `2fbecf6`, `sleeptest 6` (360 s window, 2000 ms cycle —
the pager was in ACTIVE mode, `modes.c:1777-1779`). **No device-side log of the window exists**
(USB dies at the first light sleep), so everything below is reconstructed from the NVS report.

## 1. The awake-time budget closes exactly

| bucket | avg/wake | max | total (×128 sleeps) |
|---|---|---|---|
| post-wake yield (`ST_MARK(0)`, `modes.c:1980`) | 313 ms | 198 ms | 40.0 s |
| input+ui+render (`ST_MARK(1)`, `modes.c:2128`) | 28 ms | **3501 ms** | 3.6 s |
| mqtt status/retry (`ST_MARK(2)`, `modes.c:2369`) | 235 ms | **30014 ms** | 30.1 s |
| msg_pump+health (`ST_MARK(3)`, `modes.c:2413`) | 234 ms | **30012 ms** | 30.0 s |
| accel+loc / sms / catrust (`ST_MARK(4..6)`) | 0 | 0 | 0 |

40.0 + 3.6 + 30.1 + 30.0 = 103.7 s ≈ 360 − 256 = 104 s awake. So the window contained **exactly
two 30-second events and one 3.5 s event; every other wake was ~200 ms of yield and nothing else**
(`cycle N: ... awake before it 197 ms`). Both 30 s figures are
`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS = 30000` (`firmware/sdkconfig:1966`): two **blocking library
commands got no terminal response from the modem for 30 s each**. The 3501 ms is one full
e-paper refresh — rca1 being rendered.

`rc=0` on the loss is load-bearing. Both host-side "session dead" verdicts
(`xport_lte.cpp:564-573` connect watchdog, `:580-600` no-SUBACK-in-30 s) set `s_last_class` but
never `s_last_rc`, which is still its initial `0` (`xport_lte.cpp:88`). **The session was killed
by our own liveness logic, not by the broker or the network.**

## 2. Timeline

| t | event | evidence |
|---|---|---|
| −79 s | last uplink: `/up` 44 B OK, 23 ms | publish ring |
| +61 s | relay publishes rca1 `m_303734af` | task brief |
| ~+66 s | modem receives it (eDRX `0010` = 20.48 s, PTW `0001`, `AT+CPSMS=0` — `phase1-boot.log:104,109`) and emits `+SQNSMQTTONMESSAGE`. **Not delivered to the host for 186 s.** No eDRX/PSM figure explains 182 s; this is a host-interface problem, not a radio one |
| +189 s | relay publishes rca2 `m_fd97c926` | task brief |
| ~+221 s | a blocking command is issued and gets no answer for 30 s. Two candidates, both in bucket 3: the idle liveness re-SUBSCRIBE (`xport_lte.cpp:618`, due at −79+300 = +221, `sendCmd()` is blocking via `_returnAfterReply()`, `WalterModem.cpp:4937`) or `run_modem_health_check()`'s bare `AT` (`modes.c:2404`, due every 60 wake cycles ≈ 132 s). **Undetermined — see §5** |
| ~+251 s | that command times out. **Its 30 s is the only contiguous RTS-asserted window in the whole run** |
| +252 s | rca1 arrives — 1 s before that window closes. It was held all along and came out the moment the host stayed awake for seconds instead of milliseconds |
| +252..256 s | full refresh, 3501 ms (bucket 1 max) |
| +258 s | `sleeptest_note('L', rc=0)` (`modes.c:2258`): host-side verdict, session down, `lte_session_down()` → `mqttDisconnect()` |
| +258.. | bucket 2's 30014 ms: `handle_mqtt_loss()` / `net_session_up()` blocking on a modem that is still not answering |
| — | no ack for rca1 is ever attempted: `msg_pump()` runs only while `st.mqtt_connected` (`modes.c:2386`), false from +251 |
| — | rca2 is gone for good: the teardown drops the modem's MQTT client, and V02_DESIGN §9.1 item 4 measured that this broker keeps **no** persistent session — a queued QoS 1 `/down` is discarded. Nothing at the relay for either id, consistent |

## 3. Mechanism: RTS is not actually held high across the sleep

`net_sleep()` (`net.cpp:1148-1150`) disables flow control, makes RTS a plain GPIO output and drives
it high, then sleeps. **The pad hardware undoes the last step.** `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y`
(`firmware/sdkconfig:825`) makes IDF run, at system init,
`esp-idf/components/esp_hw_support/sleep_gpio.c:187-199` → `esp_sleep_config_gpio_isolate()` +
`esp_sleep_enable_gpio_switch(true)`: every valid GPIO gets `SLP_OE=0 / SLP_IE=0 / no pull` in its
IO_MUX **sleep** fields and its `SLP_SEL` bit set. From then on the pad switches to *output driver
off, floating* for the whole of `esp_light_sleep_start()` and back on wake, in hardware, with no
software involved. It is visible in every boot log: `sleep: Configure to isolate all GPIO pins in
sleep state` / `Enable automatic switching of GPIO sleep configuration` (`phase1-report.log`, the
two lines after `spi_flash:`).

So for ~2000 ms of every 2200 ms cycle the modem's CTS input sat on a board pull nobody in this
repo controls, and `PROTOCOL.md` §8.3 / M5's open question ("does the Sequans queue or drop while
CTS is deasserted") was never even the right question — the first question is whether the modem saw
CTS deasserted at all. rca1 arriving intact 182 s late says the line floated to the *deasserted*
level on this board most of the time (the modem held rather than dumped), which is luck, not
design, and is exactly the kind of marginal behaviour that produces the 35 s / 136 s / 182 s tail
recorded in `docs/GOTCHAS.md` ("Pages never arrive while the pager sleeps").

The vendor's own `WalterModem::sleep(_, true)` (`WalterModem.cpp:5054-5097`) has the identical
defect; `net.cpp` inherited it by replicating the choreography.

## 4. What can be lost at the boundary — and what the counters already exclude

- **(a) Bytes the modem already put on the wire when RTS goes high.** The driver RX ring (256 B,
  `uart_driver_install(uartNo, UART_BUF_SIZE*2, ...)`, `WalterModem.cpp:4866`) is plain DRAM and
  survives light sleep; the hardware RX FIFO is in a domain the ESP32-S3 does not power down
  (`SOC_PM_SUPPORT_TOP_PD` is undefined for S3, so `light_sleep_uart_prepare()`,
  `sleep_modes.c:504-521`, takes the `suspend_uarts()` branch). What is lost is only what is still
  **in flight on the wire**: `net.cpp:1148-1182` never drains the ring, never waits for line idle,
  and never yields to the RX task, so 0-3 characters that the modem emitted between the RTS write
  and the clock gating land in an unclocked UART. **NOT excluded by any counter.** This is the
  "residue" that `RCA_SLEEP_PUBLISH.md` §2 already blames for the split `"> "` prompt.
- **(b) The ESP's own TX FIFO. EXCLUDED on this target.** `esp_light_sleep_start()` →
  `suspend_uarts()` (`sleep_modes.c:455-474`) forces XOFF on every enabled UART and spins until the
  TX FSM is IDLE/TX_WAIT_SEND; `resume_uarts()` (`:478-487`) forces XON after. The FIFO content is
  retained. `uart_wait_tx_done()` before sleep is **not** needed. (Separate latent defect, not in
  play here: `_uartWrite()` does `uart_wait_tx_done(_uartNo, pdMS_TO_TICKS(10))` and ignores the
  return, `WalterModem.cpp:965-966` — 10 ms at 115200 is ~115 bytes, so a 122-byte `/status`
  payload can return unflushed.)
- **(c) The modem dropping a URC after its own CTS timeout. STILL OPEN**, and now compounded by
  §3: we do not know what CTS level it saw. rca2's loss does **not** evidence a drop — it is fully
  explained by the +258 s teardown plus V02_DESIGN §9.1 item 4.
- **(d) `_uartRxTask` / parser state across sleep. Not a loss source by itself.** The task
  (`WalterModem.cpp:1572-1608`, priority 3 core 0, against `modes_run()`'s priority 1) is a plain
  FreeRTOS task blocked in `uart_read_bytes(..., 30 s)`; `_parserData` is plain RAM. But it holds a
  *partial line* across a sleep, so if (a) eats the bytes in between, the next wake's bytes are
  appended to a stale head: one corrupted line, one lost URC. `buf_drop_queue=0`, `buf_drop_pool=0`
  and `prompt_orphan=0` exclude the pool/queue drops and the specific `"> "` orphan; **they do not
  count a stranded or corrupted ordinary line.**

## 5. Fix order, each with a discriminator in the existing `sleeptest` report

1. **Hold RTS through the sleep — IMPLEMENTED.** `gpio_sleep_sel_dis(RTS)` before
   `esp_light_sleep_start()` (`net.cpp:1152-1180`). One IO_MUX register write, no power cost (the
   pad drives high into a CMOS input either way). *Discriminator:* the 35/136/182 s delivery tail
   disappears — a page lands within one wake cycle. Quantitatively, add **bytes read from the modem
   UART in the first 50 ms after each wake** to the report: near-zero per wake today, a burst on the
   wake after a page once this holds.
2. **Drain before sleeping.** After raising RTS, wait for the line to be idle for ~2 ms and let the
   RX task consume the ring into the parser before `esp_light_sleep_start()` (a `vTaskDelay(2)` is
   enough at priority 1 vs 3). Costs 2 ms per 2200 ms = 0.09% duty ≈ 0.03 mA ≈ 0.8 mAh/day.
   *Discriminator:* a **"bytes still buffered in the UART RX ring at sleep entry"** counter
   (`uart_get_buffered_data_len()` immediately before the sleep) — non-zero today, must become 0.
3. **Do not light-sleep within N ms of a command that expects a URC.** `net_modem_busy()` was meant
   to be this but only covers the event task's `mqttReceive()` (`xport_lte.cpp:336`). *Discriminator:*
   a **SUBACK-missed** counter and a **`+CEREG`/`AT` poll timeout** counter; both must go to 0.
4. **Split the two 30 s stalls apart.** Record, per blocking modem call, which command was
   outstanding when the stall began. Two lines of instrumentation, and it is what §2's `~+221 s`
   row needs to stop being two candidates.
5. **Make the "session dead" verdict cheaper to be wrong about.** The 30 s no-SUBACK bound
   (`xport_lte.cpp:580`) cost this run both pages. Not worth changing until 1-4 land: if the SUBACK
   was simply held with everything else, 1 fixes it.

**Can a 200 ms wake window see a page URC at all?** With RTS genuinely held (fix 1), the modem has
the whole window and 200 ms at 115200 is 2300 byte-times — yes, provided the Sequans' resume
latency after CTS assertion is well under 200 ms. That latency is **UNMEASURED**. If fix 1 does not
close the tail, it is the next thing to measure, and the design needs the modem to wake the host
instead. **There is no modem→ESP wake line available**: `firmware/main/pins.h` defines none, the
vendored library exposes only RX/TX/RTS/CTS/RESET (`Kconfig:148-176`) and has no `RING`/wake
support. The only host-wake path on this silicon is `esp_sleep_enable_uart_wakeup()` on UART1 with
RTS left **asserted** through the sleep — which loses the leading characters of the waking line,
i.e. exactly the `\r\n` the parser delimits on. Cheapest experiment before committing to that: hold
RTS asserted for one `sleeptest` window with light sleep on and see whether URCs arrive corrupted
or not at all.

## 6. Also fixed here

`sleeptest 6 0 0` — the form `RCA_SLEEP_PUBLISH.md` §3 documents — was rejected by the console.
`0` now means "build default" for both overrides (`main.c:196-214`), matching what
`modes_debug_sleeptest_start()` already did with a zero (`modes.c:1958`, `:1790`). Docs corrected
in `RCA_SLEEP_PUBLISH.md` §3 and `HARDWARE_TESTING.md`.
