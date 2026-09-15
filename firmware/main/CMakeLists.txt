idf_component_register(SRCS "net.cpp" "ui.c" "disp.c" "gfx.c" "input.c" "modes.c" "msg.c" "ident.c" "cbor.c" "auth.c" "setup.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES json driver esp_timer nvs_flash mbedtls console esp_partition)
