idf_component_register(SRCS "net.cpp" "xport_lte.cpp" "xport_wifi.c" "wifi_sta.c" "net_connect_guard.c" "net_probe_guard.c" "resub_verdict.c" "publish_quiet.c" "ui.c" "scr_home.c" "scr_chat.c" "scr_device.c" "scr_setup.c" "scr_lock.c" "scr_pick.c" "scr_book.c" "scr_greeting.c" "disp.c" "disp_flip.c" "gfx.c" "input.c" "modes.c" "msg.c" "ident.c" "carrier.c" "watchdog.c" "cbor.c" "auth.c" "setup.c" "lock.c" "book.c" "loc.c" "accel.c" "cfg.c" "catrust.c" "cafetch.c" "otapipe.c" "ota.c" "bookpull.c" "sms.c" "coverage.c" "wifi_policy.c" "wificred.c" "flightrec.c" "rail.c" "battstat.c" "clockfmt.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash mbedtls console esp_partition esp_hw_support app_update esp_app_format esp_rom detools dptechnics__walter-modem esp_wifi esp_netif esp_event esp-tls mqtt)

# Debug only: `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build` makes
# modes_run() never light-sleep, so RTS stays asserted and the USB serial log
# stays alive. Stands in for the 30 s UI-awake window while the button and
# keyboard are not wired up. Never ship a build made this way: it burns the
# battery at the awake current. Unset the variable and reconfigure to undo.
if(DEFINED ENV{PAGER_DEBUG_NO_LIGHT_SLEEP})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAGER_DEBUG_NO_LIGHT_SLEEP=1)
    message(WARNING "PAGER_DEBUG_NO_LIGHT_SLEEP is set: this build never light-sleeps")
endif()

# Bench only: `PAGER_OTA_TEST_ABORT=1 idf.py reconfigure build` makes app_main() abort()
# right after the boot log, to prove the bootloader rolls an OTA image back. Never ship it.
if(DEFINED ENV{PAGER_OTA_TEST_ABORT})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAGER_OTA_TEST_ABORT=1)
    message(WARNING "PAGER_OTA_TEST_ABORT is set: this build panics at boot on purpose")
endif()
