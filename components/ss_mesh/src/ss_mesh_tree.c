#include <string.h>

#include "esp_check.h"
#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ss_mesh.h"
#include "ss_mesh_tree.h"

static SemaphoreHandle_t s_tree_lock;
static uint8_t s_children[CONFIG_MESH_AP_CONNECTIONS][SS_MESH_TREE_MAC_LEN];
static bool s_child_present[CONFIG_MESH_AP_CONNECTIONS];

static int find_child(const uint8_t mac[SS_MESH_TREE_MAC_LEN])
{
    for (int i = 0; i < CONFIG_MESH_AP_CONNECTIONS; i++) {
        if (s_child_present[i] && memcmp(s_children[i], mac, SS_MESH_TREE_MAC_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t ss_mesh_tree_init(void)
{
    if (s_tree_lock != NULL) {
        return ESP_OK;
    }
    s_tree_lock = xSemaphoreCreateMutex();
    return s_tree_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

void ss_mesh_tree_child_connected(const uint8_t mac[SS_MESH_TREE_MAC_LEN])
{
    if (s_tree_lock == NULL || mac == NULL || xSemaphoreTake(s_tree_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (find_child(mac) < 0) {
        for (int i = 0; i < CONFIG_MESH_AP_CONNECTIONS; i++) {
            if (!s_child_present[i]) {
                memcpy(s_children[i], mac, SS_MESH_TREE_MAC_LEN);
                s_child_present[i] = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_tree_lock);
}

void ss_mesh_tree_child_disconnected(const uint8_t mac[SS_MESH_TREE_MAC_LEN])
{
    if (s_tree_lock == NULL || mac == NULL || xSemaphoreTake(s_tree_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    int child = find_child(mac);
    if (child >= 0) {
        s_child_present[child] = false;
    }
    xSemaphoreGive(s_tree_lock);
}

esp_err_t ss_mesh_tree_build(ss_mesh_tree_t *tree)
{
    ESP_RETURN_ON_FALSE(tree != NULL, ESP_ERR_INVALID_ARG, "ss_tree", "tree is null");
    ESP_RETURN_ON_FALSE(s_tree_lock != NULL, ESP_ERR_INVALID_STATE, "ss_tree", "tree is not initialized");

    mesh_addr_t routes[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_count = 0;
    ESP_RETURN_ON_ERROR(esp_mesh_get_routing_table(routes, sizeof(routes), &route_count), "ss_tree", "get routes");

    memset(tree, 0, sizeof(*tree));
    tree->complete = route_count <= CONFIG_MESH_ROUTE_TABLE_SIZE;
    if (route_count > CONFIG_MESH_ROUTE_TABLE_SIZE) {
        route_count = CONFIG_MESH_ROUTE_TABLE_SIZE;
    }

    uint8_t root_mac[SS_MESH_TREE_MAC_LEN] = { 0 };
    if (esp_mesh_is_root()) {
        memcpy(root_mac, ss_mesh_get_station_mac(), sizeof(root_mac));
    }

    if (xSemaphoreTake(s_tree_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < route_count; i++) {
        if (esp_mesh_is_root() && memcmp(routes[i].addr, root_mac, sizeof(root_mac)) == 0) {
            continue;
        }
        ss_mesh_tree_node_t *node = &tree->nodes[tree->count++];
        memcpy(node->mac, routes[i].addr, sizeof(node->mac));
        memcpy(node->parent_mac, root_mac, sizeof(node->parent_mac));
        node->direct_child = find_child(routes[i].addr) >= 0;
        node->parent_known = node->direct_child;
    }
    xSemaphoreGive(s_tree_lock);
    return ESP_OK;
}
