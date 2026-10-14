# ../../../../firmware/main is read-only here: only pins.h and placeholder_ca.h are included from it.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "." "../../../../firmware/main"
                       REQUIRES driver esp_timer esp_hw_support dptechnics__walter-modem)
