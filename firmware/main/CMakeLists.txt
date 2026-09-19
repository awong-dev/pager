idf_component_register(SRCS "net.cpp" "ui.c" "scr_home.c" "scr_chat.c" "scr_device.c" "scr_setup.c" "scr_lock.c" "scr_pick.c" "scr_book.c" "scr_greeting.c" "disp.c" "gfx.c" "input.c" "modes.c" "msg.c" "ident.c" "cbor.c" "auth.c" "setup.c" "lock.c" "book.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash mbedtls console esp_partition esp_hw_support)
