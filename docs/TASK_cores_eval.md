# TASK_cores_eval: network workers and UI on CPU1 (9 Oct 2026)

Read-only design evaluation for two owner questions. Assumptions: the `kbd` task is floating
(`tskNO_AFFINITY`, ui.c:1162); the two survey items from `docs/TASK_kbtask.md` land (kbd task wakes
the main loop early via `input_wait_event()`, modes.c:3083; wake status refresh deferred until after
the first draw, modes.c:3281-3294). Nothing here was bench-measured; every number is marked estimate
or cites the log it came from. Line numbers are from the tree as of 9 Oct 2026 17:05.

## Facts both answers rest on

F1. **Task map today.** Main loop = the IDF main task, CPU0, prio 1 (sdkconfig:1031, main.c:2594).
    Modem library: `uart_rx_task` prio 3, `queueProcessingTask` prio 2, `eventProcessingTask` prio 4,
    all pinned CPU0 (WalterModem.cpp:5159-5183). `kbd` prio 2 floating. Debug console REPL prio 2,
    unpinned (esp_console.h:65-66, `xTaskCreate`). esp_timer task and ISR on CPU0 (sdkconfig:1093-1096).
    CPU1 runs its idle task, IPC task, and the kbd task when it lands there.
F2. **The modem library is multi-caller tolerant, not thread-safe.** Every public call is synchronous:
    it allocates from an 8-slot pool in `_cmdPoolGet()` with no lock (WalterModem.cpp:1073-1083, an
    unlocked check-then-set), posts to `_taskQueue`, and blocks on a per-command
    `std::condition_variable` (WalterDefines.h:391-405). One `_curCmd` slot serialises the wire
    (WalterModem.cpp:1778-1855). It is already called from three tasks: the loop, the event task
    (xport_lte.cpp:414, :485) and the console. A second core widens the pool race from "preemption
    window" to true concurrency.
F3. **Sleep is safe today by construction.** `skip_sleep` (modes.c:2681-2683) ORs
    `net_modem_busy()` (= `s_handler_busy`, xport_lte.cpp:323/941), connect/publish in flight, resub
    hold. Every other AT call is issued from the loop task itself, so none can be mid-flight when the
    same task reaches `net_sleep()`. A command in flight across a light sleep is the patch 1.19 wedge
    class (GOTCHAS "A finished command left in the modem library's slot"): the tick freezes
    (net.cpp:1611-1618) and the wake bytes draw a stray response.
F4. **Rendering is gated on publish quiet regardless of who publishes.** `disp_pre_write_gate_hook()`
    (ui.c:1113-1134, disp.c:776/910) waits up to 1500 ms for `publish_quiet_gate_should_wait()`
    (xport_lte.cpp:737-754). Moving a publish to another task does not let the panel draw sooner.
F5. **Static DRAM is full.** `esp_idf_size` on `build/school_pager.map` (debug build, 9 Oct 17:05):
    D/IRAM 343,351 B used, **2,505 B remain**. That is why the kbd stack already comes from the heap
    (ui.c:1160-1162). Boot heap: `free=2195356` (final-release-boot.log), which includes the 2 MiB
    PSRAM, so about 98 KB internal free (estimate). A 12 KB internal worker stack is affordable;
    a PSRAM stack is not usable for anything that writes flash (NVS, OTA).
F6. **One task owns the task watchdog.** watchdog.c:186-193 subscribes `s_loop_task` only;
    `watchdog_feed()` resets the TWDT only from that task (:214), and vendor patch 1.10's in-wait
    feed goes through the same check. `/status` key 59 `stallcmd` names the loop's stage.
F7. **Affinity mechanics in IDF 5.2 (dual-core, non-SMP).** UART/I2C/SPI ISRs bind to the core that
    installs the driver (uart.c:1646, i2c.c:383, spi_master.c:213/250); freeing from the other core
    goes through IPC (intr_alloc.c:704-705). `esp_light_sleep_start()` stalls the other core by IPC
    ISR from whichever core calls it (sleep_modes.c:933/1132; `CONFIG_ESP_IPC_ISR_ENABLE=y`). The TWDT
    watches both idle tasks (sdkconfig:1051-1052). An unpinned task that touches the FPU is pinned
    on first use (port.c:66). Flash writes park the other core in `spi_flash_op_block_func` via IPC
    (cache_utils.c:184-206): neither core escapes an NVS or OTA write stall.

## Q1. Network workers, floating

**Recommendation: don't now. Do later only on a measured trigger (below).** Pin, do not float, if it
is ever built.

Facts that drive it:
1. After the two landing items, the AT traffic that can still stall the loop while someone is
   typing is: a reply or ack publish (hundreds of ms, 30 s timeout), which F4 makes panel-blocking
   anyway; the liveness re-SUBSCRIBE every 300 s of uplink silence (about 130 ms when awake,
   GOTCHAS "Pages stop arriving"); `net_session_up()` on a reconnect (two round trips, rare).
   `run_modem_health_check()` with its `vTaskDelay(1000)` (modes.c:2067) runs in sleep mode only
   (modes.c:3590-3594), never during a typing session. loc/sms/catrust/bookpull/ota are one bounded
   step per pass. So the worker's realistic win is about 130 ms once per 5 min of typing, plus
   responsiveness while the modem is wedged (5 s VMON/CSQ timeouts, 30 s publish, 300 s attach wait
   in `net_init()`/recovery, README R14).
2. F3 and F6: a worker needs a new sleep interlock and a second watchdog subscriber. The interlock
   has a race (worker dequeues and transmits between the loop's `skip_sleep` evaluation and
   `esp_light_sleep_start()`) that must be closed with a mutex held across the sleep, the same shape
   as `ui_kb_sleep_park()`. Every bench-found contract in net.cpp/xport_lte.cpp ("called from
   modes_run()'s own task, never from an event callback", xport_lte.cpp:757-760; `msg_pump()` never
   concurrent with `msg_mark_read()`, README R10) is a single-task invariant.
3. F7: floating buys nothing here. CPU1 is idle apart from the kbd task (about 100 x 0.3 ms per s,
   3% of a core, estimate). A worker pinned to CPU1 is guaranteed a core; a floating one may land on
   CPU0 and, at prio >= 2, preempt the prio-1 loop. Nothing in IDF breaks with a floating worker
   (no Wi-Fi/BT; esp_timer stays on CPU0; sleep stalls either core), but a float-using worker
   silently pins itself, and a worker that busy-loops on CPU1 trips the idle-task TWDT.

Power: a correctly blocking worker adds no mA. The risk is the opposite: a worker that holds
`skip_sleep` true turns 5 s sleep cycles into awake time at about 40 mA (README M2 assumption);
one missed cycle per wake would double sleep-mode current (1.8-2.1 mA estimated today, M1).

Cheaper step first (survey item 4): add the `svc=` bucket to the debug `looptime` line and, while
`input_awake()`, defer the deferrable steps (loc/catrust/bookpull/ota/liveness). One `if`, no task.

Trigger to build the worker: a `looptime` capture during typing showing `svc` p95 > 300 ms from
AT calls that are not already panel-gated (publish is), more than once per typing session; or an
owner requirement that the UI stay live through modem recovery (R14's 300 s attach wait).

Concrete design if the trigger fires (hand to firmware-dev as a spec):
- One task `net`, pinned CPU0, prio 1 (never above the modem's own tasks), 12 KB stack from
  internal heap, TWDT-subscribed. watchdog.c generalises `s_loop_task` to a two-entry set and
  gives the worker its own stage breadcrumb (a second `/status` key, schema change, needs the owner).
- Interface: a 4-deep queue of `{PUBLISH(topic, buf, len, qos), SESSION_UP, SERVICE_TICK,
  STATUS_REFRESH}`; completions posted back as `input.c`-style events so the loop's
  `input_wait_event()` wakes. The loop keeps the modes FSM, rendering, input, and all RTC state;
  no RTC fields move. Wake sources unchanged (timer + ext1).
- Sleep: `net_worker_idle()` ORed into `skip_sleep`; `s_net_mutex` taken by the worker around each
  AT call and by `net_sleep()` across `esp_light_sleep_start()` (100 ms bound, log on timeout,
  sleep anyway: the kbd park pattern, net.cpp:1529-1531).
- Vendor patch 1.23: a portMUX around `_cmdPoolGet()` (F2). Recorded in PATCHES.md.
- Failure modes: worker stuck in a 30 s publish keeps the loop awake (same cost as today, about
  0.33 mAh per event); a worker crash reboots via TWDT with its own breadcrumb; a lost completion
  event must time out in the loop (reuse the connect watchdog shape, modes.c:3463-3470).
- Measurement to justify: key-to-echo p95 before/after via console `key` injection in scenarios
  A (idle chat) and C (within 2 s of a send) from `firmware/README.md` "Keyboard task"; sleep-mode
  average current unchanged within 0.2 mA (M1); `rsp_stale_cmd`, `stallcmd` resets, `MEMORY_FULL`
  all zero over a 1 h soak with light sleep on. Two flashes, no A/B flag.

## Q2. UI on CPU1

**Recommendation: don't.** Neither a UI task on CPU1 nor moving the main loop there.

Facts that drive it:
1. The loop's stalls are blocking waits, not CPU contention: e-ink BUSY (disp.c:390-455,
   `vTaskDelay` polling; 450 ms partial, 1.4-3.4 s full), the publish gate (F4), and synchronous AT
   round trips. CPU0's other load is tiny: the modem event task polls at 100 Hz
   (WalterModem.cpp:1866-1871, about 0.2% of a core, estimate), the RX task wakes per UART burst,
   the queue task only during a command. The debug `looptime` line (modes.c:3269) already splits
   paint/refresh/status; contention would show as unexplained `rest`, and the 9 Oct capture
   (`status=43`, `refresh=565`) shows none.
2. Nothing on the host is bit-timed. The panel runs its waveform from the host-uploaded LUT
   inside the SSD1680 (disp.c:974-980; TP0A is byte 60 of the LUT, disp.c:241-248). The host does
   4 MHz polling SPI transmits with no DMA (disp.c:480/492, :1259-1271) and no microsecond delays
   (no `esp_rom_delay_us` in disp.c). Core placement cannot change a pixel.
3. CPU1 does not dodge the stalls people hope it would: flash writes park both cores (F7), and
   light sleep halts both with `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` (sdkconfig:920,
   sleep_modes.c:840 CPU retention). No `CONFIG_PM_ENABLE`, so both cores clock at 160 MHz and
   CPU1 is never gated differently while awake.
4. The sleep path is the one thing that must stay reliable. `esp_light_sleep_start()` is called
   from the loop on CPU0 (net.cpp:1531). IDF supports calling it from either core (F7), but doing
   so from CPU1 is UNVERIFIED on this build, and every wake-byte and RTS finding (patches 1.18,
   1.19, M5) was measured with the sleeping task on CPU0. That is a soak-sized change for no
   measured gain.
5. The kbd task already shares CPU1 (when it lands there) at a 10 ms cadence; a UI task next to it
   would not conflict, but a split UI task re-opens README R4 (ui.c has no cross-task mutex; disp.c
   has one, disp.c:316-329, ui.c's framebuffer and screen stack do not) and the rail/gate ownership
   that survey item 3 rejected.

Power: none either way while asleep (both cores off); while awake an idle CPU1 sits in WFI, so
moving work there changes awake current by well under 0.5 mA (estimate). Not a reason to do it.

Trigger to revisit: a `looptime` capture with `period` > 150 ms while paint, refresh, status and
svc are all small (unexplained `rest`), reproducible. If it ever fires, the change is one Kconfig
line, `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1=y` (no code), followed by `sleeptest` and a 1 h light-sleep
soak with the page-delivery check from `docs/SLEEP_URC_DESIGN.md`. Do not split a UI task.

## Note: the floating 10 ms I2C poller

No downside forces pinning the kbd task. The legacy I2C driver's ISR binds to the core that calls
`i2c_driver_install()` (ui.c:880 at init on CPU0; `kb_reinit_locked()` from the kbd task on whichever
core it is on). The ISR-to-task handoff is a FreeRTOS queue, cross-core safe. An `i2c_driver_delete()`
from the other core frees the ISR through IPC (F7): works, costs one IPC round trip (tens of
microseconds, estimate) on the 1280 B IPC task stack (sdkconfig:1079), which is enough for that
callback. The sleep park is a mutex (ui.c:985-990, net.cpp:1529-1531), independent of core; the
stall at `esp_light_sleep_start()` reaches whichever core the task is on. The task uses no float, so
the FPU auto-pin (F7) cannot surprise it.

Soft reasons to pin CPU1 anyway: on CPU0 the task preempts the prio-1 loop for about 0.3 ms every
10 ms (up to 3% of CPU0, estimate; a render's polling SPI write is sliced but the panel latches each
byte, harmless); the I2C ISR's core becomes nondeterministic after a re-init; `kbd: task started
float (core N)` (ui.c:1016) names only the starting core, so bench logs cannot say where a read ran.
Soft reason to float: none in practice, since CPU1 is idle. Verdict: not worth a flash on its own.
Whichever variant is on the glass when the `abcdefgh` burst test passes stays; if the file is
re-flashed for another reason, pin to CPU1 (the TASK_kbtask spec) for determinism. No A/B flag.

## Summary for the owner

Neither change is recommended now. Moving the network work to worker tasks would not make the pager
feel faster: the display is already held back during a publish by the register-loss gate, so a
publish on another task still blocks the draw, and once the two deferrals land the only AT traffic
left to stall a typing session is a 130 ms re-subscribe about every five minutes. The cost is real:
the modem library is tolerant of several callers but has an unlocked command pool, the light-sleep
interlock would need a new mutex held across sleep, and the watchdog assumes one loop task, and all
of that touches the path the weeks-on-a-charge target depends on. Static DRAM is also full (2.5 KB
left at link), so any worker stack comes from the roughly 98 KB of internal heap. Floating workers
would gain nothing because CPU1 is idle anyway. Putting the UI on CPU1 does not help either: the
loop's stalls are waits on the panel, the publish gate and modem round trips, not CPU contention; the
e-ink waveform runs inside the controller, so core placement cannot affect the image; and flash writes
and light sleep stop both cores. The cheap step is to measure modem servicing in the `looptime` line
and defer the non-urgent steps while someone is typing; revisit workers only if that capture shows
non-publish stalls over 300 ms per session, and revisit CPU1 only if an unexplained loop period over
150 ms shows up. The floating keyboard task is fine as is; pin it to CPU1 only if it is reflashed for
another reason.
