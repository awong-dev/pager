idf_component_register(SRCS "net.cpp" "ui.c" "scr_home.c" "scr_chat.c" "scr_device.c" "scr_setup.c" "scr_lock.c" "disp.c" "gfx.c" "input.c" "modes.c" "msg.c" "ident.c" "cbor.c" "auth.c" "setup.c" "lock.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer nvs_flash mbedtls console esp_partition)
