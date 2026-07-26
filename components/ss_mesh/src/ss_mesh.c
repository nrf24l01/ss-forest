#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "mesh_netif.h"
#include "ss_mesh.h"
#include "ss_mesh_tree.h"

static const char *TAG = "ss_mesh";
static const uint8_t SS_MESH_ID[6] = { 0x53, 0x53, 0x46, 0x4f, 0x52, 0x01 };

static mesh_addr_t s_parent_addr;
static int s_layer = -1;
static bool s_connected;

static void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    mesh_addr_t id = { 0 };
    static uint8_t last_layer;

    switch (event_id) {
    case MESH_EVENT_STARTED:
        esp_mesh_get_id(&id);
        s_layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "mesh started, id=" MACSTR, MAC2STR(id.addr));
        break;
    case MESH_EVENT_STOPPED:
        s_layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "mesh stopped");
        break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        s_layer = connected->self_layer;
        memcpy(s_parent_addr.addr, connected->connected.bssid, sizeof(s_parent_addr.addr));
        ESP_LOGI(TAG, "parent connected, layer %d -> %d, parent=" MACSTR "%s",
                 last_layer, s_layer, MAC2STR(s_parent_addr.addr), esp_mesh_is_root() ? ", root" : "");
        last_layer = s_layer;
        s_connected = true;
        ESP_ERROR_CHECK(mesh_netifs_start(esp_mesh_is_root()));
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
        s_layer = esp_mesh_get_layer();
        s_connected = false;
        ESP_LOGI(TAG, "parent disconnected");
        mesh_netifs_stop();
        break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        s_layer = layer_change->new_layer;
        ESP_LOGI(TAG, "layer changed %d -> %d%s", last_layer, s_layer,
                 esp_mesh_is_root() ? ", root" : "");
        last_layer = s_layer;
        break;
    }
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
        ss_mesh_tree_child_connected(child->mac);
        ESP_LOGI(TAG, "child connected aid=%d mac=" MACSTR, child->aid, MAC2STR(child->mac));
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child = (mesh_event_child_disconnected_t *)event_data;
        ss_mesh_tree_child_disconnected(child->mac);
        ESP_LOGI(TAG, "child disconnected aid=%d mac=" MACSTR, child->aid, MAC2STR(child->mac));
        break;
    }
    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGI(TAG, "routing table changed delta=%d size=%d", routing->rt_size_change, routing->rt_size_new);
        break;
    }
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(TAG, "root address=" MACSTR, MAC2STR(root->addr));
        break;
    }
    default:
        ESP_LOGD(TAG, "mesh event id=%" PRId32, event_id);
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
#if !CONFIG_MESH_USE_GLOBAL_DNS_IP
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(mesh_netif_start_root_ap(esp_mesh_is_root(), dns.ip.u_addr.ip4.addr));
#endif
}

esp_err_t ss_mesh_init(ss_mesh_recv_cb_t *recv_cb, bool fixed_root)
{
    ESP_RETURN_ON_ERROR(ss_mesh_tree_init(), TAG, "init mesh tree");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "init nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");
    ESP_RETURN_ON_ERROR(mesh_netifs_init(recv_cb), TAG, "init mesh netifs");

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "init wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL), TAG, "register ip event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "set wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable wifi ps");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");

    ESP_RETURN_ON_ERROR(esp_mesh_init(), TAG, "init mesh");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, mesh_event_handler, NULL), TAG, "register mesh event");
    ESP_RETURN_ON_ERROR(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER), TAG, "set max layer");
    if (fixed_root) {
        ESP_RETURN_ON_ERROR(esp_mesh_set_vote_percentage(1), TAG, "set vote percentage");
    }
    ESP_RETURN_ON_ERROR(esp_mesh_set_ap_assoc_expire(10), TAG, "set ap expire");
    ESP_RETURN_ON_ERROR(esp_mesh_send_block_time(30000), TAG, "set send block time");
    ESP_RETURN_ON_ERROR(esp_mesh_fix_root(fixed_root), TAG, "set fixed root");
    if (fixed_root) {
        ESP_RETURN_ON_ERROR(esp_mesh_set_type(MESH_ROOT), TAG, "set root type");
    }

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
#if !CONFIG_MESH_IE_ENCRYPTED
    cfg.crypto_funcs = NULL;
#endif
    memcpy((uint8_t *)&cfg.mesh_id, SS_MESH_ID, sizeof(SS_MESH_ID));
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *)&cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *)&cfg.router.password, CONFIG_MESH_ROUTER_PASSWD, strlen(CONFIG_MESH_ROUTER_PASSWD));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *)&cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD, strlen(CONFIG_MESH_AP_PASSWD));

    ESP_RETURN_ON_ERROR(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE), TAG, "set ap auth");
    ESP_RETURN_ON_ERROR(esp_mesh_set_config(&cfg), TAG, "set mesh config");
    ESP_RETURN_ON_ERROR(esp_mesh_start(), TAG, "start mesh");
    ESP_LOGI(TAG, "mesh start ok, heap=%" PRIu32 ", fixed_root=%d", esp_get_free_heap_size(), fixed_root);
    return ESP_OK;
}

esp_err_t ss_mesh_send_to_root(const void *payload, size_t len)
{
    mesh_data_t data = {
        .data = (uint8_t *)payload,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    return esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
}

esp_err_t ss_mesh_send_to_node(const uint8_t node_mac[6], const void *payload, size_t len)
{
    mesh_data_t data = {
        .data = (uint8_t *)payload,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    for (int attempt = 0; attempt < 3; attempt++) {
        int route_table_capacity = esp_mesh_get_routing_table_size();
        int route_table_size = 0;
        if (route_table_capacity <= 0) {
            return ESP_ERR_NOT_FOUND;
        }

        mesh_addr_t *route_table = calloc((size_t)route_table_capacity, sizeof(*route_table));
        if (route_table == NULL) {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = esp_mesh_get_routing_table(route_table, route_table_capacity * sizeof(*route_table),
                                                   &route_table_size);
        if (err != ESP_OK) {
            free(route_table);
            return err;
        }

        if (route_table_size > route_table_capacity) {
            free(route_table);
            continue;
        }

        for (int i = 0; i < route_table_size; i++) {
            if (memcmp(route_table[i].addr, node_mac, 6) == 0) {
                err = esp_mesh_send(&route_table[i], &data, MESH_DATA_P2P, NULL, 0);
                free(route_table);
                return err;
            }
        }

        free(route_table);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_ERR_NOT_FOUND;
}

const uint8_t *ss_mesh_get_station_mac(void)
{
    return mesh_netif_get_station_mac();
}

bool ss_mesh_is_connected(void)
{
    return s_connected;
}
