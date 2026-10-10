# Status (9 Oct 2026)

Built 9 Oct 2026. Where this status and the spec body below disagree, this status wins.

- **Review follow-ups applied:** the light-sleep park wraps only `esp_light_sleep_start()` (not
  `net_sleep()`, as Do step 7 says); `i2cscan swap` re-inits only with the rail on; the task stack
  is 4096 B (the spec says 3072 B).
- **Known bench-only race, left as is:** console I2C driver use is guarded by the pause flag, not
  the mutex.
- **Bench status (unit sora1, 9 Oct 2026):** the new build boots. Loop period is unchanged (about
  100 ms). The task pauses and resumes around console probes within 20 ms. No crash or watchdog
  in 150 s.
- **OPEN: CardKB burst verification (Verify step 2) is pending.** The CardKB on sora1 does not
  answer on I2C. `i2cscan` finds 0 devices, the lines idle high, and this is true on both the
  baseline and the new build. `kbtime` reports "never acked". Real-keystroke results need a working
  keyboard and the owner to type `abcdefgh` fast three times in each scenario. See
  `docs/GOTCHAS.md` ("sora1's CardKB does not answer at 0x5F") and `firmware/README.md`
  ("Keyboard task").
- **Survey outcome (Part 2):** do item 1 (move the wake status refresh after the first draw) and
  the follow-up in item 5 (the kbd task wakes the main loop). Measure modem servicing (item 4)
  later. Do not touch the publish-quiet gate (item 2), and do not move the e-ink refresh to a task
  (item 3). Items 1 and 5 are implemented (10 Oct 2026, see below); the rest are not, see `docs/ROADMAP.md`.

---

# TASK_kbtask: CardKB scan on its own FreeRTOS task (9 Oct 2026)

Owner decisions (9 Oct 2026): the CardKB scan moves off the main loop onto its own task that feeds
input.c's queue. **The task is the only read path.** Main-loop polling and the refresh-time poll
hook get deleted, not disabled. There is no A/B flag (owner: "do not keep the code with keyboard
polling in the mainloop"). Verification compares two flashes: the current debug build, then the new build.

Why: the CardKB holds one unread key. Any main-loop stall longer than one keystroke gap loses keys.
The loop stalls in the wake status refresh (43 ms measured, 2x5000 ms worst case), the
publish-quiet gate (560 to 710 ms seen, 1500 ms cap), and modem servicing.

## Key decisions
- **Location:** `ui.c`, next to the existing CardKB code (ui.c:776-981). All the I2C state is
  already static there, and the raw-key `ESP_LOGD` keeps tag `ui`, which main.c:2575 already raises
  to DEBUG in debug builds. No new file.
- **Task:** `"kbd"`, static stack 3072 B, priority 2 (main loop is 1), **floating** (`tskNO_AFFINITY`, owner change 9 Oct 2026; first spec pinned it to CPU1). In the
  IDF build all walter-modem tasks are pinned to CPU0 (WalterModem.cpp:5159-5182: rx prio 3, queue
  2, event 4), so CPU1 is mostly idle; the scheduler may place `kbd` on either core.
- **Period:** CONFIG_FREERTOS_HZ=100, so 15 ms cannot be expressed. Use **1 tick = 10 ms**.
  Power: about 100 reads/s x about 0.3 ms at 100 kHz, plus a core waking from WFI. Estimated
  < 0.1 mA, and only while the rail is on and the chip is awake. At 30 min of awake time per day
  that is about 0.05 mAh/day. CONFIG_PM_ENABLE is off, so the CPU clock does not change either way.
- **Rail state:** notification, not polling. rail.c calls a new `ui_kb_rail_changed()` as the
  **last** line of `rail_on()` and `rail_off()`. That point is after `s_rail_on`/`s_restored_us`
  are final. Do not notify from `ui_kb_bus_restore()`: rail.c:163 runs before rail.c:165-169
  publish the new state. While the rail is off the task blocks in `ulTaskNotifyTake(pdTRUE,
  portMAX_DELAY)`. During the 1300 ms boot guard it waits one tick at a time and re-checks against
  `esp_timer_get_time()`. A long tick timeout is wrong here because the tick count does not advance
  across a manual light sleep (net.cpp:1611). Guard length (`PAGER_KB_BOOT_GUARD_MS`) is unchanged.
- **Bus ownership:** a new static mutex `s_kb_bus_mutex` in ui.c. The task holds it for exactly one
  read (plus the once-per-edge re-init). `ui_kb_bus_release()`, `ui_kb_bus_restore()` and
  `ui_kb_i2c_reinit()` take it too. The task checks `!s_kb_poll_paused && s_i2c_installed &&
  rail_is_on()` *after* taking the mutex. This closes the race where `rail_off()` deletes the driver
  while `s_rail_on` is still true (rail.c:183 vs :196).
- **Console pause is synchronous:** `ui_debug_pause_kb_poll(true)` sets the flag, then takes and
  gives the mutex once, so any in-flight read has finished before it returns. `(false)` clears the
  flag and notifies the task. `kbtime` already calls it (main.c:700/758). **`i2cscan` does not pause
  today**, so it needs a pause too.
- **Light sleep:** the task holds no wake lock (there are none with PM off), and esp_light_sleep_start()
  halts both CPUs regardless of ready tasks, so it cannot block sleep. The real hazard is the
  opposite: an attentive-window sleep (rail stays on) starting in the middle of an I2C
  transaction. With a 0.3 ms read every 10 ms that is about 3% of sleep entries. It would show up as
  a read failure that uses up the once-per-edge re-init. Fix: modes.c parks the task around
  `net_sleep()` (takes the mutex, 100 ms bound, then sleeps anyway with one log line on timeout).
  Across sleep the task is frozen. On an ext1 wake with the rail already on, it polls within one
  tick, so the key the CardKB held during sleep is read in about 10 ms. If the rail was off, the
  `rail_on()` notification starts the guard and polling resumes 1300 ms later, same as today.
- **Attentive/awake-window logic stays in modes.c/input.c.** The task does not gate on
  `input_awake()`. It polls whenever the rail is on and past the guard, which matches today's
  every-pass poll (modes.c:3141 is ungated).
- **Queue depth 8 -> 32** (input.c:69, +288 B RAM). A 1.5 s gate + 0.57 s refresh at about 8
  keys/s is about 17 keys, so 8 would turn "key lost at the CardKB" into "key lost at the queue".
  The drop log input.c:105 goes from ESP_LOGD to ESP_LOGW so a drop is visible on the bench.
- **Cross-task 64-bit reads:** `s_awake_until_us`/`s_hot_until_us` (input.c) are now written on
  the kbd task's core and read on the main loop's, and `s_restored_us` (rail.c) is read by the kbd task. Xtensa int64 accesses can
  tear, so wrap them in a portMUX.

## Read
firmware/main/ui.c:776-1025, ui.h:454-524, rail.c:140-200, input.c:60-110 and 280-305,
modes.c:2852-2866 and 3110-3145, disp.c:370-460, disp.h:88-102, main.c:678-760 and 885-950.

## Files
firmware/main/ui.c, ui.h, rail.c, input.c, modes.c, disp.c, disp.h, main.c (plus comment-only
touch-ups in accel.h:148/160 and rail.h:104).

## Do
1. **ui.c:** add `static SemaphoreHandle_t s_kb_bus_mutex` (xSemaphoreCreateMutexStatic) and a
   static task (xTaskCreateStaticPinnedToCore(kbd_task, "kbd", 3072, NULL, 2, ..., tskNO_AFFINITY)). Create both
   at the end of `ui_init()` after `i2c_kb_init()` (ui.c:1090). ui_init's idempotency guard already
   covers setup_run()'s second call. Add `static TaskHandle_t s_kbd_task`.
2. **ui.c:** replace `ui_poll_keyboard()` (ui.c:909-981) with `static void kbd_task(void *)`. It
   reuses the body's read/fail/re-init/`input_feed_key` code unchanged. The raw-key `ESP_LOGD(TAG,
   "CardKB: 0x%02x", byte)` (ui.c:968) must stay and must run before `input_feed_key()`.
   Loop shape (load-bearing):
   ```c
   for (;;) {
       if (s_kb_poll_paused || !rail_is_on()) {
           log_state_once(paused ? "paused (console)" : "paused (rail off)");
           ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue;
       }
       int64_t r = rail_restored_us();
       if (r != s_kb_guard_restored_us) { s_kb_guard_restored_us = r; s_kb_reinit_done_this_restore = false; }
       if (r != 0 && esp_timer_get_time() - r < PAGER_KB_BOOT_GUARD_MS * 1000LL) {
           log_state_once("boot guard"); /* s_kb_skipped_reads++ once per edge */
           ulTaskNotifyTake(pdTRUE, 1); continue;
       }
       log_state_once("polling");
       xSemaphoreTake(s_kb_bus_mutex, portMAX_DELAY);
       if (!s_kb_poll_paused && s_i2c_installed && rail_is_on()) { /* existing read + re-init via kb_reinit_locked() */ }
       xSemaphoreGive(s_kb_bus_mutex);
       if (byte) { ESP_LOGD(...); input_feed_key(byte); }
       ulTaskNotifyTake(pdTRUE, 1); /* 10 ms, wakes early on rail/pause change */
   }
   ```
   `log_state_once()` prints `ESP_LOGI(TAG, "kbd: %s (rail on +%lld ms)")` only when the state string
   changes. Add one start line, `kbd: task started float (core N) prio2 period 10ms`. That gives one INFO line
   per rail-gated pause or resume, and none for the per-sleep park.
3. **ui.c:** move the driver delete+install into `static void kb_reinit_locked(void)`. Public
   `ui_kb_i2c_reinit()`, `ui_kb_bus_release()` and `ui_kb_bus_restore()` take/give `s_kb_bus_mutex`
   around their bodies. Use a non-recursive mutex, so nothing calls a public one while holding it.
   `ui_debug_pause_kb_poll()` works as described in Key decisions. Add `void ui_kb_rail_changed(void)`
   (xTaskNotifyGive when the handle is non-NULL) and `void ui_kb_sleep_park(void)` /
   `ui_kb_sleep_unpark(void)` (take with a 100 ms bound, remembering whether it succeeded / give if
   taken). Redefine `s_kb_skipped_reads` as "rail-restore edges whose guard the task waited out",
   and update its comment at modes.c:1702.
4. **Delete** the strong `disp_busy_idle_hook()` (ui.c:984-1025). **Delete** the weak hook
   (disp.c:374-377), its two calls (disp.c:430, :457) and its declaration (disp.h:88-102). Leave
   the 10 ms fallback-step loop in disp.c as it is (it still works, no restyle). If grep then shows
   no callers of `modes_on_run_task()`, delete it too (modes.c:2457-2466, modes.h:32).
5. **Delete** the `ui_poll_keyboard()` call and its comment block at modes.c:3117-3141, and
   `ui_poll_keyboard()`'s declaration and doc in ui.h:454-472. Update the ui.h docs of the remaining
   kb functions so they mention the task. Fix the comments that name `ui_poll_keyboard()`: modes.c:76,
   :461, :1704; main.c:691, :723, :734, :1321; accel.h:148, :160; rail.h:104. Comment-only edits.
6. **rail.c:** in `rail_on()`, set `s_restored_us` inside the existing `s_on_mux` critical section
   (rail.c:165-169), then call `ui_kb_rail_changed()` last. In `rail_off()`, call it last (after
   rail.c:198). `rail_restored_us()` reads under `s_on_mux`.
7. **modes.c:2863-2865:** `ui_kb_sleep_park();` just before `net_sleep(interval_ms);`, and
   `ui_kb_sleep_unpark();` right after it returns.
8. **main.c `cmd_i2cscan`** (main.c:887): when `!accel`, call `ui_debug_pause_kb_poll(true)` before
   the scan. After it, if `swap`, call `ui_kb_i2c_reinit()` to restore the real pins, then
   `ui_debug_pause_kb_poll(false)`.
9. **input.c:** `INPUT_QUEUE_DEPTH` 8 -> 32 (:69). Change the queue-full log to ESP_LOGW (:105).
   Put a `static portMUX_TYPE s_win_mux` around writes and reads of `s_awake_until_us` and
   `s_hot_until_us` (arm_awake_window, input_feed_key, input_note_shake_wake, input_awake, input_hot).

## Verify
Build with the IDF env (memory: idf-build-python-env). Log into the scratchpad. Bench rules: kill
stale bench processes, keep one long-lived serial reader, use short captures.
0. **Save the baseline first.** The new build overwrites `firmware/build/`. Copy
   `firmware/build/{flash_args,bootloader/bootloader.bin,school_pager.bin,partition_table/partition-table.bin,ota_data_initial.bin}`
   (school_pager.bin from 9 Oct 10:15) into `firmware/build-baseline/` with the same relative paths.
   Flash it from there with `esptool.py write_flash @flash_args`.
1. `idf.py build` must be warning-clean for the touched files. `grep -rn ui_poll_keyboard\|disp_busy_idle_hook firmware/main`
   must return nothing.
2. On each flash (baseline, then new), the owner types `abcdefgh` fast in three scenarios. Capture
   the `ui: CardKB: 0x..` DEBUG lines (0x61-0x68):
   A. screen awake, idle chat;
   B. right after a shake: start typing at the first draw (keys during the 1.3 s boot guard are
      lost on both builds, because the CardKB is still booting);
   C. within 2 s of sending a message (publish + gate).
   Expected: new build gets 8/8 in A, B and C, with zero `input event queue full` lines. Baseline
   drops bytes in B and C (about first + last).
3. New build boot shows `kbd: task started`. After about 120 s idle, `kbd: paused (rail off)`.
   After a shake, `kbd: boot guard` then `kbd: polling` at about +1300 ms.
4. `kbtime 3` reports ACKs (about 1136 ms, matching round9r-kbtime4). `i2cscan` lists 0x5F.
   `i2cscan swap` runs. Typing works after each of them (the task resumed).
5. A 10 min attentive-window session (a key every ~60 s keeps 1 s sleeps cycling) shows no
   `3 consecutive I2C failures` or `re-initializing the driver` lines. That confirms the sleep park.
6. If a current trace is available, the awake-floor delta vs baseline should be < 0.2 mA.
   Otherwise skip this step (memory: no battery tracking).

## Other gaps (survey, 9 Oct 2026)
Once the task is in, keys stop getting lost in any of these stalls. What remains is render latency.
Assessed with the single-loop preference in mind:

1. **Wake status refresh** (modes.c:752, called on the awake edge before the first render). **IMPLEMENTED 10 Oct 2026:** the awake edge sets `s_wake_status_pending` and `modes_run()` runs the refresh right after that pass's `ui_render()` (debug log `wake status refresh after first draw: N ms`; the `status=` field is gone from `looptime`).
   Measured `status=43` ms (TASK_looptime capture). Worst case is 2 x 5000 ms
   (PAGER_VMON/CSQ_TIMEOUT_MS, net.cpp:1897-1899) when the AT queue is busy (connect in flight,
   publish). Effect: the first key's echo waits for two modem round trips. This breaks the "UI
   first" rule. Smaller fix: **deferral**. Render with the cached batt/rssi, and run the refresh on
   the next pass after `ui_render()` (the status bar repaints on the next partial). No task.
   **Do** (one small move in modes.c).
2. **Publish-quiet gate** (ui.c disp_pre_write_gate_hook). 560 ms (HARDWARE_TESTING.md:197) and
   710 ms (TASK_uifirst) seen, 1500 ms cap. It mitigates real panel corruption, and a task split
   cannot help because the panel write itself must wait. With the kbd task, keys queue and land in
   one partial afterwards. **Don't.** Revisit the cap only with S10/S11 data showing publishes
   never stay in flight that long.
3. **E-ink busy wait** (disp_wait_busy_fb, `refresh=565` ms measured). It already yields
   (vTaskDelay) and only holds the main loop. The loop drains all queued events, then renders once
   (modes.c event loop then `ui_render()`), so a burst becomes one partial. A render task would
   spread disp/SPI, publish gate and rail ownership across tasks: large coupling, little gain.
   **Don't.**
4. **Modem servicing** (net_session_up, msg_pump publish, run_modem_health_check, loc/sms/catrust/
   bookpull/ota). The last five are documented as one non-blocking step per call. Publish and
   session/health calls are synchronous AT round trips (hundreds of ms typical, timeouts in seconds).
   No per-call timings exist. Smaller fix: **measure first**. Add a `svc=` bucket to the debug
   `looptime` line, covering everything from `net_get_mqtt_status` through `ota_service`. If any
   call goes over 300 ms during `input_awake()`, defer that non-urgent step while the UI is awake
   (catrust/bookpull/ota/loc are all deferrable). Do not split. **Later**, right after this task lands.
5. **100 ms awake cadence** (**IMPLEMENTED 10 Oct 2026:** `input_wait_event(100)` replaces the `vTaskDelay`; `input.c`'s `push_event()` notifies the `modes_run()` task after each enqueue, and `net_sleep()` skips the sleep, returning false, if `input_pending()` after the kbd park.) (modes.c:3087 `vTaskDelay(100)`). It adds 0-100 ms (mean 50) to every
   key echo. With the kbd task, replace it with `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))`, and
   have the task `xTaskNotifyGive(s_modes_run_task)` after `input_feed_key()`. Coupling is one
   handle. grep finds no other task-notification users (NOTIFICATION_ARRAY_ENTRIES=1). Power: none,
   the wake is early but not more frequent. **Do, as a follow-up**, measured separately so Part 1's
   before/after stays clean.
6. **Shake classifier** (accel_poll at 20 ms) is unaffected and needs no change.

Net: one new task (keyboard). Everything else is a deferral or a measurement, and the main loop
stays single.
