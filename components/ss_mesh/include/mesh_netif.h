#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mesh.h"

typedef void (mesh_raw_recv_cb_t)(mesh_addr_t *from, mesh_data_t *data);

esp_err_t mesh_netifs_init(mesh_raw_recv_cb_t *cb);
esp_err_t mesh_netifs_destroy(void);
esp_err_t mesh_netifs_start(bool is_root);
esp_err_t mesh_netifs_stop(void);
esp_err_t mesh_netif_start_root_ap(bool is_root, uint32_t dns_addr);
uint8_t *mesh_netif_get_station_mac(void);
