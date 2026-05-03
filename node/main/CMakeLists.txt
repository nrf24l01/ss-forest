idf_component_register(SRCS "node_main.c"
                    PRIV_REQUIRES ss_mesh bt esp_driver_gpio led_strip
                    INCLUDE_DIRS ".")
