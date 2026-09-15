idf_component_register(SRCS "net.cpp" "ui.c" "modes.c" "msg.c" "ident.c" "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES json driver esp_timer nvs_flash)
