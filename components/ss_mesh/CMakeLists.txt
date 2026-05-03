idf_component_register(SRCS "src/ss_mesh.c"
                            "../../mesh_example/main/mesh_netif.c"
                    INCLUDE_DIRS "include" "../../mesh_example/main/include"
                    REQUIRES esp_wifi
                    PRIV_REQUIRES esp_event esp_netif nvs_flash esp_driver_gpio)
