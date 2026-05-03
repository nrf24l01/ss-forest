#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (ss_mesh_recv_cb_t)(mesh_addr_t *from, mesh_data_t *data);

esp_err_t ss_mesh_init(ss_mesh_recv_cb_t *recv_cb, bool fixed_root);
esp_err_t ss_mesh_send_to_root(const void *payload, size_t len);
esp_err_t ss_mesh_send_to_node(const uint8_t node_mac[6], const void *payload, size_t len);
const uint8_t *ss_mesh_get_station_mac(void);
bool ss_mesh_is_connected(void);

#ifdef __cplusplus
}
#endif
