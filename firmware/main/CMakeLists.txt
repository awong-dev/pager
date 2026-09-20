idf_component_register(SRCS "net.cpp" "ui.c" "scr_home.c" "scr_chat.c" "scr_device.c" "scr_setup.c" "scr_lock.c" "scr_pick.c" "scr_book.c" "scr_greeting.c" "disp.c" "gfx.c" "input.c" "modes.c" "msg.c" "ident.c" "cbor.c" "auth.c" "setup.c" "lock.c" "book.c" "loc.c" "accel.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash mbedtls console esp_partition esp_hw_support dptechnics__walter-modem)

# Debug only: `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build` makes
# modes_run() never light-sleep, so RTS stays asserted and the USB serial log
# stays alive. Stands in for the 30 s UI-awake window while the button and
# keyboard are not wired up. Never ship a build made this way: it burns the
# battery at the awake current. Unset the variable and reconfigure to undo.
if(DEFINED ENV{PAGER_DEBUG_NO_LIGHT_SLEEP})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAGER_DEBUG_NO_LIGHT_SLEEP=1)
    message(WARNING "PAGER_DEBUG_NO_LIGHT_SLEEP is set: this build never light-sleeps")
endif()
