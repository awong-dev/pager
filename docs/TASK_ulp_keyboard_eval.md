# TASK_ulp_keyboard_eval: ULP keyboard polling so the CPU can sleep through the typing window (10 Oct 2026)

Read-only evaluation of the owner's question: would moving the CardKB poll to the ULP let the S3
light-sleep until a keypress, cutting the cost of the 120 s window? This is not an implementation.
**Nothing ULP-related gets built without the owner's explicit go-ahead**, and rewiring is the owner's
call. Nothing was measured on the bench. Each number below is either cited or marked *estimate*. Line
numbers are from the tree at 0772bf2.

## Facts the answers rest on

F1. **Window.** Every key re-arms `PAGER_UI_AWAKE_S` 119 s (input.c:67), which sets `skip_sleep`
    (modes.c:2690): the CPU stays fully awake with RTS asserted. The attentive window
    (`PAGER_ATTENTIVE_S` 120, modes.c:169) keeps the CardKB rail on (modes.c:2846). It also sets a
    1 s wake cadence (modes.c:167), but that cadence covers only the last second, after `ui_awake`
    lapses. A shake without a key opens the 15 s hot window instead (input.c:69).
F2. **CardKB.** One unread byte, no FIFO, no IRQ line. The next key overwrites it (GOTCHAS "CardKB
    holds one key"). `kbd` reads one byte every 10 ms at 100 kHz (ui.c:1047, :1076); 0 means no key.
    The CardKB needs 1.1 s after rail-on before it ACKs (ui.c:798-800).
F3. **Bus.** CardKB is alone on I2C_NUM_0, SDA IO10 / SCL IO9 (pins.h). The LIS3DH has its own bus,
    I2C_NUM_1 on IO17/IO18, on the always-on 3V rail. Nothing is shared.
F4. **RTC I2C pins (IDF v5.2.1, `components/ulp/ulp_riscv/ulp_riscv_i2c.c:57-63`).** On ESP32-S3,
    SDA must be **GPIO1 or GPIO3** and SCL must be **GPIO0 or GPIO2**. The pager uses IO10/IO9, so
    **it does not qualify.** IO9/IO10 are RTC GPIOs (RTC_GPIO0-21), so a ULP *bit-banged* I2C master
    could drive them.
F5. **RTC slow memory is nearly full.** `build/school_pager.map`: `rtc_slow_seg` is 8192 B and
    7648 B are used, leaving **544 B free**. 7.1 KB of that is walter-modem's `RTC_DATA_ATTR`
    contexts (`_pdpCtxSetRTC` 3200, `blueCherryRTC` 2320, `_socketCtxSetRTC` 960,
    `_mqttTopicSetRTC` 520). `CONFIG_ULP_COPROC_ENABLED` is unset (sdkconfig:1826), and the reserve
    defaults to 4096 B on S3. The ULP program and its data live in that same segment.
F6. **Light sleep.** No `CONFIG_PM_ENABLE` (manual `esp_light_sleep_start()`, net.cpp:1543).
    `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y`. ext1 is IO16 only. IDF latches a HOLD on ext1 pads,
    so `rtc_gpio_hold_dis` runs after every wake (net.cpp:1552). `esp_sleep_enable_ulp_wakeup()`
    sets `RTC_COCPU_TRIG_EN` with no deep-sleep-only guard (sleep_modes.c:1344-1364), but every IDF
    ULP-RISC-V example is deep-sleep. **ULP wake from light sleep on this build is UNVERIFIED.**
F7. **Cost basis.** The rc1 card today shows ui-awake ≈ 2.8 h/day ≈ 61 mAh/day, implying
    **21.8 mA**, against the 40 mA prior in relay/app/battmodel.py:21. Every result below is given
    at both. Light sleep 0.55 mA (rc1). Rail 5 mA (prior, UNVERIFIED; CardKB ATmega8A plus its
    LED). Total ≈ 300 mAh/day.

## 1. Feasibility

- **Pins.** Hardware RTC I2C needs SDA=IO1 and SCL=IO2 or IO0. IO0 is Walter's 3V3_EN (the CardKB
  rail gate) and is out. IO2 is the display SCK on the Friend's plug-in header. IO3 is strapping and
  never touched. The only legal rewiring is: CardKB SDA→IO1 (left pin 14, free), CardKB SCL→IO2,
  and display SCK moved by jumper to a free GPIO (IO4-7, IO15; the matrix handles 4 MHz). That
  breaks the Friend's plug-straight-on header, and the owner has called the CardKB pins "final".
  **Without rewiring, the ULP has to bit-bang I2C on IO9/IO10.** That is the "ULP bit-banger" the
  rules reserve for owner authorisation. It is feasible: IDF's `ds18b20_onewire` example bit-bangs
  on the ULP, I2C is master-clocked, and the ATmega's slave clock-stretches, so SCL must be read back.
- **Rail.** The ULP is useless unless the CardKB is powered. Holding the rail all day so a key can
  wake the pager from cold costs 5 mA × 24 h = **120 mAh/day (40 %)**, which is a no-go. The ULP
  would therefore run only inside the attentive window, so rail-on time is unchanged and the shake
  stays the cold wake.
- **ULP current.** The ULP-RISC-V draws ~0.2 mA while running (*estimate*, S2/S3 datasheet class
  figure, UNVERIFIED). A 1-byte read takes ~0.4 ms (soft) or ~0.25 ms (RTC I2C), plus the program
  restart. Poll at 10 / 20 / 50 ms ≈ **10 / 5 / 2 µA**. RTC_PERIPH is already powered for ext1.
  Budget ≤ 0.02 mA: noise next to the 0.55 mA floor.
- **Handoff.** The ULP stores the byte in RTC slow memory (a 4-byte ring is plenty), calls
  `ulp_riscv_wakeup_main_processor()` **after the STOP**, then halts. Next key pressed before the
  host takes over: the CardKB holds it (F2) and `kbd` reads it. A key is lost only if two land
  within the takeover gap (a few ms plus the 40 ms input yield, modes.c:202). At human rates that
  never happens, but there is no hard guarantee. Pads must move to the RTC mux before sleep and
  back to the digital mux (`rtc_gpio_deinit` + `kb_reinit_locked()`) after wake. Whether
  `esp_sleep_enable_gpio_switch()` leaves RTC-muxed pads alone is UNVERIFIED.
- **ext1.** COCPU and ext1 wake sources coexist. IO9/IO10 are not in the ext1 mask, so IDF latches
  no hold on them. The code must still release any hold it sets itself.
- **Memory.** The ULP does not fit in 544 B (F5). A minimal poller plus stack is ~1.5-2 KB
  (*estimate*). Making room takes `CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM=y` (~7.5 KB comes off the
  RTC-fast heap) or a vendor patch that drops the modem library's RTC contexts. Both changes reach
  well beyond the keyboard.

## 2. Sleep policy

Sleeping between keystrokes buys almost nothing. A partial refresh holds BUSY ~450 ms, so at 2-3
keys/s the CPU is awake anyway. Each sleep edge also costs `ui_kb_sleep_park()` (bound 100 ms,
ui.c:974), the RTS park/unpark, the 40 ms input yield, and the modem's refusal of commands for
200 ms after RTS re-asserts (§10). Recommended: **typing-idle timeout 4 s** (3-5 s range). That
covers inter-key and short thinking gaps. Each pause then costs ≤ 4 s × 21.8 mA ≈ 0.02 mAh.

First-key latency after a sleep at a 20 ms poll is 10 ms average phase, plus ~1-2 ms light-sleep
exit, plus the 40 ms yield (skippable on a COCPU wake), plus <1 ms driver re-init, then the same
~450 ms render (plus the publish gate, ≤1.5 s). That is about **+35 ms** against today's 10 ms
poll, which is not perceptible.

**Hidden cost: inbound latency in a conversation.** Today RTS stays asserted for the whole window,
so a reply shows as soon as the modem has it. A sleeping window parks RTS, and a reply then waits
for the 20 s cadence plus the probe answer: **+10-25 s** on top of eDRX. The cadence has to be
20 s, not 1 s, because a 200 ms wake delivers nothing (§10.3). This applies to every option that
sleeps inside the window, including the cheap one below.

## 3. Savings (per 30 s burst over a 150 s horizon; ×40 = owner's model, ×68 = rc1's 2.8 h)

| Option | Saves/day at 21.8 mA (×40 / ×68) | at 40 mA | Keyboard after a 30 s pause |
|---|---|---|---|
| ULP, 4 s idle, 120 s rail window | 27 / 45 mAh (9 / 15 %) | 50 / 84 | live, +35 ms |
| Main-CPU 50 ms timer-wake poll, 4 s idle (no ULP) | 25 / 42 (8 / 14 %) | 48 / 81 | live, +25 ms |
| **Cheap: `PAGER_UI_AWAKE_S` 30 and `PAGER_ATTENTIVE_S` 30** | **26 / 44 (9 / 15 %)** | 43 / 74 | dead; shake (~1.5 s) |
| Both windows 60 s | 17 / 29 (6 / 10 %) | 29 / 49 | dead after 60 s |
| UI 30 s, attentive 120 s at 1 s cadence | 16 / 27 (5 / 9 %) | 29 / 50 | ≤1 s, drops keys (F2) |

All rows include their own extra cost: ULP 0.02 mA, CPU poll ~1.6 mA (2 ms × 40 mA per 50 ms,
*estimate*), and 20 s timer wakes at 0.35 s. The ULP beats the no-ULP timer poll by only ~2 mAh/day.
The cheap row comes close to the ULP because it also cuts 90 s of rail per burst.

## 4. Risks

- **Rules and memory.** A bit-banger needs owner authorisation, rewiring is the owner's call (F4),
  and RTC slow memory overflows (F5).
- **Driver ownership.** The legacy driver on I2C_NUM_0 versus RTC-mux pads: every sleep needs
  delete/reinit, and the ULP must never wake the host mid-transaction. Today's park mutex covers
  only the `kbd` task.
- **Build.** A `ulp/` subproject, `ulp_embed_binary` and Kconfig changes; host tests cannot see it.
- **Watchdog.** The TWDT watches the loop task only (watchdog.c:19). Short sleeps are fine
  (`WD_SLEEP_ENTER`). A ULP that hangs fails silent rather than resetting: keys stop and nothing
  notices. It needs a loop-side "ULP alive" counter.
- **Bench.** Every in-window sleep drops USB-Serial-JTAG, so console `key` injection and live logs
  die mid-session. `PAGER_DEBUG_NO_LIGHT_SLEEP` builds cannot exercise the feature, which makes it
  release-image-plus-ammeter work only.
- **UX.** +10-25 s on replies inside a conversation (§2), for every sleeping option.

## 5. Recommendation: no-go on the ULP; do the cheap thing first

1. Measure, no flash: in `battstat`/the card, check the per-session ui seconds. Count rc1 sessions
   per day and how long each is (is 2.8 h made of ~68 sessions with a 119 s tail?). Measure the
   CardKB rail current with a meter in series with 3V3-OUT (1 min). If that current is ≫ 5 mA, the
   rail term dominates and the case for the cheap option gets stronger.
2. Flash the cheap change (two constants, owner picks 30 s or 60 s). Bench proof on the release
   image: the 1 h soak shows `aw.ui` per session ≈ typing + 30 s, `rl` drops the same way, and the
   `abcdefgh` burst passes inside the window. Then the owner judges the shake-to-resume on the glass.
3. **Trigger to revisit the ULP:** the owner rejects shake-to-resume, AND after step 2 ui+rail still
   exceed 10 % of the daily mAh. If both hold, build the main-CPU timer-wake poll first (no ULP,
   no rewiring, within ~2 mAh/day of the ULP). Bring the ULP back only if that poll's measured
   in-window floor is > 1.5 mA above the 0.55 mA sleep floor. The first ULP experiment
   (authorisation needed) is a 20-line ULP program that counts its own runs in RTC memory and
   wakes the host every 100 runs from **light** sleep, which settles F6.

## Summary for the owner

Not with the ULP, at least not yet. Its I2C only works on GPIO0-3. Two of those are taken by the
keyboard rail switch and the display clock, so using it means either rewiring or having the ULP
bit-bang I2C on the keyboard pins, which needs your approval. It also has no memory to run from: the
modem library has left only 544 B of the RTC memory free. Shrinking the 120 s window to 30 s saves
about as much, ~27 mAh/day (9 %) on 40 chats a day and ~45 mAh/day if today's 2.8 h holds, because
the keyboard rail also switches off sooner. The price is a shake to wake the keyboard after 30 s of
no typing. Any option that sleeps mid-conversation also delays incoming replies by 10-25 s.
