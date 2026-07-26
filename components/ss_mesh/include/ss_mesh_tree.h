#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SS_MESH_TREE_MAC_LEN 6

typedef struct {
    uint8_t mac[SS_MESH_TREE_MAC_LEN];
    uint8_t parent_mac[SS_MESH_TREE_MAC_LEN];
    bool parent_known;
    bool direct_child;
} ss_mesh_tree_node_t;

typedef struct {
    uint16_t count;
    bool complete;
    ss_mesh_tree_node_t nodes[CONFIG_MESH_ROUTE_TABLE_SIZE];
} ss_mesh_tree_t;

esp_err_t ss_mesh_tree_init(void);
void ss_mesh_tree_child_connected(const uint8_t mac[SS_MESH_TREE_MAC_LEN]);
void ss_mesh_tree_child_disconnected(const uint8_t mac[SS_MESH_TREE_MAC_LEN]);
esp_err_t ss_mesh_tree_build(ss_mesh_tree_t *tree);

#ifdef __cplusplus
}
#endif
