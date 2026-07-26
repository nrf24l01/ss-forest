idf_component_register(SRCS "src/ss_mesh.c"
                             "src/ss_mesh_tree.c"
                            "src/mesh_netif.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash
                    PRIV_REQUIRES lwip)
